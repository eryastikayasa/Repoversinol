#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WS_EVENT";
static volatile bool lifecycle_invalidated = false;
static TaskHandle_t cleanup_task_handle = NULL;

static void invalidate_connection_generation(void)
{
    if (lifecycle_invalidated) return;
    lifecycle_invalidated = true;
    ++websocket_connection_generation;
    ESP_LOGW(TAG, "Connection generation invalidated: %lu",
             (unsigned long)websocket_connection_generation);
}

static void websocket_cleanup_task(void *arg)
{
    esp_websocket_client_handle_t ws = (esp_websocket_client_handle_t)arg;
    vTaskDelay(pdMS_TO_TICKS(10));

    if (ws != NULL && client == ws && !esp_websocket_client_is_connected(ws)) {
        ESP_LOGI(TAG, "Cleanup worker: destroy client after FINISH");
        esp_err_t err = esp_websocket_client_destroy(ws);
        if (err == ESP_OK) {
            client = NULL;
            websocket_reset_started();
        } else {
            ESP_LOGW(TAG, "Cleanup worker: destroy failed err=0x%x", (unsigned)err);
        }
    }

    cleanup_task_handle = NULL;
    vTaskDelete(NULL);
}

void websocket_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t event_client =
        (esp_websocket_client_handle_t)handler_args;

    if (client != NULL && event_client != NULL && event_client != client) {
        ESP_LOGW(TAG, "Event from stale client ignored: event=%ld", (long)event_id);
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            lifecycle_invalidated = false;
            is_connected = true;
            setup_complete = false;
            websocket_tx_error = false;
            ++websocket_connection_generation;
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            ESP_LOGI(TAG, "WebSocket CONNECTED generation=%lu",
                     (unsigned long)websocket_connection_generation);
            websocket_schedule_setup(websocket_connection_generation);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (!data || !is_connected || websocket_tx_error) break;
            if (data->op_code == 0x08) {
                ESP_LOGW(TAG, "Gemini CLOSE frame");
                if (data->data_ptr && data->data_len >= 2) {
                    uint16_t code = ((uint8_t)data->data_ptr[0] << 8) |
                                    (uint8_t)data->data_ptr[1];
                    ESP_LOGW(TAG, "CLOSE code=%u", (unsigned)code);
                }
                break;
            }
            if ((data->op_code == 0x00 || data->op_code == 0x01 || data->op_code == 0x02) &&
                data->data_ptr && data->data_len > 0) {
                (void)websocket_rx_enqueue_data(data, websocket_connection_generation);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket ERROR");
            if (data) {
                ESP_LOGE(TAG,
                         "error_type=%d sock_errno=%d tls_esp=0x%x tls_stack=0x%x handshake=%d",
                         (int)data->error_handle.error_type,
                         data->error_handle.esp_transport_sock_errno,
                         (unsigned)data->error_handle.esp_tls_last_esp_err,
                         (unsigned)data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_ws_handshake_status_code);
            }
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket disconnected/closed");
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            break;

        case WEBSOCKET_EVENT_FINISH:
            ESP_LOGI(TAG, "WebSocket FINISH");
            if (cleanup_task_handle == NULL && client != NULL) {
                esp_websocket_client_handle_t ws = client;
                if (xTaskCreate(websocket_cleanup_task, "ws_cleanup", 3072,
                                (void *)ws, 3, &cleanup_task_handle) != pdPASS) {
                    ESP_LOGE(TAG, "Gagal membuat cleanup task");
                }
            }
            break;

        default:
            break;
    }
}
