#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
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

static const char *TAG = "MAIN";
static constexpr size_t AUDIO_FRAME_BYTES = 640;

static void audio_capture_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AUDIO_FRAME_BYTES];
    uint64_t frames = 0;
    int64_t last_us = 0;
    uint64_t interval_total_us = 0;
    uint32_t interval_max_us = 0;
    uint32_t drops = 0;
    int64_t last_log_us = esp_timer_get_time();

    ESP_LOGI(TAG, "MIC: PCM16 mono 16kHz, 320 samples / 640 bytes / 20ms");

    for (;;) {
        size_t got = audio_read_mic(frame, sizeof(frame));
        int64_t now_us = esp_timer_get_time();
        if (last_us != 0) {
            uint32_t interval = (uint32_t)(now_us - last_us);
            interval_total_us += interval;
            if (interval > interval_max_us) interval_max_us = interval;
        }
        last_us = now_us;

        if (got != sizeof(frame)) {
            ++drops;
            continue;
        }

        ++frames;
        if (websocket_is_connected() && !websocket_tx_enqueue_audio(frame, sizeof(frame), websocket_connection_generation))
            ++drops;

        if (now_us - last_log_us >= 5000000) {
            last_log_us = now_us;
            uint32_t avg_interval = frames > 1 ? (uint32_t)(interval_total_us / (frames - 1)) : 0;
            ESP_LOGI(TAG,
                     "MIC profile: frames=%llu interval_avg_us=%lu interval_max_us=%lu drops=%lu heap=%u psram=%u",
                     (unsigned long long)frames,
                     (unsigned long)avg_interval,
                     (unsigned long)interval_max_us,
                     (unsigned long)drops,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            uint32_t tx_avg = websocket_tx_frames ?
                (uint32_t)(websocket_tx_write_total_us / websocket_tx_frames) : 0;
            uint32_t rx_avg = websocket_rx_messages ?
                (uint32_t)(websocket_rx_process_total_us / websocket_rx_messages) : 0;
            ESP_LOGI(TAG,
                     "VOICE profile: TX frames=%lu bytes=%llu drops=%lu q_hwm=%u write_avg_us=%lu write_max_us=%lu | RX msg=%lu frag=%lu drops=%lu q_hwm=%u process_avg_us=%lu process_max_us=%lu",
                     (unsigned long)websocket_tx_frames,
                     (unsigned long long)websocket_tx_bytes,
                     (unsigned long)websocket_tx_drops,
                     (unsigned)websocket_tx_high_water,
                     (unsigned long)tx_avg,
                     (unsigned long)websocket_tx_write_max_us,
                     (unsigned long)websocket_rx_messages,
                     (unsigned long)websocket_rx_fragments,
                     (unsigned long)websocket_rx_drops,
                     (unsigned)websocket_rx_high_water,
                     (unsigned long)rx_avg,
                     (unsigned long)websocket_rx_process_max_us);
        }
    }
}

static void session_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (wifi_is_ready() && !websocket_is_connected()) websocket_app_start();
        vTaskDelay(pdMS_TO_TICKS(2000));
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
    if (!wifi_wait_for_connection(15000)) {
        ESP_LOGE(TAG, "WiFi failed to obtain IP");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    websocket_app_start();

    if (xTaskCreatePinnedToCore(audio_capture_task, "audio_capture", 4096, NULL, 5, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(session_supervisor_task, "session_supervisor", 3072, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Voice task creation failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "VOICE ENGINE READY: WiFi -> Gemini Live -> MIC/Speaker");
    vTaskDelete(NULL);
}
