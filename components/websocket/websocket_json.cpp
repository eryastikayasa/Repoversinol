#include "websocket_internal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "WS_JSON";
static uint8_t pcm_decode_buffer[24 * 1024];

void clear_session_handle(void)
{
    session_handle[0] = '\0';
    session_resumable = false;
}

bool store_session_handle(const char *handle)
{
    if (!handle || !handle[0]) return false;
    size_t len = strlen(handle);
    if (len >= sizeof(session_handle)) return false;
    memcpy(session_handle, handle, len + 1);
    session_resumable = true;
    ESP_LOGI(TAG, "Gemini session resumption handle updated");
    return true;
}

bool build_gemini_setup(char **output, size_t *output_len)
{
    if (!output || !output_len) return false;
    *output = NULL;
    *output_len = 0;

    /*
     * Google Live API contract:
     * - setup is the first client message
     * - model is required
     * - response modality is AUDIO
     * - server-side automatic activity detection remains enabled
     * - session resumption and context compression are explicitly enabled
     *   for long-lived voice sessions
     */
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    if (!setup) { cJSON_Delete(root); return false; }

    cJSON *generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = generation ? cJSON_AddArrayToObject(generation, "responseModalities") : NULL;
    if (!generation || !modalities) { cJSON_Delete(root); return false; }
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));

    cJSON *speech = cJSON_AddObjectToObject(generation, "speechConfig");
    cJSON *voice = speech ? cJSON_AddObjectToObject(speech, "voiceConfig") : NULL;
    cJSON *prebuilt = voice ? cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig") : NULL;
    if (!speech || !voice || !prebuilt) { cJSON_Delete(root); return false; }
    cJSON_AddStringToObject(prebuilt, "voiceName", "Kore");
    cJSON_AddStringToObject(speech, "languageCode", "id-ID");

    cJSON_AddStringToObject(setup, "model", "models/gemini-3.1-flash-live-preview");
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    cJSON *realtime = cJSON_AddObjectToObject(setup, "realtimeInputConfig");
    cJSON *aad = realtime ? cJSON_AddObjectToObject(realtime, "automaticActivityDetection") : NULL;
    if (!realtime || !aad) { cJSON_Delete(root); return false; }
    cJSON_AddBoolToObject(aad, "disabled", false);

    cJSON *resume = cJSON_AddObjectToObject(setup, "sessionResumption");
    if (!resume) { cJSON_Delete(root); return false; }
    if (session_resumable && session_handle[0])
        cJSON_AddStringToObject(resume, "handle", session_handle);

    cJSON *compression = cJSON_AddObjectToObject(setup, "contextWindowCompression");
    cJSON *window = compression ? cJSON_AddObjectToObject(compression, "slidingWindow") : NULL;
    if (!compression || !window) { cJSON_Delete(root); return false; }
    cJSON_AddNumberToObject(window, "targetTokens", 12500);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    *output = json;
    *output_len = strlen(json);
    ESP_LOGI(TAG, "Gemini setup: AUDIO + server AAD + resumption + sliding-window compression");
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

static void process_session_resumption(cJSON *obj)
{
    if (!cJSON_IsObject(obj)) return;

    cJSON *handle = cJSON_GetObjectItem(obj, "newHandle");
    cJSON *resumable = cJSON_GetObjectItem(obj, "resumable");
    if (cJSON_IsTrue(resumable) && cJSON_IsString(handle)) {
        (void)store_session_handle(handle->valuestring);
    }
}

static void process_server_content(cJSON *server)
{
    if (!cJSON_IsObject(server)) return;

    cJSON *turn = cJSON_GetObjectItem(server, "modelTurn");
    cJSON *parts = turn ? cJSON_GetObjectItem(turn, "parts") : NULL;
    if (cJSON_IsArray(parts)) {
        cJSON *part = NULL;
        cJSON_ArrayForEach(part, parts) {
            cJSON *inline_data = cJSON_GetObjectItem(part, "inlineData");
            if (cJSON_IsObject(inline_data) && !decode_audio(inline_data))
                ESP_LOGW(TAG, "Gemini RX audio decode/queue failed");
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(server, "interrupted"))) {
        clear_audio_buffer();
        audio_turn_complete_pending = false;
        audio_turn_active = false;
        ESP_LOGI(TAG, "Gemini TURN INTERRUPTED: playback flushed");
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(server, "generationComplete"))) {
        ESP_LOGI(TAG, "Gemini GENERATION COMPLETE");
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(server, "turnComplete"))) {
        audio_turn_complete_pending = true;
        check_audio_playback_complete();
        ESP_LOGI(TAG, "Gemini TURN COMPLETE");
    }

    /* SessionResumptionUpdate may be nested in serverContent on newer schemas. */
    cJSON *nested_resume = cJSON_GetObjectItem(server, "sessionResumptionUpdate");
    process_session_resumption(nested_resume);
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

    /* Server messages carry one primary top-level union field. */
    cJSON *setup_obj = cJSON_GetObjectItem(root, "setupComplete");
    if (cJSON_IsObject(setup_obj)) {
        setup_complete = true;
        ESP_LOGI(TAG, "Gemini SETUP COMPLETE");
    }

    process_session_resumption(cJSON_GetObjectItem(root, "sessionResumptionUpdate"));

    cJSON *go_away = cJSON_GetObjectItem(root, "goAway");
    if (cJSON_IsObject(go_away)) {
        cJSON *time_left = cJSON_GetObjectItem(go_away, "timeLeft");
        ESP_LOGW(TAG, "Gemini GO_AWAY timeLeft=%s",
                 cJSON_IsString(time_left) ? time_left->valuestring : "object");
        /* Do not close from the RX callback. The supervisor will recover the session. */
        websocket_tx_error = true;
        is_connected = false;
        setup_complete = false;
        ++websocket_connection_generation;
        websocket_tx_flush_queue();
        websocket_rx_flush_queue();
        websocket_rx_request_reset();
    }

    process_server_content(cJSON_GetObjectItem(root, "serverContent"));

    /* Tool calls are intentionally reported but not auto-executed by this voice layer. */
    cJSON *tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (cJSON_IsObject(tool_call))
        ESP_LOGW(TAG, "Gemini toolCall received but no tool executor is configured");

    cJSON *tool_cancel = cJSON_GetObjectItem(root, "toolCallCancellation");
    if (cJSON_IsObject(tool_cancel))
        ESP_LOGI(TAG, "Gemini toolCallCancellation received");

    cJSON_Delete(root);
}
