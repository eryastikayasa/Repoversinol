#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gemini_api_key.h"

#define WEBSOCKET_SERVER_URL \
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" \
    GEMINI_API_KEY

void websocket_app_start(void);
bool websocket_is_connected(void);
void websocket_disconnect(void);
