#include <cJSON.h>
#include <stdint.h>

// Pull in non-shared config
#include "local_config.h"
// #define WIFI_SSID        "..."
// #define WIFI_PASS        "..."
// #define SERVER_IP        "xxx.xxx.xxx.xxx"

#define LOG_FORMAT(letter, format)  LOG_COLOR_ ## letter #letter " (%u) %s: " format LOG_RESET_COLOR "\n"

#define ESP_LOG_PRINTF(format, ...) do {                     \
		esp_log_write(ESP_LOG_DEBUG, "ESP_LOG_PRINTF", LOG_COLOR_D format LOG_RESET_COLOR __VA_OPT__(,) __VA_ARGS__); \
	} while (0)


// wifi.c
void app_wifi_init_sta(void);
