#include "main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

RTC_DATA_ATTR static int s_boot_count = 0;


void app_main(void)
{
	esp_log_level_set("ESP_LOG_PRINTF", ESP_LOG_VERBOSE);
	
	++s_boot_count;
	ESP_LOGI(TAG, "Boot count: %d", s_boot_count);
	
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	
	// Set up before initing other submodules so that they can add console commands.
	app_console_init();
	
	app_util_init();
	
	app_wifi_init_sta();
	
	app_snapclient_init();
	
	app_console_start();
}
