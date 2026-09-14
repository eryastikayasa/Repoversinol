#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "gemini_api_key.h"

#define WEBSOCKET_SERVER_URL \
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" \
    GEMINI_API_KEY

void websocket_app_start(void);
bool websocket_is_connected(void);
bool websocket_healthcheck(void);
uint32_t websocket_get_reconnect_count(void);
UBaseType_t websocket_get_tx_queue_depth(void);
UBaseType_t websocket_get_rx_queue_depth(void);
void websocket_disconnect(void);
bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation);
extern volatile bool websocket_tx_error;
extern uint32_t websocket_connection_generation;
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
