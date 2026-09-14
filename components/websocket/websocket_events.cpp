#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "WS_EVENT";
static bool lifecycle_invalidated = false;

static void invalidate_connection_generation(void)
{
    if (lifecycle_invalidated) return;
    lifecycle_invalidated = true;
    ++websocket_connection_generation;
}

static void invalidate_live_session(const char *reason)
{
    is_connected = false;
    setup_complete = false;
    websocket_tx_error = true;
    invalidate_connection_generation();
    websocket_tx_flush_queue();
    websocket_rx_flush_queue();
    websocket_rx_request_reset();
    ESP_LOGW(TAG, "Gemini Live session invalidated: %s", reason ? reason : "unknown");
}

void websocket_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t event_client =
        (esp_websocket_client_handle_t)handler_args;

    /* Ignore late events from an old client after a reconnect has started. */
    if (client && event_client && event_client != client) return;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            lifecycle_invalidated = false;
            is_connected = true;
            setup_complete = false;
            websocket_tx_error = false;
            ++websocket_connection_generation;
            websocket_note_connected();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();

            ESP_LOGI(TAG, "WebSocket CONNECTED generation=%lu",
                     (unsigned long)websocket_connection_generation);

            /*
             * Google requires SETUP to be the first client message and requires
             * the client to wait for setupComplete before sending additional
             * messages. The TX worker is the only task allowed to send frames.
             */
            websocket_schedule_setup(websocket_connection_generation);
            break;

        case WEBSOCKET_EVENT_DATA:
            /* Callback only copies the fragment. JSON/audio work stays outside it. */
            if (!data || !is_connected || websocket_tx_error) break;
            if ((data->op_code == 0x00 || data->op_code == 0x01 || data->op_code == 0x02) &&
                data->data_ptr && data->data_len > 0) {
                websocket_rx_enqueue_data(data, websocket_connection_generation);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket ERROR");
            if (data) {
                ESP_LOGE(TAG, "type=%d errno=%d tls=0x%x stack=0x%x handshake=%d",
                         (int)data->error_handle.error_type,
                         data->error_handle.esp_transport_sock_errno,
                         (unsigned)data->error_handle.esp_tls_last_esp_err,
                         (unsigned)data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_ws_handshake_status_code);
            }
            invalidate_live_session("transport/websocket error");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket DISCONNECTED/CLOSED");
            invalidate_live_session("connection closed");
            break;

        case WEBSOCKET_EVENT_FINISH: {
            ESP_LOGI(TAG, "WebSocket FINISH");
            esp_websocket_client_handle_t old_client = client;
            client = NULL;
            websocket_reset_started();
            if (old_client) websocket_cleanup_finished(old_client);
            break;
        }

        default:
            break;
    }
}
