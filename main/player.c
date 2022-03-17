#include "main.h"
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s.h"
#include "FLAC/stream_decoder.h"
#include "opus.h"
#include "time_model.h"
#include <string.h>

static const char *TAG = "app_player";

// The maximum allowable frame size, which should be part of the server config.  Note that opus
// limits this to 120ms, but even less is better (less uncompressed data to queue).
#define PLAYER_MAX_FRAME_SAMPLES                    3840   // 80ms @ 48kHz

// This size of the static buffer of zeros to use to fill the i2s buffer when there's no audio.
// The esp32 i2s APIs annoyingly don't offer this functionality, so a static buffer is kept.
#define PLAYER_DMA_BUF_ZEROS_SAMPLES                200

// This represents to minimum number of samples that should be buffered.  That is, this should
// be large enough to ensure that data remains available between executions of the audio service
// routine.  It must be greater than DMA_BUF_SIZE_SAMPLES, and probably should be several times
// that (since the service routine is only invoked when a DMA buffer is consumed).
#define PLAYER_PLAYBACK_BUF_SAMPLES_MIN             5500

// This is the minimum time error (in samples) to correct.  Anything smaller will be ignored
// in the hope that it's just transient time sync error that will just as likely drift the other
// way.  This prevents injecting samples one cycle just to remove them on the next.
#define PLAYER_PLAYBACK_SAMPLE_JITTER_MIN           120

// Size of the individual DMA buffers.  They are limited to 4092 bytes (but annoying only throw
// an error if >4096, which burnt me) so 1023 for 2ch, 16b.  The longer the buffer, the fewer
// interrupts but it could be bad if they were too big (e.g. larger than the chunk/frame size)
// but that isn't likely to be an issue in practice considering the small-ish max size.
#define PLAYER_DMA_BUF_SIZE_SAMPLES                 1000   // ~21ms @ 48kHz

// Number of DMA buffers to allocate.  The total sample memory is given by SIZE_SAMPLES*BUF_COUNT.
// This needs to be large enough to hold the worst case, which would be something like
// SAMPLES_MIN + MAX_FRAME (i.e. decoding a frame when the buffered data just dips below the
// needs-service threshold.
#define PLAYER_DMA_BUF_COUNT                        20

// This is the maximum number chunks to queue.  Unfortunately what this practically means depends
// on the chunk/frame size the server is using (which it doesn't advertise).  This doesn't
// consume meaningful memory on it's own, but the queued frames can pretty easily exhaust the
// memory if they aren't limited.
#define PLAYER_CHUNK_BUF_LEN                        50

#if PLAYER_PLAYBACK_BUF_SAMPLES_MIN + PLAYER_MAX_FRAME_SAMPLES + PLAYER_DMA_BUF_SIZE_SAMPLES > PLAYER_DMA_BUF_COUNT * PLAYER_PLAYBACK_BUF_SAMPLES_MIN
#error DMA needs more buffers
#endif

typedef struct __attribute__((packed)) ScPacketWireChunk
{
	int32_t timestamp_sec;   // The second value of the timestamp when this part of the stream was recorded
	int32_t timestamp_usec;  // The microsecond value of the timestamp when this part of the stream was recorded
	uint32_t size;           // Size of the following payload
	uint8_t payload[0];      // Buffer of data containing the encoded PCM data (a decodable chunk per message)
} ScPacketWireChunk;

typedef struct RiffHdr
{
	uint32_t chunk_id_4cc;    // 'RIFF' fourcc
	uint32_t chunk_size;      // 
	uint32_t format_4cc;      // 'WAVE' fourcc
	uint32_t sub1_id_4cc;     // 'fmt ' fourcc
	uint32_t sub1_size;       //
	uint16_t format;          // 1=PCM, otherwise compressed
	uint16_t num_channels;    // 1, 2, etc
	uint32_t sample_rate;     // 8000, 44100, 48000, etc
	uint32_t byte_rate;       // sample_rate * num_channels * sample_bits/8
	uint16_t block_align;     // num_channels * sample_bits/8
	uint16_t sample_bits;     // 8, 16, etc
	uint32_t sub2_id_4cc;     // 'fmt ' fourcc
	uint32_t sub2_size;       //
	uint8_t data[0];
} RiffHdr;

typedef struct PlayerCtrl
{
	QueueHandle_t i2s_event_queue;
	
	QueueHandle_t chunk_queue;
	
	TimeModel time_model;
	uint32_t sync_status;
	SemaphoreHandle_t time_mutex;
	
	uint8_t codec;
	uint8_t muted;
	uint8_t volume;
	uint32_t buffer_ms;
	uint32_t latency_ms;
	
	OpusDecoder *opus_decoder;
	FLAC__StreamDecoder *flac_decoder;
	
} PlayerCtrl;

static uint32_t s_exists = 0;
static PlayerCtrl s_player_ctrl;

static uint8_t s_zero_samples[4 * PLAYER_DMA_BUF_ZEROS_SAMPLES];
static int16_t s_decode_samples[2 * PLAYER_MAX_FRAME_SAMPLES];

static uint64_t scc_time_get_us()
{
	xSemaphoreTake(s_player_ctrl.time_mutex, portMAX_DELAY);
	uint64_t time = xtime_calc(&s_player_ctrl.time_model, app_read_systimer_unit1());
	xSemaphoreGive(s_player_ctrl.time_mutex);
	return time;
}

static void app_player_fill_zeros(uint32_t samples)
{
	uint32_t t0 = app_read_systimer_unit1()/16000;
	while (samples)
	{
		size_t size = (samples > PLAYER_DMA_BUF_ZEROS_SAMPLES ? PLAYER_DMA_BUF_ZEROS_SAMPLES : samples);
		ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_write(I2S_NUM, s_zero_samples, 4 * size, &size, 100));
		samples -= size / 4;
	}
	uint32_t t1 = app_read_systimer_unit1()/16000;
	if (t1 - t0 > 5)
	{
		// If this waited for a buffer than the system is dead.  It means the open loop prediction
		// of samples currently in the I2S queue was wrong and there's almost no way of salvaging
		// it a this point.
		ESP_LOGW(TAG, "app_player_fill_zeros - filling took %ims", t1 - t0);
	}
}

void app_player_set_params(uint32_t buffer_ms, uint32_t latency_ms, uint8_t muted, uint8_t volume)
{
	s_player_ctrl.buffer_ms = buffer_ms;
	s_player_ctrl.latency_ms = latency_ms;
	s_player_ctrl.muted = muted;
	s_player_ctrl.volume = volume;
}

void app_player_set_time_model(const TimeModel *model)
{
	if (!s_player_ctrl.sync_status)
	{
		ESP_LOGI(TAG, "got initial time sync");
	}
	xSemaphoreTake(s_player_ctrl.time_mutex, portMAX_DELAY);
	memcpy(&s_player_ctrl.time_model, model, sizeof(TimeModel));
	s_player_ctrl.sync_status = 1;
	xSemaphoreGive(s_player_ctrl.time_mutex);
}

int app_player_enqueue_chunk(ScPacketWireChunk *chunk)
{
	// FIXME: Once this isn't using a global, this check can be removed.
	// FIXME: This should probably monitor free memory and drop chunks to prevent OOM, though
	// that's of niche value since usually the chunk count should be pretty fixed as the server's
	// buffer_ms and if there isn't enough RAM for that much data the system can't work (unless it
	// gives up on sync).  Still, if the player gets behind (e.g. no time sync) this could be
	// helpful since the static queue size needs to be large enough for small chunks but the server
	// could send big ones.s
	if (!s_player_ctrl.chunk_queue)
	{
		return 0;
	}
	BaseType_t rsend = xQueueSend(s_player_ctrl.chunk_queue, &chunk, 0);
	if (rsend == errQUEUE_FULL)
	{
		ScPacketWireChunk *old;
		BaseType_t rrecv = xQueueReceive(s_player_ctrl.chunk_queue, &old, 0);
		rsend = xQueueSend(s_player_ctrl.chunk_queue, &chunk, 0);
		if (rrecv == pdTRUE)
		{
			free(old);
		}
		//ESP_LOGW(TAG, "chunk buffer overflow");
	}
	return (rsend == pdTRUE);
}

void app_player_manager(void *pvParameters)
{
	i2s_event_t event;
	uint32_t tx_pending = 0;
	uint32_t print_period = 0;
	
	// The I2S device seems to be set up with all DMA buffers considered full of zeros.  So init
	// the pending count as "full" so that this can better estimate the DMA queue latency and more
	// importantly never need to wait for free spots in i2s_write.
	tx_pending = PLAYER_DMA_BUF_COUNT * PLAYER_DMA_BUF_SIZE_SAMPLES;
	
	while(1)
	{
		print_period++;
		for (uint32_t ri2sq = xQueueReceive(s_player_ctrl.i2s_event_queue, &event, (200 / portTICK_PERIOD_MS));
			ri2sq == pdPASS ;
			ri2sq = xQueueReceive(s_player_ctrl.i2s_event_queue, &event, 0))
		{
			if (event.type == I2S_EVENT_TX_DONE)
			{
				if (tx_pending <= PLAYER_DMA_BUF_SIZE_SAMPLES)
				{
					tx_pending = PLAYER_DMA_BUF_SIZE_SAMPLES;
					// This is actually not okay as it should be padding zeros with enough space to
					// prevent running out of buffers.  If it does underrun, that means it grabbed
					// some random buffer so there should still be PLAYER_DMA_BUF_SIZE_SAMPLES
					// queued, from a latency perspective.
					ESP_LOGE(TAG, "i2s DMA underrun");
				}
				else
				{
					tx_pending -= PLAYER_DMA_BUF_SIZE_SAMPLES;
				}
			}
		}
		
		// TODO: This `now` is probably pretty sketchy since this needs to wakeup after the ISR and
		// read the queue.  Hopefully that's a fairly small and reasonably deterministic delay but
		// isn't ideal.  It's fine for now as this isn't compensating for round trip time, but if
		// that gets to be something it supports, it needs a better time source.  That probably
		// means calculating the sysclk value based on number of samples sent, but maybe it could be
		// captured at the I2S interrupt if I rewrite the i2s driver like I want to.
		uint64_t now = scc_time_get_us();
		
		if (!s_player_ctrl.sync_status && print_period % 16 == 0)
		{
			ESP_LOGI(TAG, "time sync pending");
		}
		
		// ESP_LOGE(TAG, "queue chunk (count = %i, free mem = %i)", uxQueueMessagesWaiting(s_player_ctrl.chunk_queue), esp_get_free_heap_size());
		for (ScPacketWireChunk *chunk;
				s_player_ctrl.sync_status &&
				tx_pending < PLAYER_PLAYBACK_BUF_SAMPLES_MIN &&
				xQueueReceive(s_player_ctrl.chunk_queue, &chunk, 0) == pdTRUE;
			 free(chunk))
		{
			//ESP_LOGE(TAG, "wire chunk buffer reading len = %i", s_player_ctrl.chunk_buf_count);
			int64_t  play_time     = TV_2_US(chunk->timestamp);
			int64_t  play_offset  = (s_player_ctrl.buffer_ms - s_player_ctrl.latency_ms) * 1000LL + play_time - now;
			int64_t  sample_offset = play_offset * 48000 / 1000000;
			uint32_t nsamples = 0;
			//ESP_LOGE(TAG, "testing a chunk @ (time-now)==%lli (time=%lli, now=%lli)", play_offset, play_time, now);
			if (sample_offset < 0)
			{
				// In theory there could be useful samples at the end of the chunk even if the start
				// is negative, but the buffer is underrunning so better to try and start fresh than
				// salvage the tail of a chunk.
				ESP_LOGW(TAG, "discarded a chunk @ sample_offset=%lli, (time-now)==%lli (time=%lli, now=%lli, buffer=%i, latency=%is)", sample_offset, play_offset, play_time, now, s_player_ctrl.buffer_ms, s_player_ctrl.latency_ms);
				continue;
			}
			if (sample_offset > PLAYER_PLAYBACK_BUF_SAMPLES_MIN)
			{
				// This means that the chunk is scheduled to be played further in the future than
				// we buffer for I2S.  So if shouldn't be played now and instead deferred.  The
				// DMA buffer will be filled with zeros as needed.
				if (xQueueSendToFront(s_player_ctrl.chunk_queue, &chunk, 0) != pdTRUE)
				{
					// The buffer is full but the front chunk is still in the future.  That probably
					// means the number of chunks needed to be buffered `buffer_ms/chunk_ms` is
					// larger than PLAYER_CHUNK_BUF_LEN (less BUF_SAMPLES_MIN).
					ESP_LOGE(TAG, "discarded a future chunk due to full buffer?!");
					free(chunk);
				}
				ESP_LOGI(TAG, "replaced future buf @ sample_offset=%lli, (time-now)==%lli (time=%lli, now=%lli, buffer=%i, latency=%is)", sample_offset, play_offset, play_time, now, s_player_ctrl.buffer_ms, s_player_ctrl.latency_ms);
				break;
			}
			if (tx_pending + PLAYER_PLAYBACK_SAMPLE_JITTER_MIN < sample_offset)
			{
				ESP_LOGI(TAG, "padding for chunk play (tx_pending=%i, sample_offset=%i", tx_pending, sample_offset);
				app_player_fill_zeros(sample_offset - tx_pending);
				tx_pending = sample_offset;
			}
			//ESP_LOGI(TAG, "playing a chunk @ sample_offset=%lli, (time-now)==%lli (time=%lli, now=%lli)", sample_offset, play_offset, play_time, now);
			if (s_player_ctrl.codec == PLAYER_CODEC_OPUS)
			{
				assert(s_player_ctrl.opus_decoder);
				// TODO: Opus specs 120ms as the max frame, but snapcast can probably guarantee less than
				// that.  For example, it's currently sending 20ms frames and probably never sending 120ms.
				// TODO: Might want to statically allocate this, but if I get rid of the I2S driver with
				// its overhead then this would just decode into my I2S DMA ring buffer and thus a static
				// scratch buffer is more of a fallback solution (to avoid fragmentation and overhead)
				// TODO: This is a little slow.  At COMPLEXITY:10 and 20MHz DIO flash and -O2, it takes about
				// 30ms to decode a 40ms frame.  -O2/-Os don't make much difference, bumping the flash to
				// 80MHz QIO helps massively, dropping decode time to ~12ms.  With the faster flash, -Os
				// is about 10% slower than -O2.  Sprinkling some IRAM_ATTR didn't really help in the slow
				// flash case, but I also didn't identify any hotspots, just tried 'randomly'.
				int16_t max_frame = PLAYER_MAX_FRAME_SAMPLES;
				// TODO: some of the decoder options (OPUS_SET_GAIN_REQUEST) could be helpful
				int r = opus_decode(s_player_ctrl.opus_decoder, chunk->payload, chunk->size, s_decode_samples, max_frame, 0);
				if (r < 0)
				{
					ESP_LOGE(TAG, "Failed to decode OPUS data: %i(%s)", r, opus_strerror(r));
					continue;
				}
				nsamples = r;
			}
			else
			{
				ESP_LOGE(TAG, "Unsupported CODEC %i", s_player_ctrl.codec);
				continue;
			}
			int16_t min=0x7FFF, max=-0x8000;
			for (int i = 0; i < 2 * nsamples; i++)
			{
				int16_t *s = s_decode_samples + i;
				*s = ((int32_t)*s) * s_player_ctrl.volume / 100 / 8 * (1 - s_player_ctrl.muted);
				if (*s < min) min = *s;
				if (*s > max) max = *s;
			}
			size_t written = 0;
			ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_write(I2S_NUM, s_decode_samples, 4*nsamples, &written, 100));
			tx_pending += written / 4;
			// ESP_LOGE(TAG, "Decoded %i samples, write %i bytes", nsamples, written);
		}
		if (tx_pending < PLAYER_PLAYBACK_BUF_SAMPLES_MIN)
		{
			//ESP_LOGW(TAG, "filling zeros");
			app_player_fill_zeros(PLAYER_PLAYBACK_BUF_SAMPLES_MIN - tx_pending);
			tx_pending = PLAYER_PLAYBACK_BUF_SAMPLES_MIN;
		}
	}
}

int app_player_init(uint8_t codec, const void *codec_info, uint32_t codec_info_sizes)
{
	if (s_exists)
	{
		ScPacketWireChunk *old;
		while (xQueueReceive(s_player_ctrl.chunk_queue, &old, 0) == pdPASS)
		{
			free(old);
		}
	}
	
	s_exists = 1;
	
	// Should be in BSS and be zeroed, but just to be sure...
	bzero(s_zero_samples, 4 * PLAYER_DMA_BUF_ZEROS_SAMPLES);
	
	s_player_ctrl.sync_status = 0;
	s_player_ctrl.codec = codec;
	s_player_ctrl.buffer_ms = 0;
	s_player_ctrl.latency_ms = 0;
	s_player_ctrl.muted = 1;
	s_player_ctrl.volume = 100;
	s_player_ctrl.opus_decoder = NULL;
	s_player_ctrl.flac_decoder = NULL;
	
	// TODO: use the correct sample rate, channels, etc from the codec info
	switch (codec)
	{
		case PLAYER_CODEC_PCM:
		{
			if (codec_info_sizes != sizeof(RiffHdr))
			{
				ESP_LOGE(TAG, "PCM codec info unexpected size (got %i, expected %i)", codec_info_sizes, sizeof(RiffHdr));
				return 1;
			}
			const RiffHdr *riff = (const RiffHdr *)codec_info;
			ESP_LOGI(TAG, "PCM info chunk='%.4s', format='%.4s', size=%i", &riff->chunk_id_4cc, &riff->format_4cc, riff->chunk_size);
			ESP_LOGI(TAG, "PCM info subchunk='%.4s', size=%i", &riff->sub1_id_4cc, riff->sub1_size);
			ESP_LOGI(TAG, "PCM info format=%i, channels=%i, sample=%i, byte_rate=%i, align=%i, bps=%i", riff->format, riff->num_channels, riff->sample_rate, riff->byte_rate, riff->block_align, riff->sample_bits);
			ESP_LOGI(TAG, "PCM info subchunk='%.4s', size=%i", &riff->sub2_id_4cc, riff->sub2_size);
			break;
		}
		case PLAYER_CODEC_FLAC:
		{
			s_player_ctrl.flac_decoder = FLAC__stream_decoder_new();
			if (!s_player_ctrl.flac_decoder)
			{
				ESP_LOGE(TAG, "Failed to create FLAC decoder");
				return 1;
			}
			// TODO
			ESP_LOGE(TAG, "Server requested FLAC and that isn't working yet");
			FLAC__stream_decoder_delete(s_player_ctrl.flac_decoder);
			return 1;
		}
		case PLAYER_CODEC_OPUS:
		{
			int error = OPUS_OK;
			s_player_ctrl.opus_decoder = opus_decoder_create(48000, 2, &error);
			if (error != OPUS_OK)
			{
				s_player_ctrl.opus_decoder = NULL;
				ESP_LOGE(TAG, "Failed to create OPUS decoder: %i(%s)", error, opus_strerror(error));
				return 1;
			}
			break;
		}
		default:
			ESP_LOGE(TAG, "unknown codec %i", codec);
			return 1;
	}
	
	s_player_ctrl.time_mutex = xSemaphoreCreateMutex();
	
	s_player_ctrl.chunk_queue = xQueueCreate(PLAYER_CHUNK_BUF_LEN, sizeof(ScPacketWireChunk*));
	
	ESP_LOGW(TAG, "initial free mem %d", esp_get_free_heap_size());
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,
		.sample_rate = 48000,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.dma_buf_count = PLAYER_DMA_BUF_COUNT,
		.dma_buf_len = PLAYER_DMA_BUF_SIZE_SAMPLES,
		.use_apll = false,
		.tx_desc_auto_clear = false,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.bits_per_chan = 0
	};
	i2s_pin_config_t pin_config = {
		.mck_io_num = I2S_PIN_NO_CHANGE,
		.bck_io_num = GPIO_PIN_I2S_BCK,
		.ws_io_num = GPIO_PIN_I2S_LRCK,
		.data_out_num = GPIO_PIN_I2S_DOUT,
		.data_in_num = I2S_PIN_NO_CHANGE
	};
	i2s_driver_install(I2S_NUM, &i2s_config, 16, &s_player_ctrl.i2s_event_queue);
	i2s_set_pin(I2S_NUM, &pin_config);
	
	ESP_LOGW(TAG, "with i2s driver free mem %d", esp_get_free_heap_size());
	
	// Task for signal on I2S DMA complete
	xTaskCreate(&app_player_manager, "player_manager", 1024*16, NULL, 1, NULL); 
	ESP_LOGW(TAG, "with player_manager driver free mem %d", esp_get_free_heap_size());
	
	ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_set_clk(I2S_NUM, 48000, 16, 2));
	
	return 0;
}


