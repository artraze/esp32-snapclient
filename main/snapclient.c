#include "main.h"
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "driver/i2s.h"
#include "time_model.h"

static const char *TAG = "snapclient";

// #define TIME_DBG_SAMPLES      800
#ifdef TIME_DBG_SAMPLES
static uint32_t s_time_dbg_count = 0;
static uint64_t s_time_dbg_remote[TIME_DBG_SAMPLES];
static uint64_t s_time_dbg_local[TIME_DBG_SAMPLES];
static uint64_t s_time_dbg_ticks[TIME_DBG_SAMPLES];
#endif

#define SNAPCAST_MESSAGE_TYPE_BASE               0
#define SNAPCAST_MESSAGE_TYPE_CODEC_HEADER       1
#define SNAPCAST_MESSAGE_TYPE_WIRE_CHUNK         2
#define SNAPCAST_MESSAGE_TYPE_SERVER_SETTINGS    3
#define SNAPCAST_MESSAGE_TYPE_TIME               4
#define SNAPCAST_MESSAGE_TYPE_HELLO              5
#define SNAPCAST_MESSAGE_TYPE_STREAM_TAGS        6
#define SNAPCAST_MESSAGE_TYPE_LAST               6

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

typedef struct __attribute__((packed)) ScPacketTime
{
	int32_t latency_sec;
	int32_t latency_usec;
} ScPacketTime;

typedef struct ScClientState
{
	TimeModelState time_state;
	
	// Server settings fields
	uint32_t buffer_ms;
	uint32_t latency;
	uint8_t muted;
	uint8_t volume;
} ScClientState;


static ScClientState s_state;


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
	"    \"Arch\": \"riscv\","              \
	"    \"ClientName\": \"esp32client\","   \
	"    \"HostName\": \"esp32client01\","   \
	"    \"ID\": \"84:f7:03:39:f7:2c\","     \
	"    \"Instance\": 1,"                   \
	"    \"MAC\": \"84:f7:03:39:f7:2c\","    \
	"    \"OS\": \"esp32\","                 \
	"    \"SnapStreamProtocolVersion\": 2,"  \
	"    \"Version\": \"0.0.1\""             \
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

static int scc_send_msg_time(int sock)
{
	ScPacketTime latency;
	
	if (scc_send_hdr(sock, SNAPCAST_MESSAGE_TYPE_TIME, sizeof(ScPacketTime)) <= 0 ||
		write(sock, &latency, sizeof(ScPacketTime)) <= 0 )
	{
		ESP_LOGE(TAG, "time message failed to send");
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
				// TODO: the time message is kind of a keep-alive so send it on timeout.  This
				// could probably be in a better spot.
				scc_send_msg_time(sock);
#ifdef TIME_DBG_SAMPLES
				if (s_time_dbg_count > TIME_DBG_SAMPLES / 2)
				{
					printf("time_samples=[");
					for (uint32_t i = 0; i < s_time_dbg_count; i++)
						printf("(%lli, %lli, %lli),", s_time_dbg_ticks[i], s_time_dbg_local[i], s_time_dbg_remote[i]);
					printf("]\n");
					s_time_dbg_count = 0;
				}
#endif
				continue;
			}
			ESP_LOGE(TAG, "header recv failed: r=%i, errno=%i(%s)", r, errno, strerror(errno));
			return 1;
		}
		// TODO: populate recv time
		// Add the remote clock instance
		uint64_t time = TV_2_US(hdr->sent);
		uint64_t clock = app_read_systimer_unit1();
		if (xtime_add_observation(&s_state.time_state, time, clock))
		{
			app_player_set_time_model(&s_state.time_state.model);
		}
#ifdef TIME_DBG_SAMPLES
		if (s_time_dbg_count < TIME_DBG_SAMPLES)
		{
			s_time_dbg_remote[s_time_dbg_count] = time;
			s_time_dbg_local[s_time_dbg_count] = xtime_calc(&s_state.time_state.model, clock);
			s_time_dbg_ticks[s_time_dbg_count] = clock;
			s_time_dbg_count++;
		}
#endif
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
	
	// FIXME: This needs to check if the player was already created, and if so destroy and recreate
	// it.
	uint8_t codec = 0;
	if (*codec_size == 3 && !strncmp(codec_name, "pcm", 3))
	{
		codec = PLAYER_CODEC_PCM;
	}
	else if (*codec_size == 4 && !strncmp(codec_name, "flac", 4))
	{
		codec = PLAYER_CODEC_FLAC;
	}
	else if (*codec_size == 4 && !strncmp(codec_name, "opus", 4))
	{
		codec = PLAYER_CODEC_OPUS;
	}
	else
	{
		ESP_LOGE(TAG, "unknown codec %.*s", *codec_size, codec_name);
		return 1;
	}
	if (app_player_init(codec, data, *data_size))
	{
		return 1;
	}
	app_player_set_params(state->buffer_ms, state->latency, state->muted, state->volume);
	return 0;
}

static int scc_recv_wire_chunk(ScClientState *state, char **buffer, uint32_t size)
{
//	ESP_LOGI(TAG, "handling wire chunk message (size=%i)", size);
	ScPacketWireChunk *chunk = ((ScPacketWireChunk *)(*buffer));
	if (size < sizeof(ScPacketWireChunk) || size < sizeof(ScPacketWireChunk) + chunk->size)
	{
		ESP_LOGE(TAG, "wire chunk content size (%i+%i) larger than message size (%i)", sizeof(ScPacketWireChunk), chunk->size, size);
		return 1;
	}
	
	if (app_player_enqueue_chunk(chunk))
	{
		// ESP_LOGE(TAG, "queue chunk (count = %i, free mem = %i)", uxQueueMessagesWaiting(state->chunk_queue), esp_get_free_heap_size());
		*buffer = NULL;
	}
	else
	{
		ESP_LOGE(TAG, "failed to buffer wire chunk");
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
	
	app_player_set_params(state->buffer_ms, state->latency, state->muted, state->volume);
	
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

		if (sock_set_timeout(sock, SO_RCVTIMEO, 2000) < 0)
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
				ESP_LOGE(TAG, "failed to allocate buffer for message body (size=%i, free=%i)", hdr.size, esp_get_free_heap_size());
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
					failed = scc_recv_codec_header(&s_state, body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_WIRE_CHUNK:
					failed = scc_recv_wire_chunk(&s_state, &body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_SERVER_SETTINGS:
					failed = scc_recv_server_settings(&s_state, body, hdr.size);
					break;
				case SNAPCAST_MESSAGE_TYPE_STREAM_TAGS:
					failed = scc_recv_stream_tags(&s_state, body, hdr.size);
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
		
		// TODO: reset the player here.  In particular, free+clear the chunk queue since half the
		// time this dies is due to OOM in the wifi, so it won't recover unless some is freed.
		ESP_LOGE(TAG, "DIED");
	}
}

void app_snapclient_init(void)
{
	s_state.time_state.sync_status = 0;
	s_state.time_state.n_samples_filt = 0;
	s_state.time_state.n_samples_raw = 0;
	
	s_state.buffer_ms = 1000;
	s_state.latency   = 0;
	s_state.muted     = 1;
	s_state.volume    = 100;
	
	xTaskCreate(&app_snapclient_task, "snapclient_task", 1024*3, NULL, 5, NULL);
	ESP_LOGW(TAG, "with app_snapclient_task driver free mem %d", esp_get_free_heap_size());
}
