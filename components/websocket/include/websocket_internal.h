#pragma once
#include "websocket_mgr.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_HANDLE_MAX_LEN 1024
#define WS_TX_AUDIO_SIZE 640
#define WS_TX_QUEUE_LENGTH 3
#define WS_RX_MAX_PAYLOAD_SIZE (48 * 1024)
#define WS_RX_FRAGMENT_SIZE (8 * 1024)
#define WS_RX_POOL_COUNT 4
#define WS_RX_QUEUE_LENGTH WS_RX_POOL_COUNT
#define WS_TX_AUDIO_SEND_TIMEOUT_MS 10
#define WS_TX_AUDIO_SLOW_THRESHOLD_US 30000
#define WS_TX_RETRY_WINDOW_MS 3000

typedef enum { WS_TX_COMMAND_SETUP = 1, WS_TX_COMMAND_AUDIO = 2 } ws_tx_command_type_t;
typedef enum {
    LIVE_ST_DISCONNECTED = 0,
    LIVE_ST_CONNECTING,
    LIVE_ST_SETUP_SENT,
    LIVE_ST_READY,
    LIVE_ST_SPEAKING,
    LIVE_ST_RECONNECTING
} live_state_t;

typedef struct {
    ws_tx_command_type_t type;
    uint32_t generation;
    uint16_t len;
    uint8_t data[WS_TX_AUDIO_SIZE];
} ws_tx_command_t;
extern QueueHandle_t websocket_tx_queue;
extern TaskHandle_t websocket_tx_task_handle;
bool websocket_tx_init(void);
bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation);
void websocket_tx_flush_queue(void);

typedef struct {
    uint32_t generation;
    uint32_t payload_len;
    uint32_t offset;
    uint16_t len;
    uint8_t pool_index;
} ws_rx_fragment_t;
extern QueueHandle_t websocket_rx_queue;
extern QueueHandle_t websocket_rx_free_queue;
extern TaskHandle_t websocket_rx_task_handle;
bool websocket_rx_init(void);
void websocket_rx_request_reset(void);
void websocket_rx_flush_queue(void);
bool websocket_rx_enqueue_data(esp_websocket_event_data_t *data, uint32_t generation);

extern esp_websocket_client_handle_t client;
extern volatile bool is_connected;
extern volatile bool setup_complete;
extern volatile bool websocket_tx_error;
extern volatile live_state_t websocket_live_state;
extern uint32_t websocket_connection_generation;
extern char session_handle[SESSION_HANDLE_MAX_LEN];
extern bool session_resumable;
extern uint32_t websocket_turn_count;
extern uint32_t websocket_goaway_count;

extern StreamBufferHandle_t audio_stream;
extern TaskHandle_t audio_playback_task_handle;
extern volatile bool audio_turn_active;
extern volatile bool audio_turn_complete_pending;
extern uint32_t audio_chunks_received;
extern uint64_t audio_bytes_received;
extern uint64_t audio_bytes_queued;
extern uint32_t audio_write_calls;
extern uint64_t audio_bytes_played;
extern uint64_t audio_bytes_dropped;

extern uint32_t websocket_tx_frames;
extern uint64_t websocket_tx_bytes;
extern uint32_t websocket_tx_drops;
extern UBaseType_t websocket_tx_high_water;
extern uint32_t websocket_tx_encode_count;
extern uint64_t websocket_tx_encode_total_us;
extern uint32_t websocket_tx_encode_max_us;
extern uint32_t websocket_tx_json_count;
extern uint64_t websocket_tx_json_total_us;
extern uint32_t websocket_tx_json_max_us;
extern uint32_t websocket_tx_write_count;
extern uint64_t websocket_tx_write_total_us;
extern uint32_t websocket_tx_write_max_us;
extern uint32_t websocket_tx_write_slow;
extern uint32_t websocket_tx_write_fail;
extern uint32_t websocket_tx_write_timeout;
extern uint32_t websocket_rx_messages;
extern uint32_t websocket_rx_fragments;
extern uint32_t websocket_rx_drops;
extern UBaseType_t websocket_rx_high_water;
extern uint64_t websocket_rx_process_total_us;
extern uint32_t websocket_rx_process_max_us;

void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void websocket_schedule_setup(uint32_t generation);
void websocket_cleanup_finished(esp_websocket_client_handle_t old_client);
void websocket_note_connected(void);
void reset_rx_buffer(void);
bool ensure_rx_buffer(size_t required_size);
void process_websocket_payload(esp_websocket_event_data_t *data);
bool build_gemini_setup(char **output, size_t *output_len);
void process_gemini_message(const char *json, size_t len);
void clear_session_handle(void);
bool store_session_handle(const char *handle);
size_t get_audio_pending_bytes(void);
bool start_audio_playback(void);
void clear_audio_buffer(void);
void request_audio_buffer_clear(void);
void reset_audio_turn_stats(void);
void begin_audio_turn(void);
bool queue_audio_pcm(const uint8_t *pcm, size_t len);
void check_audio_playback_complete(void);
void websocket_disconnect(void);
void websocket_reset_started(void);
