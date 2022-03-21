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

// This is handy for configuration of the device, but sadly the USB_JTAG implementation is fairly
// buggy.  In particular, vfs_dev_usb_serial_jtag doesn't correctly support O_NONBLOCK so once
// the esp_console starts and switches to "driver" based usb_jtag it becomes permanently blocking.
// That might not be terrible, but the USB logic, unlike a real UART, won't send data if it's not
// connected.  Thus, the device stalls out after a ESP_LOG or two.
// This attempts to detect if the USB_JTAG is connected before enabling the console, but it doesn't
// seem like there is a robust way (see comments in main.c) so this provides a way to disable the
// console startup.
// ALSO: This sets the UART to blocking mode, which may tanks the ESP_LOG performance.  Not sure...
// TODO: test that
#define APP_USE_CONSOLE



#define TV_2_US(tv)                              (((tv##_sec) * 1000000ULL) + (tv##_usec))

#define LOG_FORMAT(letter, format)  LOG_COLOR_ ## letter #letter " (%u) %s: " format LOG_RESET_COLOR "\n"

#define ESP_LOG_PRINTF(format, ...) do {                     \
		esp_log_write(ESP_LOG_DEBUG, "ESP_LOG_PRINTF", LOG_COLOR_D format LOG_RESET_COLOR __VA_OPT__(,) __VA_ARGS__); \
	} while (0)


struct ScPacketWireChunk;
struct TimeModel;

// wifi.c
void app_wifi_init_sta(void);

// player.c
struct PlayerState;
#define PLAYER_CODEC_PCM                       0
#define PLAYER_CODEC_FLAC                      1
#define PLAYER_CODEC_OPUS                      2
void app_player_set_params(struct PlayerState *state, uint32_t buffer_ms, uint32_t latency_ms, uint8_t muted, uint8_t volume);
void app_player_set_time_model(struct PlayerState *state, const struct TimeModel *model);
int app_player_enqueue_chunk(struct PlayerState *state, struct ScPacketWireChunk *chunk);
struct PlayerState *app_player_create(uint8_t codec, const void *codec_info, uint32_t codec_info_sizes);
void app_player_destroy(struct PlayerState *state);

// console.c
void app_console_init(void);
void app_console_start(void);

// snapclient.c
void app_snapclient_init(void);

// time.c
void app_sntp_init(void);
uint64_t app_time_get_us(void);

// utils.c
cJSON *util_parse_json(const char *buffer, uint32_t size, const char *log_title);
uint64_t app_read_systimer_unit1();
void app_util_init();
