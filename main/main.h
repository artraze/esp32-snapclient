#include <cJSON.h>
#include <stdint.h>

// Pull in non-shared config
#include "local_config.h"
// #define WIFI_SSID        "..."
// #define WIFI_PASS        "..."
// #define SERVER_IP        "xxx.xxx.xxx.xxx"


#define I2S_NUM              0
#define GPIO_PIN_I2S_BCK     0
#define GPIO_PIN_I2S_LRCK    1
#define GPIO_PIN_I2S_DOUT    10

#define TV_2_US(tv)                              (((tv##_sec) * 1000000ULL) + (tv##_usec))

#define LOG_FORMAT(letter, format)  LOG_COLOR_ ## letter #letter " (%u) %s: " format LOG_RESET_COLOR "\n"

#define ESP_LOG_PRINTF(format, ...) do {                     \
		esp_log_write(ESP_LOG_DEBUG, "ESP_LOG_PRINTF", LOG_COLOR_D format LOG_RESET_COLOR __VA_OPT__(,) __VA_ARGS__); \
	} while (0)


struct ScPacketWireChunk;

// wifi.c
void app_wifi_init_sta(void);

// player.c
#define PLAYER_CODEC_PCM                       0
#define PLAYER_CODEC_FLAC                      1
#define PLAYER_CODEC_OPUS                      2
void app_player_set_params(uint32_t buffer_ms, uint32_t latency_ms, uint8_t muted, uint8_t volume);
int app_player_enqueue_chunk(struct ScPacketWireChunk *chunk);
int app_player_init(uint8_t codec, const void *codec_info, uint32_t codec_info_sizes);

// snapclient.c
void app_snapclient_init(void);

// utils.c
cJSON *util_parse_json(const char *buffer, uint32_t size, const char *log_title);
