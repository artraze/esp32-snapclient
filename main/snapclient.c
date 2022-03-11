#include "main.h"
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "driver/i2s.h"
#include "FLAC/stream_decoder.h"
#include "opus.h"

static const char *TAG = "snapclient";

#define SNAPCAST_MESSAGE_TYPE_BASE               0
#define SNAPCAST_MESSAGE_TYPE_CODEC_HEADER       1
#define SNAPCAST_MESSAGE_TYPE_WIRE_CHUNK         2
#define SNAPCAST_MESSAGE_TYPE_SERVER_SETTINGS    3
#define SNAPCAST_MESSAGE_TYPE_TIME               4
#define SNAPCAST_MESSAGE_TYPE_HELLO              5
#define SNAPCAST_MESSAGE_TYPE_STREAM_TAGS        6
#define SNAPCAST_MESSAGE_TYPE_LAST               6

#define SNAPCAST_CODEC_UNKNOWN                   0
#define SNAPCAST_CODEC_PCM                       1
#define SNAPCAST_CODEC_FLAC                      2
#define SNAPCAST_CODEC_OPUS                      3

typedef struct __attribute__((packed)) ScPacketHdr
{
	uint16_t  type;           // Should be one of the typed message IDs
	uint16_t  id;             // Used in requests to identify the message (not always used)
	uint16_t  refersTo;       // Used in responses to identify which request message ID this is responding to
	int32_t   sent_sec;       // The second value of the timestamp when this message was sent. Filled in by the sender.
	int32_t   sent_usec;      // The microsecond value of the timestamp when this message was sent. Filled in by the sender.
	int32_t   received_sec;   // The second value of the timestamp when this message was received. Filled in by the receiver.
	int32_t   received_usec;  // The microsecond value of the timestamp when this message was received. Filled in by the receiver.
	uint32_t  size;           // Total number of bytes of the following typed message
} ScPacketHdr;

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

typedef struct ScClientState
{
	uint8_t codec;
	// Server settings fields
	uint32_t buffer_ms;
	uint32_t latency;
	uint8_t muted;
	uint8_t volume;
	
	OpusDecoder *opus_decoder;
	FLAC__StreamDecoder *flac_decoder;
	
} ScClientState;


static int sock_recv_loop(int sock, void *buffer, uint32_t size)
{
	uint32_t count = 0;
	while (count < size)
	{
		int r = read(sock, buffer + count, size - count);
		if (r <= 0)
		{
			// TODO: Should do anything with EAGAIN / timeout?
			return r;
		}
		count += r;
	}
	return count;
}

static int sock_send_loop(int sock, const void *buffer, uint32_t size)
{
	uint32_t count = 0;
	while (count < size)
	{
		int r = write(sock, buffer + count, size - count);
		if (r <= 0)
		{
			// TODO: Should do anything with EAGAIN / timeout?
			return r;
		}
		count += r;
	}
	return count;
}

static int sock_set_timeout(int sock, int opt, int32_t msec)
{
	struct timeval to;
	to.tv_sec = msec / 1000;
	to.tv_usec = msec % 1000 * 1000;
	return setsockopt(sock, SOL_SOCKET, opt, &to, sizeof(to));
}

static int scc_send_hdr(int sock, uint16_t type, uint32_t size)
{
	ScPacketHdr hdr;
	struct timeval now;
	bzero(&hdr, sizeof(ScPacketHdr));
	hdr.type = type;
	hdr.size = size;
	gettimeofday(&now, NULL);
	hdr.sent_sec = now.tv_sec;
	hdr.sent_usec = now.tv_usec;
	return sock_send_loop(sock, &hdr, sizeof(ScPacketHdr));
}

static int scc_send_msg_hello(int sock)
{
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	printf("mac:" MACSTR "\n", MAC2STR(mac));
	
	const char *snapclient_hello =           \
	"{"                                      \
	"    \"Arch\": \"x86_64\","              \
	"    \"ClientName\": \"esp32client\","   \
	"    \"HostName\": \"esp32client01\","   \
	"    \"ID\": \"84:f7:03:39:f7:2c\","     \
	"    \"Instance\": 1,"                   \
	"    \"MAC\": \"84:f7:03:39:f7:2c\","    \
	"    \"OS\": \"esp32\","                 \
	"    \"SnapStreamProtocolVersion\": 2,"  \
	"    \"Version\": \"0.17.1\""            \
	"}";

	uint32_t len = strlen(snapclient_hello);
	if (scc_send_hdr(sock, SNAPCAST_MESSAGE_TYPE_HELLO, len + 4) <= 0 ||
		write(sock, &len, sizeof(len)) <= 0 ||
		write(sock, snapclient_hello, len) <= 0)
	{
		ESP_LOGE(TAG, "hello message failed to send");
		return 1;
	}
	return 0;
}

static int scc_recv_hdr(int sock, ScPacketHdr *hdr)
{
	while (1)
	{
		int r = sock_recv_loop(sock, hdr, sizeof(ScPacketHdr));
		if (r <= 0)
		{
			if (errno == EAGAIN)
			{
				ESP_LOGI(TAG, "header recv timed out, looping");
				continue;
			}
			ESP_LOGE(TAG, "header recv failed: r=%i, errno=%i(%s)", r, errno, strerror(errno));
			return 1;
		}
		// TODO: populate recv time
		return 0;
	}
}

static int scc_recv_codec_header(ScClientState *state, const char *buffer, uint32_t size)
{
	// FIXME: validate size to ensure it can contain these fields
	ESP_LOGI(TAG, "handling codec header message (size=%i)", size);
	const uint32_t *codec_size = ((uint32_t *)(buffer + 0));
	const char     *codec_name = buffer + 4;
	const uint32_t *data_size  = ((uint32_t *)(buffer + 4 + *codec_size));
	const char     *data       = buffer + 8 + *codec_size;
	ESP_LOGI(TAG, "codec header name=%.*s data_size=%i", *codec_size, codec_name, *data_size);
	if (*codec_size == 3 && !strncmp(codec_name, "pcm", 3))
	{
		state->codec = SNAPCAST_CODEC_PCM;
		if (*data_size != sizeof(RiffHdr))
		{
			ESP_LOGE(TAG, "PCM codec info unexpected size (got %i, expected %i)", *data_size, sizeof(RiffHdr));
			return 1;
		}
		const RiffHdr *riff = (const RiffHdr *)data;
		ESP_LOGI(TAG, "PCM info chunk='%.4s', format='%.4s', size=%i", &riff->chunk_id_4cc, &riff->format_4cc, riff->chunk_size);
		ESP_LOGI(TAG, "PCM info subchunk='%.4s', size=%i", &riff->sub1_id_4cc, riff->sub1_size);
		ESP_LOGI(TAG, "PCM info format=%i, channels=%i, sample=%i, byte_rate=%i, align=%i, bps=%i", riff->format, riff->num_channels, riff->sample_rate, riff->byte_rate, riff->block_align, riff->sample_bits);
		ESP_LOGI(TAG, "PCM info subchunk='%.4s', size=%i", &riff->sub2_id_4cc, riff->sub2_size);
	}
	else if (*codec_size == 4 && !strncmp(codec_name, "flac", 4))
	{
		state->codec = SNAPCAST_CODEC_FLAC;
		state->flac_decoder = FLAC__stream_decoder_new();
		if (!state->flac_decoder)
		{
			ESP_LOGE(TAG, "Failed to create FLAC decoder");
			return 1;
		}
		// TODO
		ESP_LOGE(TAG, "Server requested FLAC and that isn't working yet");
		FLAC__stream_decoder_delete(state->flac_decoder);
		return 1;
	}
	else if (*codec_size == 4 && !strncmp(codec_name, "opus", 4))
	{
		state->codec = SNAPCAST_CODEC_OPUS;
		int error = OPUS_OK;
		state->opus_decoder = opus_decoder_create(48000, 2, &error);
		if (error != OPUS_OK)
		{
			state->opus_decoder = NULL;
			ESP_LOGE(TAG, "Failed to create OPUS decoder: %i(%s)", error, opus_strerror(error));
			return 1;
		}
	}
	else
	{
		state->codec = SNAPCAST_CODEC_UNKNOWN;
		ESP_LOGE(TAG, "unknown codec %.*s", *codec_size, codec_name);
		return 1;
	}
	ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_set_clk(I2S_NUM, 48000, 16, 2));
	return 0;
}

static int scc_recv_wire_chunk(ScClientState *state, char **buffer, uint32_t size)
{
//	ESP_LOGI(TAG, "handling wire chunk message (size=%i)", size);
	const ScPacketWireChunk *chunk = ((ScPacketWireChunk *)(*buffer));
	if (size < sizeof(ScPacketWireChunk) || size < sizeof(ScPacketWireChunk) + chunk->size)
	{
		ESP_LOGE(TAG, "wire chunk content size (%i+%i) larger than message size (%i)", sizeof(ScPacketWireChunk), chunk->size, size);
		return 1;
	}
	uint32_t t0=0, t1=0, t2=0;
	int16_t *samples;
	int16_t nsamples;
	bool free_samples = false;
	if (state->codec == SNAPCAST_CODEC_UNKNOWN)
	{
		// TODO: snapcast spec says to just ignore if the server message hasn't come
		return 0;
	}
	else if (state->codec == SNAPCAST_CODEC_PCM)
	{
		samples = ((int16_t *)(chunk->payload));
		nsamples = chunk->size / 2;
	}
	else if (state->codec == SNAPCAST_CODEC_FLAC)
	{
		// TODO
		return 0;
	}
	else if (state->codec == SNAPCAST_CODEC_OPUS)
	{
		assert(state->opus_decoder);
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
		int16_t max_frame = (120*48000/1000); // 120ms is max size
		t0 = cpu_hal_get_cycle_count();
		samples  = malloc(2*max_frame*sizeof(int16_t));
		free_samples = true;
		// TODO: some of the decoder options (OPUS_SET_GAIN_REQUEST) could be helpful
		t1 = cpu_hal_get_cycle_count();
		int r = opus_decode(state->opus_decoder, chunk->payload, chunk->size, samples, max_frame, 0);
		t2 = cpu_hal_get_cycle_count();
		if (r < 0)
		{
			free(samples);
			ESP_LOGE(TAG, "Failed to decode OPUS data: %i(%s)", r, opus_strerror(r));
			return 1;
		}
		nsamples = 4 * r;
	}
	else
	{
		ESP_LOGE(TAG, "Unsupported CODEC %i", state->codec);
		return 1;
	}
	
	uint32_t t3 = cpu_hal_get_cycle_count();
	int16_t min=0x7FFF, max=-0x8000;
	for (int i = 0; i < nsamples / 2; i++)
	{
		int16_t *s = samples + i;
		*s = ((int32_t)*s) * state->volume / 100 / 8 * (1 - state->muted);
		if (*s < min) min = *s;
		if (*s > max) max = *s;
	}
	uint32_t t4 = cpu_hal_get_cycle_count();
	uint64_t xx = chunk->timestamp_sec * 1000000 + chunk->timestamp_usec;
//	ESP_LOGI(TAG, "samples range = [%5i, %5i] length = %i, stime = %llims", min, max, nsamples/4, xx/1000);
	
	size_t written = 0;
	uint32_t t5 = cpu_hal_get_cycle_count();
	ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_write(I2S_NUM, samples, nsamples, &written, 100));
	uint32_t t6 = cpu_hal_get_cycle_count();
	if (written < nsamples)
	{
		ESP_LOGW(TAG, "wire chunk wrote %i of %i", written, nsamples);
	}
	ESP_LOGW(TAG, "Timing: malloc=%ius, decode=%ius, volume=%ius, i2s=%ius", (t1-t0)/160, (t2-t1)/160, (t4-t3)/160, (t6-t5)/160);
	
	if (free_samples)
	{
		free(samples);
	}
	return 0;
}

static int scc_recv_server_settings(ScClientState *state, const char *buffer, uint32_t size)
{
	ESP_LOGI(TAG, "handling server settings message (size=%i)", size);
	const uint32_t *json_size = ((uint32_t *)(buffer + 0));
	const char     *json_data = buffer + 4;
	if (*json_size + 4 > size)
	{
		ESP_LOGE(TAG, "server settings payload size (%i) larger than message size (%i)", *json_size + 4, size);
		return 1;
	}
	
	cJSON *json = util_parse_json(json_data, *json_size, "server settings");
	
	// TODO: what should be done with missing values?  Overwrite with default or leave alone?
	const cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(json, "bufferMs");
	if (cJSON_IsNumber(item))
	{
		state->buffer_ms = item->valueint;
		ESP_LOGI(TAG, "server settings got buffer_ms = %i", state->buffer_ms);
	}
	item = cJSON_GetObjectItemCaseSensitive(json, "latency");
	if (cJSON_IsNumber(item))
	{
		state->latency = item->valueint;
		ESP_LOGI(TAG, "server settings got latency = %i", state->latency);
	}
	item = cJSON_GetObjectItemCaseSensitive(json, "muted");
	if (cJSON_IsBool(item))
	{
		state->muted = (cJSON_IsTrue(item) ? 1 : 0);
		ESP_LOGI(TAG, "server settings got muted = %i", state->muted);
	}
	item = cJSON_GetObjectItemCaseSensitive(json, "volume");
	if (cJSON_IsNumber(item))
	{
		state->volume = item->valueint;
		ESP_LOGI(TAG, "server settings got volume = %i", state->volume);
	}
	cJSON_Delete(json);
	
	return 0;
}

int scc_recv_stream_tags(ScClientState *state, const char *buffer, uint32_t size)
{
	ESP_LOGI(TAG, "handling stream tags message (size=%i)", size);
	const uint32_t *json_size = ((uint32_t *)(buffer + 0));
	const char     *json_data = buffer + 4;
	if (*json_size + 4 > size)
	{
		ESP_LOGE(TAG, "stream tags payload size (%i) larger than message size (%i)", *json_size + 4, size);
		return 1;
	}
	
	cJSON *json = util_parse_json(json_data, *json_size, "stream tags");
	// The parse function dumps this to console.  I'm not sure what else to so with it
	cJSON_Delete(json);
	return 0;
}

void app_snapclient_task(void *pvParameters)
{
	struct sockaddr_in saddr = { 0 };
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1704);
	inet_aton(SERVER_IP, &saddr.sin_addr);
	
	ScClientState state = {
		.codec = SNAPCAST_CODEC_UNKNOWN,
		.buffer_ms = 0,
		.latency = 0,
		.muted = 1,
		.volume = 100,
		.opus_decoder = NULL,
		.flac_decoder = NULL,
	};
	
	int sock;
	int r;

	ESP_LOGW(TAG, "initial free mem %d", esp_get_free_heap_size());
	
	for ( ;; close(sock), vTaskDelay(10000 / portTICK_PERIOD_MS))
	{
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if(sock < 0)
		{
			ESP_LOGE(TAG, "failed to allocate socket.");
			continue;
		}
		ESP_LOGI(TAG, "allocated socket");
		
		if(connect(sock, ((const struct sockaddr *)&saddr), sizeof(saddr)) != 0)
		{
			ESP_LOGE(TAG, "socket connect failed errno=%d", errno);
			continue;
		}
		ESP_LOGI(TAG, "connected");
		
		if (scc_send_msg_hello(sock))
		{
			continue;
		}
		ESP_LOGI(TAG, "sent hello");

		if (sock_set_timeout(sock, SO_RCVTIMEO, 5000) < 0)
		{
			ESP_LOGE(TAG, "set recv timeout failed: errno=%i(%s)", errno, strerror(errno));
			continue;
		}

		int failed = 0;
		while (!failed)
		{
			ScPacketHdr hdr;
			if (scc_recv_hdr(sock, &hdr)) break;
			// ESP_LOGI(TAG, "got message type=%i, size=%i", hdr.type, hdr.size);
			if (hdr.type > SNAPCAST_MESSAGE_TYPE_LAST)
			{
				// This is maybe unhelpful as it's checked later but does protect against garbage in to a degree
				ESP_LOGE(TAG, "got unknown message type %i", hdr.type);
				break;
			}
			if (hdr.size > 20000)
			{
				ESP_LOGE(TAG, "message body too large %i", hdr.size);
				break;
			}
			char *body = malloc(hdr.size);
			if (!body)
			{
				// TODO: this needn't be fatal
				ESP_LOGE(TAG, "failed to allocate buffer for message body (size=%i)", hdr.size);
				break;
			}
			r = sock_recv_loop(sock, body, hdr.size);
			if (r <= 0)
			{
				ESP_LOGE(TAG, "message body recv failed: r=%i, errno=%i(%s)", r, errno, strerror(errno));
				break;
			}
			
			switch (hdr.type) {
				case SNAPCAST_MESSAGE_TYPE_CODEC_HEADER:
					failed = scc_recv_codec_header(&state, body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_WIRE_CHUNK:
					failed = scc_recv_wire_chunk(&state, &body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_SERVER_SETTINGS:
					failed = scc_recv_server_settings(&state, body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_STREAM_TAGS:
					failed = scc_recv_stream_tags(&state, body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_BASE:
				case SNAPCAST_MESSAGE_TYPE_TIME:
				case SNAPCAST_MESSAGE_TYPE_HELLO:
					// The server shouldn't send these, but it shouldn't be fatal as this
					// understands them enough to not desync the byte stream or anything.
					ESP_LOGW(TAG, "got unexpected message type %i", hdr.type);
					break;
				default:
					ESP_LOGE(TAG, "got unknown message type %i", hdr.type);
					failed = 1;
					break;
			}
			
			free(body);
		}
		
		ESP_LOGE(TAG, "DIED");
	}
}

