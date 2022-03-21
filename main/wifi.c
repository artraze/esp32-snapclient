#include "main.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"

static const char *TAG = "app_wifi";

#define WIFI_MAX_RETRY       5

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
static EventGroupHandle_t s_wifi_event_group;

static void app_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	static int s_retry_num = 0;
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (s_retry_num < WIFI_MAX_RETRY)
		{
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		}
		else
		{
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG, "connect to the AP fail");
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

void app_wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();
	
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASS,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.pmf_cfg = { .capable = true, .required = false },
		},
	};
	
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_netif_t *netif = esp_netif_create_default_wifi_sta();

	// This seems to be the correct way to do this, esp_netif_create_wifi with if_desc set doesn't
	// work though that field has been described for that purpose, so IDK.
	char *hostname = util_read_nvs_str(APP_NVS_KEY_HOST_NAME, CONFIG_LWIP_LOCAL_HOSTNAME);
	if (hostname)
	{
		ESP_ERROR_CHECK(esp_netif_set_hostname(netif, hostname));
		free(hostname);
	}
	
	// esp_netif_set_mac would seem better since it uses the netif, but it doesn't work, so
	// esp_wifi_set_mac must be used instead
	// uint8_t mac[6] = { 0xa0, 0x76, 0x4e, 0x01, 0x01, 0x01 };
	// esp_netif_set_mac(netif, mac);
	
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL, &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL, &instance_got_ip));

	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	
	ESP_LOGI(TAG, "wifi_init_sta finished.");
	
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

	if (bits & WIFI_CONNECTED_BIT)
	{
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
	}
	else if (bits & WIFI_FAIL_BIT)
	{
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
	}
	else
	{
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}

	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
	vEventGroupDelete(s_wifi_event_group);
}
