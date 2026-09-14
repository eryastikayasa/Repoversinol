#include "websocket_mgr.h"
#include "websocket_internal.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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

void websocket_tx_flush_queue(void)
{
    if (!websocket_tx_queue) return;
    ws_tx_command_t stale = {};
    while (xQueueReceive(websocket_tx_queue, &stale, 0) == pdTRUE) {
        if (stale.data) free(stale.data);
    }
}

static void websocket_tx_fail(void)
{
    websocket_tx_error = true;
    is_connected = false;
    setup_complete = false;
    ++websocket_connection_generation;
    websocket_tx_flush_queue();
}

static void websocket_tx_task(void *arg)
{
    (void)arg;
    ws_tx_command_t cmd = {};
    for (;;) {
        if (xQueueReceive(websocket_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *audio_data = cmd.data;
        cmd.data = NULL;
        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || !client) {
            free(audio_data);
            continue;
        }
        esp_websocket_client_handle_t ws = client;
        if (!esp_websocket_client_is_connected(ws)) {
            free(audio_data);
            continue;
        }

        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup_json = NULL;
            size_t setup_len = 0;
            if (!build_gemini_setup(&setup_json, &setup_len)) {
                free(audio_data);
                continue;
            }
            if (cmd.generation != websocket_connection_generation || !is_connected || client != ws) {
                free(setup_json);
                free(audio_data);
                continue;
            }
            int sent = esp_websocket_client_send_text(ws, setup_json, (int)setup_len, pdMS_TO_TICKS(5000));
            if (sent != (int)setup_len) websocket_tx_fail();
            else ESP_LOGI(TAG, "Gemini setup sent: %d bytes", sent);
            free(setup_json);
            free(audio_data);
            continue;
        }

        if (cmd.type == WS_TX_COMMAND_AUDIO) {
            static char b64_buf[1024];
            static char json_buf[1200];
            if (!audio_data || cmd.len != WS_TX_AUDIO_SIZE) {
                free(audio_data);
                continue;
            }

            size_t encoded_len = 0;
            int ret = mbedtls_base64_encode((unsigned char *)b64_buf, sizeof(b64_buf) - 1,
                                            &encoded_len, audio_data, cmd.len);
            if (ret != 0) {
                free(audio_data);
                continue;
            }
            b64_buf[encoded_len] = '\0';
            int json_len = snprintf(json_buf, sizeof(json_buf),
                "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
                b64_buf);
            if (json_len <= 0 || (size_t)json_len >= sizeof(json_buf)) {
                free(audio_data);
                continue;
            }

            int sent = esp_websocket_client_send_text(ws, json_buf, json_len, pdMS_TO_TICKS(100));
            if (sent == json_len) {
                ++websocket_tx_frames;
                websocket_tx_bytes += cmd.len;
            } else {
                ++websocket_tx_drops;
                ESP_LOGW(TAG, "TX write failed: sent=%d expected=%d", sent, json_len);
            }
            free(audio_data);
            continue;
        }

        free(audio_data);
    }
}

bool websocket_tx_init(void)
{
    if (!websocket_tx_queue) {
        websocket_tx_queue = xQueueCreate(WS_TX_QUEUE_LENGTH, sizeof(ws_tx_command_t));
        if (!websocket_tx_queue) return false;
    }
    if (!websocket_tx_task_handle) {
        if (xTaskCreatePinnedToCore(websocket_tx_task, "ws_tx", 8192, NULL, 5,
                                    &websocket_tx_task_handle, 1) != pdPASS) return false;
    }
    return true;
}

bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation)
{
    if (!data || len != WS_TX_AUDIO_SIZE || !websocket_tx_queue || !is_connected ||
        !setup_complete || websocket_tx_error || generation != websocket_connection_generation) return false;

    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return false;
    memcpy(copy, data, len);

    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_AUDIO;
    cmd.generation = generation;
    cmd.len = (uint16_t)len;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        ++websocket_tx_drops;
        free(copy);
        return false;
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
    if (xQueueSend(websocket_tx_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGW(TAG, "Setup command queue full");
}

void websocket_app_start(void)
{
    if (!wifi_is_ready() || client || ws_started) return;
    if (!start_audio_playback()) return;
    clear_audio_buffer();
    reset_audio_turn_stats();
    reset_rx_buffer();
    websocket_tx_flush_queue();
    if (!websocket_tx_init() || !websocket_rx_init()) return;

    is_connected = false;
    setup_complete = false;
    websocket_tx_error = false;

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
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                   websocket_event_handler, (void *)client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        client = NULL;
        return;
    }
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        client = NULL;
        return;
    }
    ws_started = true;
    ESP_LOGI(TAG, "Gemini Live client started");
}

bool websocket_is_connected(void)
{
    return is_connected && setup_complete && !websocket_tx_error;
}

void websocket_disconnect(void)
{
    if (client) esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
}

void websocket_reset_started(void)
{
    ws_started = false;
}
