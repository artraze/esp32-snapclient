#include "main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#ifdef REDIRECT_CONSOLE_UART
#include "esp_rom_uart.h"
#include "esp_rom_sys.h"
#include "esp_vfs_dev.h"
#endif

#ifndef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#endif


static const char *TAG = "app_main";

RTC_DATA_ATTR static int s_boot_count = 0;


void app_main(void)
{
	esp_log_level_set("ESP_LOG_PRINTF", ESP_LOG_VERBOSE);
	
#ifdef REDIRECT_CONSOLE_UART
	// This is how the bootloader sets up the console on UART.  This only applies to the esp rom
	// functions however, like:
	//   esp_rom_uart_tx_one_char('\n');
	//   esp_rom_printf(DRAM_STR("ROM PRINTF %s\n"), "HI");
	// It's possible ESP_LOG would use these if the CONFIG_NEWLIB_NANO_FORMAT flag is set, but
	// without it ESP_LOG / printf won't use this.
	esp_rom_uart_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);
	esp_rom_uart_set_clock_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, UART_CLK_FREQ_ROM, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
	esp_rom_install_uart_printf();
	
	// printf and friends (like libc vprintf which ESP_LOG uses by default but can be overriden by
	// esp_log_set_vprintf) use stdio.  Thus, to replace it the io streams needs to be changed out.
	// I'm not sure if there's a "correct" way to do this, but here the idea is to just 'hack' the
	// global and overwrite the streams.  Newlib uses a structure called _reent that stores global
	// state (both as a true global and thread-local).  The thread (task here) local version of the
	// reent data initializes it's std files from the global ones.  So the global ones must be
	// replaced, as well as the per-task reent data for current tasks.  Here most tasks haven't
	// been created yet so it's mostly sufficient to replace the global and current task files, but
	// in theory one could iterate the FreeRTOS task blocks and edit xNewLib_reent directly, but
	// the tskTaskControlBlock structure is hidden so that wouldn't be a simple hack.
	// Ensure the uart is available as a FILE
	esp_vfs_dev_uart_register();
	// Replace global streams
	_GLOBAL_REENT->_stdin  = fopen("/dev/uart/0", "r");
	_GLOBAL_REENT->_stdout = fopen("/dev/uart/0", "w");
	_GLOBAL_REENT->_stderr = fopen("/dev/uart/0", "w");
	// Update main thread
	__getreent()->_stdin = _GLOBAL_REENT->_stdin;
	__getreent()->_stdout = _GLOBAL_REENT->_stdout;
	__getreent()->_stderr = _GLOBAL_REENT->_stderr;
	
	// Note that panics can't be redirected, though panic.c can be updated with:
	// void panic_print_char(const char c) { esp_rom_uart_tx_one_char(c); }
#endif
	
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
