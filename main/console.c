#include "main.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"

#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_serial_jtag_struct.h"

// This is a little awkward to be a global, but a lot of console things (e.g. commands) are global
// so why not this.
static esp_console_repl_t *s_repl = NULL;


int app_console_set_hostname(int argc, char **argv)
{
	if (argc != 2)
	{
		printf("Usage: set_hostname <name>\n");
		return 1;
	}
	util_write_nvs_str(APP_NVS_KEY_HOST_NAME, argv[1]);
	printf("hostname set to %s\n", argv[1]);
	return 0;
}

int app_console_dump_tasks(int argc, char **argv)
{
	TaskStatus_t *tasks_info;
	UBaseType_t n_tasks;
	uint32_t total_runtime;
	
	n_tasks = uxTaskGetNumberOfTasks();
	
	tasks_info = malloc(n_tasks * sizeof(TaskStatus_t));
	assert(tasks_info);

	n_tasks = uxTaskGetSystemState(tasks_info, n_tasks, &total_runtime);
	if (total_runtime == 0) total_runtime = -1;
	
	printf("Num  Name                     Priority PriBase  RunTime     Run%%  StackMin\n");
	for (int i = 0; i < n_tasks; i++)
	{
		TaskStatus_t *task = tasks_info + i;
		printf("%4i %24s %8i %8i %11i %4i%% %11i\n",
			task->xTaskNumber,
			task->pcTaskName,
			task->uxCurrentPriority,
			task->uxBasePriority,
			task->ulRunTimeCounter,
			task->ulRunTimeCounter / (total_runtime / 100),
			task->usStackHighWaterMark
		);
	}
	free(tasks_info);
	return 0;
}

void app_console_init(void)
{
	esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	repl_config.prompt = "snapclient>";
	
#ifdef APP_USE_CONSOLE
#if CONFIG_ESP_CONSOLE_UART
	esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
	esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
	// Attempt to detect if the USB is connected since this will block forever if it's not.
	// Ideally the driver would do something like track time of the last StartOfFrame but it
	// doesn't.  So here check to see if one of these two interupts have fired at some point
	// (the driver doesn't enable/clear them).  The TOKEN_REC is active if USB is plugged in at
	// boot, while BUS_RESET is activated on connection (but disconnect doesn't affect either, I
	// believe).
	if (USB_SERIAL_JTAG.int_raw.val & (USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_RAW | USB_SERIAL_JTAG_USB_BUS_RESET_INT_RAW))
	{
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &s_repl));
	}
#endif
#endif
	
	esp_console_cmd_t repl_cmd1 = {
		.command = "set_hostname",
		.help = "Sets the hostname name (in nvs, needs reboot)",
		.hint = "<name>",
		.func = app_console_set_hostname,
		.argtable = NULL,
	};
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_cmd_register(&repl_cmd1));
	esp_console_cmd_t repl_cmd2 = {
		.command = "tasks",
		.help = "Dumps stats on FreeRTOS tasks",
		.hint = "",
		.func = app_console_dump_tasks,
		.argtable = NULL,
	};
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_cmd_register(&repl_cmd2));
}

void app_console_start(void)
{
#ifdef APP_USE_CONSOLE
	if (s_repl)
	{
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_start_repl(s_repl));
	}
#endif
}
