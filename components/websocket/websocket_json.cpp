#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "WS_JSON";
static uint8_t pcm_decode_buffer[24 * 1024];
static volatile bool goaway_reconnect_pending = false;

void clear_session_handle(void) { session_handle[0] = '\0'; session_resumable = false; }

bool store_session_handle(const char *handle)
{
    if (!handle || !handle[0]) return false;
    size_t len = strlen(handle);
    if (len >= sizeof(session_handle)) return false;
    memcpy(session_handle, handle, len + 1);
    session_resumable = true;
    ESP_LOGI(TAG, "Gemini session resumption handle updated (%u bytes)", (unsigned)len);
    return true;
}

bool build_gemini_setup(char **output, size_t *output_len)
{
    if (!output || !output_len) return false;
    *output = NULL; *output_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(generation, "responseModalities");
    if (!setup || !generation || !modalities) { cJSON_Delete(root); return false; }
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));

    cJSON *speech = cJSON_AddObjectToObject(generation, "speechConfig");
    cJSON *voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");
    cJSON_AddStringToObject(speech, "languageCode", "id-ID");
    cJSON_AddStringToObject(setup, "model", "models/gemini-3.1-flash-live-preview");
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = cJSON_AddObjectToObject(realtime, "automaticActivityDetection");
    cJSON_AddBoolToObject(aad, "disabled", true);

    cJSON *resume = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (session_resumable && session_handle[0])
        cJSON_AddStringToObject(resume, "handle", session_handle);

    cJSON *compression = cJSON_AddObjectToObject(setup, "contextWindowCompression");
    cJSON *window = cJSON_AddObjectToObject(compression, "slidingWindow");
    cJSON_AddNumberToObject(window, "targetTokens", 12500);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    *output = json; *output_len = strlen(json);
    ESP_LOGI(TAG, "Gemini setup: AUDIO + MANUAL AAD + session resumption + compression resume=%s",
             session_resumable ? "yes" : "no");
    return true;
}

static bool decode_audio(cJSON *inline_data)
{
    cJSON *audio = cJSON_GetObjectItem(inline_data, "data");
    if (!cJSON_IsString(audio) || !audio->valuestring) return true;
    size_t b64_len = strlen(audio->valuestring);
    if (!b64_len) return true;

    size_t pcm_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &pcm_len,
                                    (const unsigned char *)audio->valuestring, b64_len);
    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return false;
    if (pcm_len == 0 || pcm_len > sizeof(pcm_decode_buffer)) return false;
    size_t decoded = pcm_len;
    ret = mbedtls_base64_decode(pcm_decode_buffer, sizeof(pcm_decode_buffer), &decoded,
                                (const unsigned char *)audio->valuestring, b64_len);
    if (ret != 0 || decoded == 0) return false;
    ++audio_chunks_received;
    audio_bytes_received += decoded;
    return queue_audio_pcm(pcm_decode_buffer, decoded);
}

void process_gemini_message(const char *json, size_t len)
{
    if (!json || !len) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "Invalid Gemini JSON len=%u heap=%u", (unsigned)len,
                 (unsigned)esp_get_free_heap_size());
        return;
    }

    cJSON *setup_obj = cJSON_GetObjectItem(root, "setupComplete");
    if (cJSON_IsObject(setup_obj)) {
        ::setup_complete = true;
        websocket_live_state = LIVE_ST_READY;
        ESP_LOGI(TAG, "Gemini SETUP COMPLETE; multi-turn READY");
    }

    cJSON *resume_update = cJSON_GetObjectItem(root, "sessionResumptionUpdate");
    if (cJSON_IsObject(resume_update)) {
        cJSON *handle = cJSON_GetObjectItem(resume_update, "newHandle");
        cJSON *resumable = cJSON_GetObjectItem(resume_update, "resumable");
        if (cJSON_IsTrue(resumable) && cJSON_IsString(handle)) store_session_handle(handle->valuestring);
    }

    cJSON *goaway = cJSON_GetObjectItem(root, "goAway");
    if (cJSON_IsObject(goaway)) {
        ++websocket_goaway_count;
        goaway_reconnect_pending = true;
        websocket_live_state = LIVE_ST_RECONNECTING;
        ESP_LOGW(TAG, "Gemini goAway received; reconnecting with saved resume handle");
    }

    cJSON *server = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(server)) {
        cJSON *turn = cJSON_GetObjectItem(server, "modelTurn");
        cJSON *parts = turn ? cJSON_GetObjectItem(turn, "parts") : NULL;
        if (cJSON_IsArray(parts)) {
            if (cJSON_GetArraySize(parts) > 0) ++websocket_model_turn_count;
            cJSON *part = NULL;
            cJSON_ArrayForEach(part, parts) {
                cJSON *inline_data = cJSON_GetObjectItem(part, "inlineData");
                if (cJSON_IsObject(inline_data) && !decode_audio(inline_data))
                    ESP_LOGW(TAG, "RX audio decode/queue failed");
            }
            if (cJSON_GetArraySize(parts) > 0) websocket_live_state = LIVE_ST_SPEAKING;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(server, "turnComplete"))) {
            ++websocket_turn_count;
            audio_turn_complete_pending = true;
            websocket_live_state = LIVE_ST_READY;
            check_audio_playback_complete();
            ESP_LOGI(TAG, "Gemini TURN COMPLETE; turn=%lu ready_for_next=yes",
                     (unsigned long)websocket_turn_count);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(server, "interrupted"))) {
            ++websocket_interrupted_count;
            clear_audio_buffer();
            audio_turn_complete_pending = false;
            audio_turn_active = false;
            websocket_live_state = LIVE_ST_READY;
            ESP_LOGI(TAG, "Gemini TURN INTERRUPTED; audio flushed, READY");
        }
    }
    cJSON_Delete(root);
}

bool websocket_goaway_reconnect_pending(void) { return goaway_reconnect_pending; }
void websocket_clear_goaway_reconnect(void) { goaway_reconnect_pending = false; }
