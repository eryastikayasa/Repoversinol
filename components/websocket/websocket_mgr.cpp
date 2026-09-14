#include "websocket_internal.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "WS_MGR";
esp_websocket_client_handle_t client = NULL;
volatile bool is_connected = false;
volatile bool setup_complete = false;
volatile bool websocket_tx_error = false;
uint32_t websocket_connection_generation = 0;
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
static volatile bool ws_close_requested = false;
static volatile bool ws_cleanup_pending = false;
static QueueHandle_t ws_cleanup_queue = NULL;
static TaskHandle_t ws_cleanup_task_handle = NULL;
static int64_t ws_connected_since_us = 0;
static uint32_t ws_reconnect_count = 0;

QueueHandle_t websocket_tx_queue = NULL;
TaskHandle_t websocket_tx_task_handle = NULL;
QueueHandle_t websocket_rx_queue = NULL;
QueueHandle_t websocket_rx_free_queue = NULL;
TaskHandle_t websocket_rx_task_handle = NULL;

uint32_t websocket_tx_frames = 0;
uint64_t websocket_tx_bytes = 0;
uint32_t websocket_tx_drops = 0;
UBaseType_t websocket_tx_high_water = 0;
uint32_t websocket_tx_encode_count = 0;
uint64_t websocket_tx_encode_total_us = 0;
uint32_t websocket_tx_encode_max_us = 0;
uint32_t websocket_tx_json_count = 0;
uint64_t websocket_tx_json_total_us = 0;
uint32_t websocket_tx_json_max_us = 0;
uint32_t websocket_tx_write_count = 0;
uint64_t websocket_tx_write_total_us = 0;
uint32_t websocket_tx_write_max_us = 0;
uint32_t websocket_tx_write_slow = 0;
uint32_t websocket_tx_write_fail = 0;
uint32_t websocket_tx_write_timeout = 0;

uint32_t websocket_rx_messages = 0;
uint32_t websocket_rx_fragments = 0;
uint32_t websocket_rx_drops = 0;
UBaseType_t websocket_rx_high_water = 0;
uint64_t websocket_rx_process_total_us = 0;
uint32_t websocket_rx_process_max_us = 0;

static void websocket_cleanup_task(void *arg)
{
    (void)arg;
    esp_websocket_client_handle_t old_client = NULL;
    for (;;) {
        if (xQueueReceive(ws_cleanup_queue, &old_client, portMAX_DELAY) == pdTRUE && old_client) {
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_websocket_client_destroy(old_client);
            ws_cleanup_pending = false;
            ESP_LOGI(TAG, "Old WebSocket client destroyed outside event callback");
        }
    }
}

void websocket_tx_flush_queue(void)
{
    if (!websocket_tx_queue) return;
    ws_tx_command_t stale = {};
    while (xQueueReceive(websocket_tx_queue, &stale, 0) == pdTRUE) {}
}

static void websocket_tx_fail(void)
{
    if (websocket_tx_error) return;
    websocket_tx_error = true;
    is_connected = false;
    setup_complete = false;
    ++websocket_connection_generation;
    websocket_tx_flush_queue();
    ESP_LOGW(TAG, "TX transport unhealthy; generation invalidated, stale audio flushed, recovery requested");
    // Do not call websocket_disconnect() from the realtime TX worker. A failed send in
    // esp_websocket_client v1.7.0 may already abort the transport, while an explicit
    // close() can wait for the client task. The supervisor owns the blocking recovery path.
}

static void websocket_tx_task(void *arg)
{
    (void)arg;
    ws_tx_command_t cmd = {};
    static char b64_buf[1024];
    static char json_buf[1200];

    ESP_LOGI(TAG, "TX worker: core=%d priority=%d queue=%d frame=%d bytes/20ms",
             xPortGetCoreID(), uxTaskPriorityGet(NULL), WS_TX_QUEUE_LENGTH, WS_TX_AUDIO_SIZE);

    for (;;) {
        if (xQueueReceive(websocket_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || !client) continue;
        esp_websocket_client_handle_t ws = client;
        if (!esp_websocket_client_is_connected(ws)) continue;

        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup_json = NULL;
            size_t setup_len = 0;
            if (!build_gemini_setup(&setup_json, &setup_len)) continue;
            int sent = esp_websocket_client_send_text(ws, setup_json, (int)setup_len, pdMS_TO_TICKS(1000));
            if (sent != (int)setup_len) websocket_tx_fail();
            else ESP_LOGI(TAG, "Gemini setup sent: %d bytes", sent);
            free(setup_json);
            continue;
        }

        if (cmd.type != WS_TX_COMMAND_AUDIO || cmd.len != WS_TX_AUDIO_SIZE) continue;

        int64_t encode_start_us = esp_timer_get_time();
        size_t encoded_len = 0;
        int ret = mbedtls_base64_encode((unsigned char *)b64_buf, sizeof(b64_buf) - 1,
                                        &encoded_len, cmd.data, cmd.len);
        uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_start_us);
        ++websocket_tx_encode_count;
        websocket_tx_encode_total_us += encode_us;
        if (encode_us > websocket_tx_encode_max_us) websocket_tx_encode_max_us = encode_us;
        if (ret != 0) { ++websocket_tx_drops; continue; }
        b64_buf[encoded_len] = '\0';

        int64_t json_start_us = esp_timer_get_time();
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
            b64_buf);
        uint32_t json_us = (uint32_t)(esp_timer_get_time() - json_start_us);
        ++websocket_tx_json_count;
        websocket_tx_json_total_us += json_us;
        if (json_us > websocket_tx_json_max_us) websocket_tx_json_max_us = json_us;
        if (json_len <= 0 || (size_t)json_len >= sizeof(json_buf)) { ++websocket_tx_drops; continue; }

        int64_t start_us = esp_timer_get_time();
        ++websocket_tx_write_count;
        int sent = esp_websocket_client_send_text(ws, json_buf, json_len,
                                                   pdMS_TO_TICKS(WS_TX_AUDIO_SEND_TIMEOUT_MS));
        uint32_t write_us = (uint32_t)(esp_timer_get_time() - start_us);
        websocket_tx_write_total_us += write_us;
        if (write_us > websocket_tx_write_max_us) websocket_tx_write_max_us = write_us;

        if (sent == json_len) {
            ++websocket_tx_frames;
            websocket_tx_bytes += cmd.len;
            if (write_us >= WS_TX_AUDIO_SLOW_THRESHOLD_US) {
                ++websocket_tx_write_slow;
                ESP_LOGW(TAG, "TX send slow: write_us=%u expected=%d", (unsigned)write_us, json_len);
            }
        } else {
            ++websocket_tx_write_fail;
            ++websocket_tx_drops;
            // esp_websocket_client returns zero for a transport write that produced no
            // bytes; count it as a realtime timeout when the measured call consumed the
            // configured timeout window. The public API does not expose a dedicated
            // timeout result, so do not pretend to know more than the API reports.
            if (sent == 0 && write_us >= (WS_TX_AUDIO_SEND_TIMEOUT_MS * 1000U)) {
                ++websocket_tx_write_timeout;
            }
            ESP_LOGW(TAG, "TX write fail: sent=%d expected=%d write_us=%u timeout=%u",
                     sent, json_len, (unsigned)write_us,
                     (unsigned)websocket_tx_write_timeout);
            websocket_tx_fail();
        }
    }
}

bool websocket_tx_init(void)
{
    if (!websocket_tx_queue) {
        websocket_tx_queue = xQueueCreate(WS_TX_QUEUE_LENGTH, sizeof(ws_tx_command_t));
        if (!websocket_tx_queue) return false;
    }
    if (!ws_cleanup_queue) {
        ws_cleanup_queue = xQueueCreate(1, sizeof(esp_websocket_client_handle_t));
        if (!ws_cleanup_queue) return false;
    }
    if (!ws_cleanup_task_handle) {
        if (xTaskCreatePinnedToCore(websocket_cleanup_task, "ws_cleanup", 3072, NULL, 2, &ws_cleanup_task_handle, 0) != pdPASS) return false;
    }
    if (!websocket_tx_task_handle) {
        if (xTaskCreatePinnedToCore(websocket_tx_task, "ws_tx", 8192, NULL, 5, &websocket_tx_task_handle, 1) != pdPASS) return false;
    }
    return true;
}

bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation)
{
    if (!data || len != WS_TX_AUDIO_SIZE || !websocket_tx_queue || !is_connected || !setup_complete || websocket_tx_error || generation != websocket_connection_generation) return false;
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_AUDIO;
    cmd.generation = generation;
    cmd.len = (uint16_t)len;
    memcpy(cmd.data, data, WS_TX_AUDIO_SIZE);

    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        ws_tx_command_t oldest = {};
        if (xQueueReceive(websocket_tx_queue, &oldest, 0) == pdTRUE && xQueueSend(websocket_tx_queue, &cmd, 0) == pdTRUE) {
            ++websocket_tx_drops;
            ESP_LOGD(TAG, "TX queue full: dropped oldest PCM frame");
        } else {
            ++websocket_tx_drops;
            return false;
        }
    }
    UBaseType_t waiting = uxQueueMessagesWaiting(websocket_tx_queue);
    if (waiting > websocket_tx_high_water) websocket_tx_high_water = waiting;
    return true;
}

void websocket_schedule_setup(uint32_t generation)
{
    if (!websocket_tx_queue || !is_connected || websocket_tx_error || generation != websocket_connection_generation) return;
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_SETUP;
    cmd.generation = generation;
    if (xQueueSendToFront(websocket_tx_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(TAG, "SETUP queue full");
}

void websocket_app_start(void)
{
    if (!wifi_is_ready() || client || ws_started || ws_cleanup_pending) return;
    if (!start_audio_playback()) return;
    clear_audio_buffer();
    reset_audio_turn_stats();
    reset_rx_buffer();
    websocket_tx_flush_queue();
    if (!websocket_tx_init() || !websocket_rx_init()) return;

    is_connected = false;
    setup_complete = false;
    websocket_tx_error = false;
    ws_close_requested = false;
    ws_connected_since_us = 0;
    ++ws_reconnect_count;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = WEBSOCKET_SERVER_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = false;
    cfg.cert_common_name = "generativelanguage.googleapis.com";
    cfg.network_timeout_ms = 15000;
    cfg.disable_auto_reconnect = true;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.buffer_size = 8192;

    client = esp_websocket_client_init(&cfg);
    if (!client) return;
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    if (err != ESP_OK) { esp_websocket_client_destroy(client); client = NULL; return; }
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) { esp_websocket_client_destroy(client); client = NULL; return; }
    ws_started = true;
    ESP_LOGI(TAG, "Gemini Live client started; reconnect_count=%lu", (unsigned long)ws_reconnect_count);
}

bool websocket_is_connected(void)
{
    return is_connected && setup_complete && !websocket_tx_error;
}

bool websocket_healthcheck(void)
{
    if (!is_connected || websocket_tx_error) return false;
    if (setup_complete) return true;
    if (ws_connected_since_us != 0 && esp_timer_get_time() - ws_connected_since_us > 15000000LL) {
        ESP_LOGW(TAG, "Gemini setup timeout; requesting recovery");
        websocket_disconnect();
        return false;
    }
    return true;
}

uint32_t websocket_get_reconnect_count(void)
{
    return ws_reconnect_count;
}

UBaseType_t websocket_get_tx_queue_depth(void)
{
    return websocket_tx_queue ? uxQueueMessagesWaiting(websocket_tx_queue) : 0;
}

UBaseType_t websocket_get_rx_queue_depth(void)
{
    return websocket_rx_queue ? uxQueueMessagesWaiting(websocket_rx_queue) : 0;
}

void websocket_disconnect(void)
{
    esp_websocket_client_handle_t ws = client;
    if (!ws || ws_close_requested) return;
    ws_close_requested = true;
    is_connected = false;
    setup_complete = false;
    websocket_tx_error = true;
    ++websocket_connection_generation;
    websocket_tx_flush_queue();
    websocket_rx_flush_queue();
    websocket_rx_request_reset();
    ESP_LOGW(TAG, "Requesting WebSocket close for recovery");
    esp_err_t err = esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) ESP_LOGW(TAG, "WebSocket close returned: %s", esp_err_to_name(err));
}

void websocket_reset_started(void) { ws_started = false; }

void websocket_cleanup_finished(esp_websocket_client_handle_t old_client)
{
    if (!ws_cleanup_queue || !old_client) return;
    ws_cleanup_pending = true;
    if (xQueueSend(ws_cleanup_queue, &old_client, 0) != pdTRUE) {
        ws_cleanup_pending = false;
        ESP_LOGE(TAG, "WebSocket cleanup queue full; old client not destroyed yet");
    }
}

void websocket_note_connected(void)
{
    ws_connected_since_us = esp_timer_get_time();
}
