#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_TX";
static uint8_t tx_pool[WS_TX_POOL_COUNT][WS_TX_AUDIO_SIZE];
static bool tx_pool_used[WS_TX_POOL_COUNT] = {false};
static StaticSemaphore_t tx_pool_mutex_storage;
static SemaphoreHandle_t tx_pool_mutex = NULL;
static uint64_t tx_send_count = 0;
static uint64_t tx_send_time_us = 0;
static uint32_t tx_send_max_us = 0;
static uint64_t tx_queue_drops = 0;
static UBaseType_t tx_queue_hwm = 0;
static int64_t tx_last_profile_us = 0;

static int tx_pool_acquire(void) {
    if (!tx_pool_mutex || xSemaphoreTake(tx_pool_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return -1;
    int slot = -1;
    for (int i = 0; i < WS_TX_POOL_COUNT; ++i) if (!tx_pool_used[i]) { tx_pool_used[i] = true; slot = i; break; }
    xSemaphoreGive(tx_pool_mutex);
    return slot;
}
static void tx_pool_release(uint8_t slot) {
    if (slot >= WS_TX_POOL_COUNT || !tx_pool_mutex) return;
    if (xSemaphoreTake(tx_pool_mutex, pdMS_TO_TICKS(5)) == pdTRUE) { tx_pool_used[slot] = false; xSemaphoreGive(tx_pool_mutex); }
}
void websocket_tx_flush_queue(void) {
    if (!websocket_tx_queue) return;
    ws_tx_command_t stale = {};
    size_t flushed = 0;
    while (xQueueReceive(websocket_tx_queue, &stale, 0) == pdTRUE) { if (stale.pool_id != WS_TX_POOL_NONE) tx_pool_release(stale.pool_id); ++flushed; }
    if (flushed) ESP_LOGW(TAG, "TX queue flushed: %u command", (unsigned)flushed);
}
static void websocket_tx_fail(void) {
    websocket_tx_error = true;
    is_connected = false;
    setup_complete = false;
    ++websocket_connection_generation;
    websocket_tx_flush_queue();
    ESP_LOGW(TAG, "TX transport failure: generation=%lu", (unsigned long)websocket_connection_generation);
}
static void tx_profile_log(void) {
    int64_t now = esp_timer_get_time();
    if (tx_last_profile_us == 0) tx_last_profile_us = now;
    if (now - tx_last_profile_us < 5000000LL) return;
    tx_last_profile_us = now;
    uint64_t avg = tx_send_count ? tx_send_time_us / tx_send_count : 0;
    UBaseType_t depth = websocket_tx_queue ? uxQueueMessagesWaiting(websocket_tx_queue) : 0;
    if (depth > tx_queue_hwm) tx_queue_hwm = depth;
    ESP_LOGI(TAG, "TX PROFILE: count=%llu send_avg_us=%llu send_max_us=%u queue_depth=%u queue_hwm=%u drops=%llu heap_internal=%u",
             (unsigned long long)tx_send_count, (unsigned long long)avg, (unsigned)tx_send_max_us,
             (unsigned)depth, (unsigned)tx_queue_hwm, (unsigned long long)tx_queue_drops,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
static void websocket_tx_task(void *arg) {
    (void)arg;
    ws_tx_command_t cmd = {};
    static char b64_buf[1024];
    static char json_buf[1400];
    ESP_LOGI(TAG, "TX worker: 20ms PCM frame, bounded queue=%u", (unsigned)WS_TX_QUEUE_LENGTH);
    for (;;) {
        if (xQueueReceive(websocket_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *audio_data = cmd.data;
        uint8_t pool_id = cmd.pool_id;
        esp_websocket_client_handle_t ws = client;
        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || !ws || !esp_websocket_client_is_connected(ws)) {
            if (pool_id != WS_TX_POOL_NONE) tx_pool_release(pool_id);
            memset(&cmd, 0, sizeof(cmd));
            continue;
        }
        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup_json = NULL; size_t setup_len = 0;
            if (build_gemini_setup(&setup_json, &setup_len)) {
                int64_t start = esp_timer_get_time();
                int sent = esp_websocket_client_send_text(ws, setup_json, (int)setup_len, pdMS_TO_TICKS(1000));
                int64_t elapsed = esp_timer_get_time() - start;
                ESP_LOGI(TAG, "SETUP TX: sent=%d expected=%u elapsed_us=%lld", sent, (unsigned)setup_len, (long long)elapsed);
                if (sent != (int)setup_len) websocket_tx_fail();
                free(setup_json);
            }
            memset(&cmd, 0, sizeof(cmd));
            continue;
        }
        if (cmd.type == WS_TX_COMMAND_AUDIO && audio_data && cmd.len == WS_TX_AUDIO_SIZE) {
            size_t encoded_len = 0;
            int ret = mbedtls_base64_encode((unsigned char *)b64_buf, sizeof(b64_buf) - 1, &encoded_len, audio_data, cmd.len);
            if (ret == 0) {
                b64_buf[encoded_len] = '\0';
                int json_len = snprintf(json_buf, sizeof(json_buf), "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", b64_buf);
                if (json_len > 0 && (size_t)json_len < sizeof(json_buf)) {
                    int64_t start = esp_timer_get_time();
                    int sent = esp_websocket_client_send_text(ws, json_buf, json_len, pdMS_TO_TICKS(250));
                    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
                    ++tx_send_count; tx_send_time_us += elapsed; if (elapsed > tx_send_max_us) tx_send_max_us = elapsed;
                    if (sent != json_len) {
                        ESP_LOGW(TAG, "TX WRITE FAIL: sent=%d expected=%d elapsed_us=%u", sent, json_len, (unsigned)elapsed);
                        websocket_tx_fail();
                    }
                } else ESP_LOGW(TAG, "TX JSON build failed len=%d", json_len);
            } else ESP_LOGW(TAG, "TX base64 encode failed ret=%d", ret);
        }
        if (pool_id != WS_TX_POOL_NONE) tx_pool_release(pool_id);
        memset(&cmd, 0, sizeof(cmd));
        tx_profile_log();
        vTaskDelay(1);
    }
}
bool websocket_tx_init(void) {
    if (!tx_pool_mutex) { tx_pool_mutex = xSemaphoreCreateMutexStatic(&tx_pool_mutex_storage); if (!tx_pool_mutex) return false; }
    if (!websocket_tx_queue) { websocket_tx_queue = xQueueCreate(WS_TX_QUEUE_LENGTH, sizeof(ws_tx_command_t)); if (!websocket_tx_queue) return false; }
    if (!websocket_tx_task_handle) if (xTaskCreate(websocket_tx_task, "ws_tx", 8192, NULL, 4, &websocket_tx_task_handle) != pdPASS) return false;
    return true;
}
bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation) {
    if (!data || len != WS_TX_AUDIO_SIZE || !websocket_tx_queue || !is_connected || !setup_complete || websocket_tx_error || generation != websocket_connection_generation) return false;
    int slot = tx_pool_acquire();
    if (slot < 0) { ++tx_queue_drops; return false; }
    memcpy(tx_pool[slot], data, WS_TX_AUDIO_SIZE);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_AUDIO; cmd.generation = generation; cmd.len = WS_TX_AUDIO_SIZE; cmd.pool_id = (uint8_t)slot; cmd.data = tx_pool[slot];
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) { tx_pool_release((uint8_t)slot); ++tx_queue_drops; return false; }
    UBaseType_t depth = uxQueueMessagesWaiting(websocket_tx_queue); if (depth > tx_queue_hwm) tx_queue_hwm = depth;
    return true;
}
void websocket_schedule_setup(uint32_t generation) {
    if (!websocket_tx_queue || !is_connected || websocket_tx_error || generation != websocket_connection_generation) return;
    ws_tx_command_t cmd = {}; cmd.type = WS_TX_COMMAND_SETUP; cmd.generation = generation; cmd.pool_id = WS_TX_POOL_NONE;
    if (xQueueSend(websocket_tx_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) ESP_LOGW(TAG, "Setup TX queue full");
}
bool websocket_send_audio_data(const uint8_t *data, size_t len) {
    if (!data || len != WS_TX_AUDIO_SIZE) return false;
    if (!is_connected || !setup_complete || websocket_tx_error) return false;
    return websocket_tx_enqueue_audio(data, len, websocket_connection_generation);
}
