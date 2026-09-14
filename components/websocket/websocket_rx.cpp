#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WS_RX";
static uint8_t *rx_pool[WS_RX_POOL_COUNT] = {0};
static uint8_t *rx_message = NULL;
static volatile bool rx_reset_pending = false;
static size_t rx_received = 0;
static size_t rx_expected = 0;
static bool rx_active = false;
static bool rx_discarding = false;

static bool alloc_pool(void)
{
    if (rx_message) return true;
    rx_message = (uint8_t *)heap_caps_malloc(WS_RX_MAX_PAYLOAD_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_message) rx_message = (uint8_t *)heap_caps_malloc(WS_RX_MAX_PAYLOAD_SIZE + 1, MALLOC_CAP_8BIT);
    if (!rx_message) return false;
    for (int i = 0; i < WS_RX_POOL_COUNT; ++i) {
        rx_pool[i] = (uint8_t *)heap_caps_malloc(WS_RX_FRAGMENT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rx_pool[i]) rx_pool[i] = (uint8_t *)heap_caps_malloc(WS_RX_FRAGMENT_SIZE, MALLOC_CAP_8BIT);
        if (!rx_pool[i]) return false;
    }
    return true;
}

static void release_index(uint8_t index)
{
    if (index < WS_RX_POOL_COUNT) xQueueSend(websocket_rx_free_queue, &index, 0);
}

void reset_rx_buffer(void)
{
    rx_received = 0;
    rx_expected = 0;
    rx_active = false;
    rx_discarding = false;
}

bool ensure_rx_buffer(size_t required_size)
{
    return required_size <= WS_RX_MAX_PAYLOAD_SIZE && rx_message != NULL;
}

static void log_rx_profile(void)
{
    uint32_t avg = websocket_rx_messages ? (uint32_t)(websocket_rx_process_total_us / websocket_rx_messages) : 0;
    ESP_LOGI(TAG, "RX profile: messages=%lu fragments=%lu drops=%lu queue_hwm=%u process_avg_us=%lu process_max_us=%lu",
             (unsigned long)websocket_rx_messages, (unsigned long)websocket_rx_fragments,
             (unsigned long)websocket_rx_drops, (unsigned)websocket_rx_high_water,
             (unsigned long)avg, (unsigned long)websocket_rx_process_max_us);
}

static void websocket_rx_task(void *arg)
{
    (void)arg;
    ws_rx_fragment_t frag = {};
    uint32_t last_log_ms = 0;
    ESP_LOGI(TAG, "RX worker: core=%d priority=%d pool=%d x %u bytes max_payload=%u",
             xPortGetCoreID(), uxTaskPriorityGet(NULL), WS_RX_POOL_COUNT,
             (unsigned)WS_RX_FRAGMENT_SIZE, (unsigned)WS_RX_MAX_PAYLOAD_SIZE);

    for (;;) {
        if (xQueueReceive(websocket_rx_queue, &frag, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (rx_reset_pending) { rx_reset_pending = false; reset_rx_buffer(); }
            continue;
        }
        if (rx_reset_pending) { rx_reset_pending = false; reset_rx_buffer(); }
        if (frag.generation != websocket_connection_generation || !is_connected) {
            release_index(frag.pool_index); continue;
        }

        if (frag.offset == 0) {
            rx_received = 0;
            rx_expected = frag.payload_len;
            rx_active = frag.payload_len <= WS_RX_MAX_PAYLOAD_SIZE;
            rx_discarding = !rx_active;
        }

        bool valid = rx_active && !rx_discarding && frag.payload_len == rx_expected &&
                     frag.offset == rx_received && frag.len <= WS_RX_FRAGMENT_SIZE &&
                     frag.offset + frag.len <= WS_RX_MAX_PAYLOAD_SIZE;
        if (valid) {
            memcpy(rx_message + frag.offset, rx_pool[frag.pool_index], frag.len);
            rx_received = frag.offset + frag.len;
        } else {
            ++websocket_rx_drops;
            rx_discarding = true;
        }

        bool final_fragment = frag.payload_len > 0 && frag.offset + frag.len >= frag.payload_len;
        release_index(frag.pool_index);

        if (final_fragment) {
            if (valid && rx_received == rx_expected) {
                rx_message[rx_received] = '\0';
                int64_t start_us = esp_timer_get_time();
                process_gemini_message((const char *)rx_message, rx_received);
                uint32_t process_us = (uint32_t)(esp_timer_get_time() - start_us);
                websocket_rx_process_total_us += process_us;
                if (process_us > websocket_rx_process_max_us) websocket_rx_process_max_us = process_us;
                ++websocket_rx_messages;
            } else {
                ++websocket_rx_drops;
            }
            reset_rx_buffer();
        }

        UBaseType_t waiting = uxQueueMessagesWaiting(websocket_rx_queue);
        if (waiting > websocket_rx_high_water) websocket_rx_high_water = waiting;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_log_ms >= 5000) { last_log_ms = now_ms; log_rx_profile(); }
    }
}

bool websocket_rx_init(void)
{
    if (!websocket_rx_queue) {
        websocket_rx_queue = xQueueCreate(WS_RX_QUEUE_LENGTH, sizeof(ws_rx_fragment_t));
        if (!websocket_rx_queue) return false;
    }
    if (!websocket_rx_free_queue) {
        websocket_rx_free_queue = xQueueCreate(WS_RX_POOL_COUNT, sizeof(uint8_t));
        if (!websocket_rx_free_queue) return false;
    }
    if (!alloc_pool()) return false;
    if (uxQueueMessagesWaiting(websocket_rx_free_queue) == 0)
        for (uint8_t i = 0; i < WS_RX_POOL_COUNT; ++i) xQueueSend(websocket_rx_free_queue, &i, 0);
    if (!websocket_rx_task_handle) {
        if (xTaskCreatePinnedToCore(websocket_rx_task, "ws_rx", 8192, NULL, 5,
                                    &websocket_rx_task_handle, 0) != pdPASS) return false;
    }
    return true;
}

void websocket_rx_request_reset(void) { rx_reset_pending = true; }

void websocket_rx_flush_queue(void)
{
    if (!websocket_rx_queue) return;
    ws_rx_fragment_t stale = {};
    while (xQueueReceive(websocket_rx_queue, &stale, 0) == pdTRUE) release_index(stale.pool_index);
    reset_rx_buffer();
}

bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data, uint32_t generation)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || data->payload_len <= 0 ||
        data->payload_offset < 0 || generation != websocket_connection_generation || !is_connected) {
        ++websocket_rx_drops; return false;
    }
    size_t len = (size_t)data->data_len;
    if (len > WS_RX_FRAGMENT_SIZE) { ++websocket_rx_drops; return false; }

    uint8_t index = 0;
    if (xQueueReceive(websocket_rx_free_queue, &index, 0) != pdTRUE) {
        ++websocket_rx_drops; return false;
    }
    memcpy(rx_pool[index], data->data_ptr, len);

    ws_rx_fragment_t frag = {};
    frag.generation = generation;
    frag.payload_len = (uint32_t)data->payload_len;
    frag.offset = (uint32_t)data->payload_offset;
    frag.len = (uint16_t)len;
    frag.pool_index = index;
    if (xQueueSend(websocket_rx_queue, &frag, 0) != pdTRUE) {
        release_index(index); ++websocket_rx_drops; return false;
    }
    ++websocket_rx_fragments;
    UBaseType_t waiting = uxQueueMessagesWaiting(websocket_rx_queue);
    if (waiting > websocket_rx_high_water) websocket_rx_high_water = waiting;
    return true;
}

void process_websocket_payload(esp_websocket_event_data_t *data)
{
    websocket_rx_enqueue_data(data, websocket_connection_generation);
}
