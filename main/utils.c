#include "main.h"
#include <stdlib.h>
#include "hal/systimer_hal.h"
#include "esp_log.h"

static const char *TAG = "app_utils";

static systimer_hal_context_t s_systimer_hal_context;

cJSON *util_parse_json(const char *buffer, uint32_t size, const char *log_title)
{
	const char *err_ptr = NULL;
	cJSON *json = cJSON_ParseWithLengthOpts(buffer, size, &err_ptr, 0);
	if (!json)
	{
		if (err_ptr)
		{
			uint32_t length = size - (err_ptr - buffer);
			if (length > 32) length = 32;
			ESP_LOGE(TAG, "failed to parse %s at %.*s\n", log_title, length, err_ptr);
		}
		else
		{
			ESP_LOGE(TAG, "failed to parse %s\n", log_title);
		}
		return NULL;
	}
	char *json_print = cJSON_Print(json);
	ESP_LOGI(TAG, "%s json = %s", log_title, json_print);
	free(json_print);
	return json;
}

uint64_t app_read_systimer_unit1()
{
	return systimer_hal_get_counter_value(&s_systimer_hal_context, 1);
}

void app_util_init()
{
	systimer_hal_init(&s_systimer_hal_context);
	
	for (int i = 0; i < 5; i++)
	{
		uint64_t t0 = systimer_hal_get_counter_value(&s_systimer_hal_context, 1);
		ets_delay_us(2);
		uint64_t t1 = systimer_hal_get_counter_value(&s_systimer_hal_context, 1);
		ets_delay_us(5);
		uint64_t t2 = systimer_hal_get_counter_value(&s_systimer_hal_context, 1);
		ets_delay_us(10);
		uint64_t t3 = systimer_hal_get_counter_value(&s_systimer_hal_context, 1);
		ESP_LOGI(TAG, "Timer: %llius, %llius, %llius, %llius", t0/16, (t1-t0)/16, (t2-t1)/16, (t3-t2)/16);
		ets_delay_us(15000);
	}
}

