#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gemini_api_key.h"

#define WEBSOCKET_SERVER_URL \
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" \
    GEMINI_API_KEY

void websocket_app_start(void);

/* Returns true only when the 20 ms PCM16 frame was accepted by the bounded
 * transport queue. A false result is a measured TX drop, not a hidden wait. */
bool websocket_send_audio_data(const uint8_t *data, size_t len);

bool websocket_is_connected(void);
