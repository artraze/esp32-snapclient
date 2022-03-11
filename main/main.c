#include "main.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s.h"
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
	
	app_wifi_init_sta();
	
	ESP_LOGW(TAG, "initial free mem %d", esp_get_free_heap_size());
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,
		.sample_rate = 48000,
		.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.dma_buf_count = 30,
		.dma_buf_len = 1024,  // This is in "frames" i.e. units of bits_per_sample/8 * num_channel
		.use_apll = false,
		.tx_desc_auto_clear = true, // Useful but kinda slow, maybe?  i2s_write time is inconsistent
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
	i2s_driver_install(0, &i2s_config, 0, NULL);
	i2s_set_pin(0, &pin_config);
	
	ESP_LOGW(TAG, "with i2s driver free mem %d", esp_get_free_heap_size());
	
	// opus decode does indeed need this much stack 
	xTaskCreate(&app_snapclient_task, "snapclient_task", 1024*12, NULL, 5, NULL);
}
