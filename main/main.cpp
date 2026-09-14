#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "VOICE_REF";
#define MIC_FRAME_BYTES 640U
#define MIC_FRAME_INTERVAL_US 20000LL
#define WIFI_WAIT_TIMEOUT_MS 15000U
#ifndef VOICE_REFERENCE_SYNTHETIC_TX
#define VOICE_REFERENCE_SYNTHETIC_TX 0
#endif

static void sync_network_time(void)
{
    ESP_LOGI(TAG, "NTP sync dimulai");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "id.pool.ntp.org");
    esp_sntp_init();
    for (int retry = 0; retry < 20; ++retry) {
        time_t now = 0; struct tm info = {};
        time(&now); localtime_r(&now, &info);
        if (info.tm_year >= (2024 - 1900)) { ESP_LOGI(TAG, "NTP READY: year=%d", info.tm_year + 1900); return; }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    struct timeval fallback = { .tv_sec = 1770000000, .tv_usec = 0 };
    settimeofday(&fallback, NULL);
    ESP_LOGW(TAG, "NTP timeout; fallback time installed");
}

static bool frame_has_activity(const uint8_t *data, size_t len)
{
    if (!data || len < 2) return false;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int16_t sample = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
        int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude >= 200) return true;
    }
    return false;
}

static void log_voice_profile(uint64_t frames, uint64_t tx_ok, uint64_t tx_drop,
                              uint64_t interval_sum_us, uint32_t interval_max_us,
                              uint64_t interval_count)
{
    ESP_LOGI(TAG,
             "MIC PROFILE: frames=%llu tx_ok=%llu tx_drop=%llu interval_avg_us=%llu interval_max_us=%u heap=%u internal=%u psram=%u",
             (unsigned long long)frames, (unsigned long long)tx_ok, (unsigned long long)tx_drop,
             (unsigned long long)(interval_count ? interval_sum_us / interval_count : 0),
             (unsigned)interval_max_us,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

#if VOICE_REFERENCE_SYNTHETIC_TX
static void synthetic_transport_task(void *arg)
{
    (void)arg;
    static uint8_t frame[MIC_FRAME_BYTES];
    uint32_t phase = 0; uint64_t frames = 0; uint64_t drops = 0;
    int64_t next_deadline = esp_timer_get_time();
    ESP_LOGI(TAG, "TEST A: synthetic PCM TX transport mode ENABLED");
    for (;;) {
        int16_t *pcm = reinterpret_cast<int16_t *>(frame);
        for (size_t i = 0; i < MIC_FRAME_BYTES / sizeof(int16_t); ++i) pcm[i] = ((phase++ % 40U) < 20U) ? 1200 : -1200;
        if (websocket_is_connected()) {
            if (websocket_send_audio_data(frame, sizeof(frame))) ++frames; else ++drops;
        }
        next_deadline += MIC_FRAME_INTERVAL_US;
        int64_t wait_us = next_deadline - esp_timer_get_time();
        if (wait_us > 0) {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000));
            if (ticks > 0) vTaskDelay(ticks);
        } else { next_deadline = esp_timer_get_time(); vTaskDelay(1); }
        if ((frames + drops) % 250 == 0)
            ESP_LOGI(TAG, "TEST A: frames=%llu drops=%llu", (unsigned long long)frames, (unsigned long long)drops);
    }
}
#endif

static void microphone_task(void *arg)
{
    (void)arg;
    static uint8_t frame[MIC_FRAME_BYTES];
    uint64_t frames = 0, tx_ok = 0, tx_drop = 0, interval_sum_us = 0, interval_count = 0;
    uint32_t interval_max_us = 0;
    int64_t previous_capture_us = 0, last_profile_us = esp_timer_get_time();
    ESP_LOGI(TAG, "MIC task: PCM16 mono 16kHz, frame=%u bytes/20ms", MIC_FRAME_BYTES);

    for (;;) {
        size_t got = audio_read_mic(frame, sizeof(frame));
        if (got != sizeof(frame)) { ESP_LOGW(TAG, "MIC short read: %u/%u", (unsigned)got, (unsigned)sizeof(frame)); continue; }
        int64_t now = esp_timer_get_time();
        if (previous_capture_us != 0) {
            uint32_t interval = (uint32_t)(now - previous_capture_us);
            interval_sum_us += interval; ++interval_count; if (interval > interval_max_us) interval_max_us = interval;
        }
        previous_capture_us = now; ++frames;
        if (!frame_has_activity(frame, sizeof(frame))) {
            int64_t profile_now = esp_timer_get_time();
            if (profile_now - last_profile_us >= 5000000LL) { log_voice_profile(frames, tx_ok, tx_drop, interval_sum_us, interval_max_us, interval_count); last_profile_us = profile_now; }
            continue;
        }
        if (!websocket_is_connected()) { vTaskDelay(1); continue; }
        if (websocket_send_audio_data(frame, sizeof(frame))) ++tx_ok; else ++tx_drop;
        int64_t profile_now = esp_timer_get_time();
        if (profile_now - last_profile_us >= 5000000LL) { log_voice_profile(frames, tx_ok, tx_drop, interval_sum_us, interval_max_us, interval_count); last_profile_us = profile_now; }
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "GEMINI LIVE VOICE REFERENCE START");
    ESP_LOGI(TAG, "PSRAM total=%u free=%u heap=%u",
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)esp_get_free_heap_size());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);

    audio_hal_init();
    audio_i2s_test_tone();
    wifi_init_sta();
    if (!wifi_wait_for_connection(WIFI_WAIT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "WiFi tidak READY; reference test dihentikan");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi READY; power save OFF");
    sync_network_time();
    websocket_app_start();

#if VOICE_REFERENCE_SYNTHETIC_TX
    if (xTaskCreate(synthetic_transport_task, "voice_test_tx", 4096, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Gagal membuat TEST A task");
#else
    if (xTaskCreatePinnedToCore(microphone_task, "voice_mic", 4096, NULL, 5, NULL, 1) != pdPASS)
        ESP_LOGE(TAG, "Gagal membuat MIC task");
#endif

    ESP_LOGI(TAG, "REFERENCE READY: MIC <-> GEMINI <-> SPEAKER");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
