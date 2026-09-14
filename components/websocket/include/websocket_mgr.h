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
void websocket_disconnect(void);
bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation);
extern uint32_t websocket_connection_generation;
extern uint32_t websocket_tx_frames;
extern uint64_t websocket_tx_bytes;
extern uint32_t websocket_tx_drops;
extern UBaseType_t websocket_tx_high_water;
extern uint64_t websocket_tx_write_total_us;
extern uint32_t websocket_tx_write_max_us;
extern uint32_t websocket_rx_messages;
extern uint32_t websocket_rx_fragments;
extern uint32_t websocket_rx_drops;
extern UBaseType_t websocket_rx_high_water;
extern uint64_t websocket_rx_process_total_us;
extern uint32_t websocket_rx_process_max_us;
