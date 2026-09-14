#include "websocket_mgr.h"
#include "websocket_internal.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "WS_MGR";

esp_websocket_client_handle_t client = NULL;
volatile bool is_connected = false;
volatile bool setup_complete = false;
volatile bool websocket_tx_error = false;
volatile uint32_t websocket_connection_generation = 0;
char session_handle[SESSION_HANDLE_MAX_LEN] = {0};
bool session_resumable = false;

StreamBufferHandle_t audio_stream = NULL;
TaskHandle_t audio_playback_task_handle = NULL;
volatile bool audio_turn_active = false;
volatile bool audio_turn_complete_pending = false;
uint32_t audio_chunks_received = 0;
uint64_t audio_bytes_received = 0;
uint64_t audio_bytes_queued = 0;
uint32_t audio_write_calls = 0;
uint64_t audio_bytes_played = 0;
uint64_t audio_bytes_dropped = 0;

static volatile bool ws_started = false;
QueueHandle_t websocket_tx_queue = NULL;
TaskHandle_t websocket_tx_task_handle = NULL;
QueueHandle_t websocket_rx_queue = NULL;
TaskHandle_t websocket_rx_task_handle = NULL;

void websocket_app_start(void)
{
    ESP_LOGI(TAG, "Starting Gemini Live reference session");
    if (!wifi_is_ready() || client != NULL || ws_started) return;

    if (!start_audio_playback()) {
        ESP_LOGE(TAG, "Audio playback init failed");
        return;
    }

    clear_audio_buffer();
    reset_audio_turn_stats();
    reset_rx_buffer();

    if (!websocket_tx_init() || !websocket_rx_init()) {
        ESP_LOGE(TAG, "WebSocket worker init failed");
        return;
    }

    is_connected = false;
    setup_complete = false;
    websocket_tx_error = false;
    ws_started = false;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = WEBSOCKET_SERVER_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = false;
    cfg.cert_common_name = "generativelanguage.googleapis.com";
    cfg.network_timeout_ms = 15000;
    cfg.disable_auto_reconnect = true;

    /* Baseline-compatible keepalive. Reconnect is deliberately disabled so
     * a transport failure cannot masquerade as a successful long session. */
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.buffer_size = 8192;

    ESP_LOGI(TAG,
             "WS config: timeout=15s auto_reconnect=OFF keepalive=30/10/3 buffer=8192");

    client = esp_websocket_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return;
    }

    esp_err_t err = esp_websocket_register_events(
        client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register events failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        client = NULL;
        return;
    }

    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        client = NULL;
        return;
    }

    ws_started = true;
}

bool websocket_is_connected(void)
{
    return is_connected && setup_complete && !websocket_tx_error;
}

void websocket_disconnect(void)
{
    if (client != NULL) {
        esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    }
}

void websocket_reset_started(void)
{
    ws_started = false;
}
