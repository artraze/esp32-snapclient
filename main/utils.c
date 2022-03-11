#include "main.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "app_utils";

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

