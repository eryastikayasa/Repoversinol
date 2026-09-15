#include "wifi_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_STARTED_BIT   BIT1
#define WIFI_FAILED_BIT    BIT2

static volatile bool s_wifi_started = false;
static volatile bool s_wifi_got_ip = false;
static volatile uint32_t s_wifi_reconnect_count = 0;
static int64_t s_last_connect_attempt_us = 0;
static constexpr int64_t WIFI_RECONNECT_BACKOFF_US = 2000000;

static void request_wifi_connect(const char *reason)
{
    const int64_t now = esp_timer_get_time();
    if (s_last_connect_attempt_us != 0 &&
        now - s_last_connect_attempt_us < WIFI_RECONNECT_BACKOFF_US) {
        ESP_LOGW(TAG, "Wi-Fi reconnect suppressed by backoff (%lld ms since last attempt)",
                 (long long)((now - s_last_connect_attempt_us) / 1000));
        return;
    }

    s_last_connect_attempt_us = now;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect gagal (%s): %s", reason ? reason : "unknown",
                 esp_err_to_name(err));
    } else if (reason) {
        ESP_LOGI(TAG, "Wi-Fi connect requested: %s", reason);
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA START");
        s_wifi_started = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
        request_wifi_connect("startup");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_got_ip = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi GOT_IP - network READY");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        uint32_t count = s_wifi_reconnect_count;
        s_wifi_reconnect_count = count + 1;
        ESP_LOGW(TAG, "Wi-Fi TERPUTUS; reconnect_count=%lu", (unsigned long)(count + 1));
        request_wifi_connect("retry");
        return;
    }
}

void wifi_init_sta(void)
{
    ESP_LOGI(TAG, "Memulai Wi-Fi manager V7.0.5");
    if (s_wifi_event_group != NULL) {
        ESP_LOGW(TAG, "Wi-Fi manager sudah diinisialisasi");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) { ESP_LOGE(TAG, "Gagal membuat Wi-Fi event group"); return; }

    s_wifi_started = false;
    s_wifi_got_ip = false;
    s_wifi_reconnect_count = 0;
    s_last_connect_attempt_us = 0;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init gagal: %s", esp_err_to_name(err)); return;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default gagal: %s", esp_err_to_name(err)); return;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) { ESP_LOGE(TAG, "Gagal membuat default Wi-Fi STA netif"); return; }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init gagal: %s", esp_err_to_name(err)); return;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Register WIFI_EVENT gagal: %s", esp_err_to_name(err)); return; }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Register IP_EVENT gagal: %s", esp_err_to_name(err)); return; }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_mode gagal: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_config gagal: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start gagal: %s", esp_err_to_name(err)); return;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) gagal: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi power save DISABLED (WIFI_PS_NONE)");
    ESP_LOGI(TAG, "Wi-Fi driver STARTED");
}

bool wifi_wait_for_connection(uint32_t timeout_ms)
{
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "WAIT GOT_IP gagal: event group NULL");
        return false;
    }
    if (s_wifi_got_ip) return true;

    ESP_LOGI(TAG, "Menunggu WIFI GOT_IP...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) != 0 && s_wifi_got_ip) {
        ESP_LOGI(TAG, "Wi-Fi READY - GOT_IP diterima");
        return true;
    }
    ESP_LOGW(TAG, "Wi-Fi belum READY dalam timeout; recovery tetap berjalan di event handler");
    return false;
}

bool wifi_is_ready(void)
{
    return s_wifi_started && s_wifi_got_ip;
}

uint32_t wifi_get_reconnect_count(void)
{
    return s_wifi_reconnect_count;
}
