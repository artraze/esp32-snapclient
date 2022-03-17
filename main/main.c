#include "main.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_console.h"

static const char *TAG = "app_main";

RTC_DATA_ATTR static int s_boot_count = 0;

int app_console_set_name(int argc, char **argv)
{
	printf("app_console_set_name %i %s\n", argc, argv[0]);
	return 0;
}

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
	
	app_util_init();
	
	app_wifi_init_sta();
	
	app_snapclient_init();
	
	// Console seems okay, but also seems to require(?) VFS which maybe isn't great, though if
	// I want to store device name somewhere I may need to use that anyways (unsure of the NVS APIs)
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	repl_config.prompt = "snapclient>";
	esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
	esp_console_cmd_t repl_cmd = {
		.command = "set_name",
		.help = "Sets the device name (in nvs)",
		.hint = "<name>",
		.func = app_console_set_name,
		.argtable = NULL,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&repl_cmd));
	ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
