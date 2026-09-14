#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SSID "Qrick WiFi"
#define WIFI_PASS "1komalima"

void wifi_init_sta(void);
bool wifi_wait_for_connection(uint32_t timeout_ms);
bool wifi_is_ready(void);
uint32_t wifi_get_reconnect_count(void);
