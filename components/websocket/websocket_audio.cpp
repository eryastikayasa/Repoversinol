#include "websocket_internal.h"
#include "audio_hal.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "WS_AUDIO";
static constexpr size_t AUDIO_RING_BUFFER_SIZE = 32 * 1024;
static constexpr size_t AUDIO_PLAYBACK_READ_SIZE = 1024;
static constexpr size_t AUDIO_PLAYBACK_TRIGGER_SIZE = 256;
static constexpr uint32_t AUDIO_PLAYBACK_RATE = 24000;

static volatile bool audio_clear_pending = false;
static uint32_t audio_turn_generation = 0;

size_t get_audio_pending_bytes(void)
{
    return audio_stream ? xStreamBufferBytesAvailable(audio_stream) : 0;
}

void check_audio_playback_complete(void)
{
    if (!audio_turn_complete_pending || !audio_stream) return;
    if (xStreamBufferBytesAvailable(audio_stream) != 0) return;
    audio_turn_complete_pending = false;
    audio_turn_active = false;
    ESP_LOGI(TAG, "Playback complete: received=%llu played=%llu dropped=%llu",
             (unsigned long long)audio_bytes_received,
             (unsigned long long)audio_bytes_played,
             (unsigned long long)audio_bytes_dropped);
}

static void audio_playback_task(void *arg)
{
    (void)arg;
    static uint8_t buffer[AUDIO_PLAYBACK_READ_SIZE];
    uint32_t generation = 0;
    bool started = false;
    uint32_t underruns = 0;

    ESP_LOGI(TAG, "Playback worker: core=%d priority=%d ring=%u bytes",
             xPortGetCoreID(), uxTaskPriorityGet(NULL), (unsigned)AUDIO_RING_BUFFER_SIZE);

    for (;;) {
        if (audio_clear_pending) {
            audio_clear_pending = false;
            if (audio_stream) xStreamBufferReset(audio_stream);
            started = false;
            audio_turn_complete_pending = false;
            audio_turn_active = false;
            generation = audio_turn_generation;
        }

        if (!audio_stream) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (generation != audio_turn_generation) {
            generation = audio_turn_generation;
            started = false;
        }

        size_t pending = xStreamBufferBytesAvailable(audio_stream);
        if (!started && audio_turn_active && pending < AUDIO_PLAYBACK_TRIGGER_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t got = xStreamBufferReceive(audio_stream, buffer, sizeof(buffer), pdMS_TO_TICKS(10));
        if (got == 0) {
            if (started && audio_turn_active && !audio_turn_complete_pending) {
                ++underruns;
                if ((underruns % 10U) == 1U)
                    ESP_LOGW(TAG, "Playback underrun count=%lu", (unsigned long)underruns);
            }
            check_audio_playback_complete();
            continue;
        }

        started = true;
        audio_write_speaker(buffer, got);
        ++audio_write_calls;
        audio_bytes_played += got;
        check_audio_playback_complete();
    }
}

bool start_audio_playback(void)
{
    if (audio_stream) return true;
    audio_stream = xStreamBufferCreate(AUDIO_RING_BUFFER_SIZE, AUDIO_PLAYBACK_TRIGGER_SIZE);
    if (!audio_stream) {
        ESP_LOGE(TAG, "Failed to create playback buffer: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    if (xTaskCreatePinnedToCore(audio_playback_task, "audio_playback", 4096, NULL, 4,
                                &audio_playback_task_handle, 0) != pdPASS) {
        vStreamBufferDelete(audio_stream);
        audio_stream = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Playback ready: %u-byte bounded buffer, %u Hz PCM16",
             (unsigned)AUDIO_RING_BUFFER_SIZE, (unsigned)AUDIO_PLAYBACK_RATE);
    return true;
}

void request_audio_buffer_clear(void) { audio_clear_pending = true; }

void clear_audio_buffer(void)
{
    if (audio_stream) xStreamBufferReset(audio_stream);
    audio_clear_pending = false;
    audio_turn_complete_pending = false;
    audio_turn_active = false;
}

void reset_audio_turn_stats(void)
{
    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_write_calls = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
    audio_turn_active = false;
    audio_turn_complete_pending = false;
}

void begin_audio_turn(void)
{
    if (audio_turn_active) return;
    ++audio_turn_generation;
    if (audio_turn_generation == 0) audio_turn_generation = 1;
    audio_turn_active = true;
    audio_turn_complete_pending = false;
    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
}

bool queue_audio_pcm(const uint8_t *pcm, size_t len)
{
    if (!pcm || !len) return false;
    len &= ~((size_t)1);
    if (!len) return false;
    if (!audio_stream && !start_audio_playback()) return false;
    if (!audio_stream) return false;

    begin_audio_turn();
    size_t sent = xStreamBufferSend(audio_stream, pcm, len, 0);
    if (sent > 0) audio_bytes_queued += sent;
    if (sent < len) {
        audio_bytes_dropped += len - sent;
        ESP_LOGW(TAG, "RX playback buffer full: dropped=%u pending=%u/%u",
                 (unsigned)(len - sent),
                 (unsigned)xStreamBufferBytesAvailable(audio_stream),
                 (unsigned)AUDIO_RING_BUFFER_SIZE);
    }
    return sent == len;
}
