#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stddef.h>

#ifndef VOICE_SYNTHETIC_TEST
#define VOICE_SYNTHETIC_TEST 0
#endif

static const char *TAG = "MAIN";
static constexpr size_t AUDIO_FRAME_BYTES = 640;

static void audio_capture_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AUDIO_FRAME_BYTES];
#if VOICE_SYNTHETIC_TEST
    static const int16_t tone[16] = {0, 2296, 4243, 5543, 6000, 5543, 4243, 2296,
                                     0, -2296, -4243, -5543, -6000, -5543, -4243, -2296};
    static size_t tone_pos = 0;
#endif
    uint64_t frames = 0;
    int64_t last_us = 0;
    uint64_t interval_total_us = 0;
    uint32_t interval_max_us = 0;
    uint32_t drops = 0;
    int64_t last_log_us = esp_timer_get_time();

#if VOICE_SYNTHETIC_TEST
    ESP_LOGW(TAG, "TEST A ENABLED: synthetic PCM16 16kHz mono, 640 bytes every 20ms");
#else
    ESP_LOGI(TAG, "MIC: PCM16 mono 16kHz, 320 samples / 640 bytes / 20ms");
#endif

    for (;;) {
#if VOICE_SYNTHETIC_TEST
        int16_t *pcm = reinterpret_cast<int16_t *>(frame);
        for (size_t i = 0; i < AUDIO_FRAME_BYTES / sizeof(int16_t); ++i) {
            pcm[i] = tone[tone_pos++ & 15];
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t now_us = esp_timer_get_time();
#else
        size_t got = audio_read_mic(frame, sizeof(frame));
        int64_t now_us = esp_timer_get_time();
        if (got != sizeof(frame)) { ++drops; continue; }
#endif

        if (last_us != 0) {
            uint32_t interval = (uint32_t)(now_us - last_us);
            interval_total_us += interval;
            if (interval > interval_max_us) interval_max_us = interval;
        }
        last_us = now_us;

        ++frames;
        if (websocket_is_connected() &&
            !websocket_tx_enqueue_audio(frame, sizeof(frame), websocket_connection_generation)) {
            ++drops;
        }

        if (now_us - last_log_us >= 5000000) {
            last_log_us = now_us;
            uint32_t avg_interval = frames > 1 ? (uint32_t)(interval_total_us / (frames - 1)) : 0;
            uint32_t tx_encode_avg = websocket_tx_encode_count ?
                (uint32_t)(websocket_tx_encode_total_us / websocket_tx_encode_count) : 0;
            uint32_t tx_json_avg = websocket_tx_json_count ?
                (uint32_t)(websocket_tx_json_total_us / websocket_tx_json_count) : 0;
            uint32_t tx_write_avg = websocket_tx_write_count ?
                (uint32_t)(websocket_tx_write_total_us / websocket_tx_write_count) : 0;
            uint32_t rx_avg = websocket_rx_messages ?
                (uint32_t)(websocket_rx_process_total_us / websocket_rx_messages) : 0;
            ESP_LOGI(TAG, "MIC profile: frames=%llu interval_avg_us=%lu interval_max_us=%lu drops=%lu heap=%u psram=%u",
                     (unsigned long long)frames, (unsigned long)avg_interval,
                     (unsigned long)interval_max_us, (unsigned long)drops,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "TX profile: q=%u/3 hwm=%u frames=%lu bytes=%llu drops=%lu | encode=%lu avg=%lu max=%lu | json=%lu avg=%lu max=%lu | write=%lu avg=%lu max=%lu slow=%lu fail=%lu timeout=%lu",
                     (unsigned)websocket_get_tx_queue_depth(),
                     (unsigned)websocket_tx_high_water,
                     (unsigned long)websocket_tx_frames,
                     (unsigned long long)websocket_tx_bytes,
                     (unsigned long)websocket_tx_drops,
                     (unsigned long)websocket_tx_encode_count,
                     (unsigned long)tx_encode_avg,
                     (unsigned long)websocket_tx_encode_max_us,
                     (unsigned long)websocket_tx_json_count,
                     (unsigned long)tx_json_avg,
                     (unsigned long)websocket_tx_json_max_us,
                     (unsigned long)websocket_tx_write_count,
                     (unsigned long)tx_write_avg,
                     (unsigned long)websocket_tx_write_max_us,
                     (unsigned long)websocket_tx_write_slow,
                     (unsigned long)websocket_tx_write_fail,
                     (unsigned long)websocket_tx_write_timeout);
            ESP_LOGI(TAG, "RX profile: q=%u/4 hwm=%u msg=%lu frag=%lu drops=%lu process_avg_us=%lu process_max_us=%lu | WiFi reconnect=%lu WS reconnect=%lu turns=%lu resume=%s goAway=%lu",
                     (unsigned)websocket_get_rx_queue_depth(),
                     (unsigned)websocket_rx_high_water,
                     (unsigned long)websocket_rx_messages,
                     (unsigned long)websocket_rx_fragments,
                     (unsigned long)websocket_rx_drops,
                     (unsigned long)rx_avg,
                     (unsigned long)websocket_rx_process_max_us,
                     (unsigned long)wifi_get_reconnect_count(),
                     (unsigned long)websocket_get_reconnect_count(),
                     (unsigned long)websocket_turn_count,
                     session_resumable ? "yes" : "no",
                     (unsigned long)websocket_goaway_count);
        }
    }
}

static void session_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool wifi_ready = wifi_is_ready();

        if (!wifi_ready) {
            websocket_disconnect();
        } else if (websocket_goaway_reconnect_pending() && client) {
            // Gemini goAway is a proactive warning. Keep the existing client alive and
            // let the server close it; esp_websocket_client then auto-reconnects. The
            // CONNECTED event sends setup with the preserved session-resumption handle.
            ESP_LOGW(TAG, "WS supervisor: goAway acknowledged; waiting for reconnect, resume=%s",
                     session_resumable ? "yes" : "no");
            websocket_clear_goaway_reconnect();
        } else if (websocket_tx_error) {
            // With auto reconnect enabled, keep the existing client alive. The client
            // will emit CONNECTED again; that event queues a fresh setup using the
            // preserved session-resumption handle.
            if (!client) websocket_app_start();
        } else {
            (void)websocket_healthcheck();
            if (!client) websocket_app_start();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "RepoNol Voice Engine starting");
    ESP_LOGI(TAG, "Heap=%u PSRAM=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    audio_hal_init();
    wifi_init_sta();
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (!wifi_wait_for_connection(5000))
        ESP_LOGW(TAG, "Wi-Fi not ready yet; continuing with offline-safe voice workers");

    if (xTaskCreatePinnedToCore(audio_capture_task, "audio_capture", 4096, NULL, 5, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(session_supervisor_task, "session_supervisor", 3072, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Voice task creation failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "VOICE ENGINE READY: WiFi -> Gemini Live -> MIC/Speaker");
    vTaskDelete(NULL);
}
