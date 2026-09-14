#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stddef.h>

static const char *TAG = "MAIN";
static constexpr size_t AUDIO_FRAME_BYTES = 640; // 320 samples * 2 bytes = 20 ms

static void audio_capture_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AUDIO_FRAME_BYTES];
    uint64_t frame_count = 0;

    ESP_LOGI(TAG, "MIC capture started: PCM16 mono 16kHz, frame=%u bytes/20ms",
             (unsigned)AUDIO_FRAME_BYTES);

    for (;;) {
        size_t got = audio_read_mic(frame, sizeof(frame));
        if (got != sizeof(frame)) {
            ESP_LOGW(TAG, "MIC short read: %u/%u bytes", (unsigned)got,
                     (unsigned)sizeof(frame));
            continue;
        }

        ++frame_count;
        if (websocket_is_connected()) {
            websocket_send_audio_data(frame, sizeof(frame));
        }

        if ((frame_count % 250U) == 0U) {
            ESP_LOGI(TAG, "MIC: frames=%llu", (unsigned long long)frame_count);
        }
    }
}

static void session_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (wifi_is_ready() && !websocket_is_connected()) {
            websocket_app_start();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "RepoNol Voice Engine starting");

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
    ESP_LOGI(TAG, "WiFi ready; power save disabled");

    websocket_app_start();

    BaseType_t audio_ok = xTaskCreatePinnedToCore(
        audio_capture_task, "audio_capture", 4096, nullptr, 5, nullptr, 1);
    BaseType_t session_ok = xTaskCreatePinnedToCore(
        session_supervisor_task, "session_supervisor", 3072, nullptr, 2, nullptr, 0);

    if (audio_ok != pdPASS || session_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice engine tasks");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Voice engine ready: WiFi -> Gemini Live -> MIC/Speaker");
    vTaskDelete(nullptr);
}
