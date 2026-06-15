/**
 * ConversationTask.cpp  —  WebSocket Streaming Edition
 * ======================================================
 * Architecture:
 *   Core 0 (WiFi): WebSocket client loop + mic → WS binary frames + WS → ring buffer
 *   Core 1 (App):  Ring buffer → I2S speaker playback
 *
 * Protocol:
 *   ESP32 → server : binary frames = raw 16 kHz mono int16 PCM chunks (1024 bytes each)
 *   ESP32 → server : text  {"event":"speech_done"}  when silence detected
 *   server → ESP32 : binary frames = 16 kHz mono int16 PCM audio chunks
 *   server → ESP32 : text  {"event":"audio_done", "end_conversation": bool}
 */

#include "app/tasks.h"
#include <esp_log.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "app/audio/microphone.h"
#include "Speaker.h"
#include "greetings_audio.h"
#include <csr.h>

static const char* TAG = "ConvTask";
TaskHandle_t convTaskHandle = nullptr;

bool is_conversation_active = false;

// ── Server Config ────────────────────────────────────────────────────────────
#define SERVER_HOST    "10.211.100.156"
#define SERVER_PORT    5000
#define SERVER_WS_PATH "/ws/conversation"
#define BOT_SECRET_KEY "dfgF.sd:Oklfgdhdsa034kJDJdsfbjsdnd/dsad"
#define SESSION_ID     "esp32-default"

// ── Recording Config ─────────────────────────────────────────────────────────
#define SAMPLE_RATE         16000
#define MAX_REC_DURATION_MS (15 * 1000)
#define SILENCE_THRESHOLD   600
#define END_SILENCE_MS      1000
#define INITIAL_SILENCE_MS  5000
#define MIC_CHUNK_BYTES     1024   // ~32 ms of audio per WS frame

// ── Ring Buffer Config (audio output) ───────────────────────────────────────
#define RING_BUF_SIZE     (1024 * 1024)  // 1 MB in PSRAM (~32 s of audio)
#define PRE_BUFFER_BYTES  (16  * 1024)   // Start playing after 16 KB (~0.5 s)

// ── Ring Buffer State ────────────────────────────────────────────────────────
static uint8_t*         g_ringBuf       = nullptr;
static volatile size_t  g_writePos      = 0;
static volatile size_t  g_readPos       = 0;
static volatile bool    g_audioStreamDone   = false;
static volatile bool    g_endConvo          = false;
static volatile bool    g_wsConnected       = false;

// Spinlock-free single-producer / single-consumer FIFO
// (Core 0 writes via onWsEvent, Core 1 reads via audioPlaybackTask)
static inline size_t ringAvailable() {
    return (g_writePos - g_readPos + RING_BUF_SIZE) % RING_BUF_SIZE;
}
static inline size_t ringFreeSpace() {
    return RING_BUF_SIZE - ringAvailable() - 1;
}
static void ringWrite(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_ringBuf[g_writePos] = data[i];
        g_writePos = (g_writePos + 1) % RING_BUF_SIZE;
    }
}
static size_t ringRead(uint8_t* out, size_t maxLen) {
    size_t avail   = ringAvailable();
    size_t toRead  = (maxLen < avail) ? maxLen : avail;
    for (size_t i = 0; i < toRead; i++) {
        out[i]   = g_ringBuf[g_readPos];
        g_readPos = (g_readPos + 1) % RING_BUF_SIZE;
    }
    return toRead;
}

// ── WebSocket Client ─────────────────────────────────────────────────────────
static WebSocketsClient g_ws;

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_CONNECTED:
            g_wsConnected = true;
            ESP_LOGI(TAG, "[WS] Connected to %s:%d%s", SERVER_HOST, SERVER_PORT, SERVER_WS_PATH);
            break;

        case WStype_DISCONNECTED:
            g_wsConnected = false;
            ESP_LOGW(TAG, "[WS] Disconnected — will auto-reconnect");
            break;

        case WStype_BIN:
            // Raw 16 kHz PCM audio chunk from backend TTS
            if (g_ringBuf && ringFreeSpace() >= length) {
                ringWrite(payload, length);
            } else {
                ESP_LOGW(TAG, "[WS] Ring buffer full — dropped %d bytes", (int)length);
            }
            break;

        case WStype_TEXT: {
            // JSON control frame from backend
            StaticJsonDocument<256> doc;
            DeserializationError err = deserializeJson(doc, payload, length);
            if (err) {
                ESP_LOGW(TAG, "[WS] JSON parse error: %s", err.c_str());
                break;
            }
            const char* ev = doc["event"] | "";
            ESP_LOGI(TAG, "[WS] Event: %s", ev);

            if (strcmp(ev, "audio_done") == 0) {
                g_audioStreamDone = true;
                g_endConvo = doc["end_conversation"] | false;
                ESP_LOGI(TAG, "[WS] Audio stream done (end_convo=%d)", (int)g_endConvo);
            } else if (strcmp(ev, "transcript") == 0) {
                ESP_LOGI(TAG, "[WS] Transcript: %s", doc["text"] | "(empty)");
            } else if (strcmp(ev, "no_speech") == 0) {
                // Backend heard nothing — signal done so we resume quickly
                g_audioStreamDone = true;
                g_endConvo = false;
            } else if (strcmp(ev, "error") == 0) {
                ESP_LOGE(TAG, "[WS] Server error: %s", doc["message"] | "unknown");
                g_audioStreamDone = true;
            }
            break;
        }

        case WStype_ERROR:
            ESP_LOGE(TAG, "[WS] Socket error");
            break;

        default:
            break;
    }
}

// ── Audio Playback Task (Core 1) ─────────────────────────────────────────────
static TaskHandle_t g_playbackTaskHandle = nullptr;

static void audioPlaybackTask(void* param) {
    const float   VOLUME_BOOST  = 3.5f;
    const size_t  CHUNK_SAMPLES = 512;          // 512 samples = 32 ms
    uint8_t       rawBuf[CHUNK_SAMPLES * 2];
    int16_t       outBuf[CHUNK_SAMPLES];
    bool          startedPlaying = false;

    ESP_LOGI(TAG, "[Play] Playback task started, pre-buffering %d KB...",
             (int)(PRE_BUFFER_BYTES / 1024));

    while (true) {
        // 1. Wait for pre-buffer to fill before starting playback
        if (!startedPlaying) {
            if (ringAvailable() >= PRE_BUFFER_BYTES || g_audioStreamDone) {
                startedPlaying = true;
                g_displayState = 2;
                if (faceDisplay) faceDisplay->Expression.GoTo_Happy();
                ESP_LOGI(TAG, "[Play] Pre-buffer ready — starting I2S output");
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // 2. Read a chunk from ring buffer
        size_t got = ringRead(rawBuf, CHUNK_SAMPLES * 2);

        if (got == 0) {
            if (g_audioStreamDone) {
                ESP_LOGI(TAG, "[Play] Ring buffer drained — playback complete");
                break;
            }
            // Waiting for more data from network
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // 3. Apply volume boost and send to I2S speaker
        size_t samples = got / 2;
        for (size_t i = 0; i < samples; i++) {
            int16_t s;
            memcpy(&s, rawBuf + i * 2, 2);
            float boosted = (float)s * VOLUME_BOOST;
            if (boosted >  32767.0f) boosted =  32767.0f;
            if (boosted < -32768.0f) boosted = -32768.0f;
            outBuf[i] = (int16_t)boosted;
        }
        GlobalSpeaker.playPCM((uint8_t*)outBuf, samples * 2);
    }

    g_playbackTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// ── Main Conversation Task (Core 0) ──────────────────────────────────────────
void conversationTask(void* param) {
    // Allocate ring buffer in PSRAM once for the lifetime of the task
    g_ringBuf = (uint8_t*)heap_caps_malloc(RING_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!g_ringBuf) {
        ESP_LOGE(TAG, "FATAL: Ring buffer PSRAM allocation failed (%d KB)", (int)(RING_BUF_SIZE / 1024));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "[WS] Ring buffer allocated: %d KB in PSRAM", (int)(RING_BUF_SIZE / 1024));

    // Setup WebSocket — persistent connection, auto-reconnects
    g_ws.begin(SERVER_HOST, SERVER_PORT, SERVER_WS_PATH);
    g_ws.onEvent(onWsEvent);
    g_ws.setReconnectInterval(3000);
    g_ws.enableHeartbeat(15000, 3000, 2);   // ping every 15s, timeout 3s, 2 retries

    // Custom headers for auth
    String headers = "X-Bot-Secret-Key: " + String(BOT_SECRET_KEY) + "\r\n"
                   + "X-Session-Id: "      + String(SESSION_ID);
    g_ws.setExtraHeaders(headers.c_str());

    ESP_LOGI(TAG, "[WS] Connecting to ws://%s:%d%s ...", SERVER_HOST, SERVER_PORT, SERVER_WS_PATH);

    while (true) {
        // ── Must call loop() very frequently to service WebSocket ──────────
        g_ws.loop();

        // ── Wait for wake-word notification ────────────────────────────────
        if (!is_conversation_active) {
            uint32_t notif = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            if (notif != 1) continue;  // Keep looping g_ws.loop() while idle

            is_conversation_active = true;
            ESP_LOGI(TAG, "Conversation started!");

            // Play greeting
            g_displayState = 2;
            if (faceDisplay) {
                faceDisplay->LookFront();
                faceDisplay->Expression.GoTo_Happy();
            }
            int greetIdx = esp_random() % GREETINGS_COUNT;
            GlobalSpeaker.playPCM(GREETINGS_LIST[greetIdx].data,
                                  GREETINGS_LIST[greetIdx].length);
        } else {
            GlobalSpeaker.beep();
        }

        // ── Pause wake-word SR ─────────────────────────────────────────────
        SR::sr_pause();

        // ── Display: Listening ─────────────────────────────────────────────
        g_displayState = 1;
        if (faceDisplay) {
            faceDisplay->LookFront();
            faceDisplay->Expression.GoTo_Skeptic();
        }

        // ── Wait for WebSocket to be connected ─────────────────────────────
        uint32_t wsWaitStart = millis();
        while (!g_wsConnected && millis() - wsWaitStart < 8000) {
            g_ws.loop();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!g_wsConnected) {
            ESP_LOGE(TAG, "WebSocket not connected after 8s — aborting turn");
            is_conversation_active = false;
            SR::sr_resume();
            continue;
        }

        // ── Record + Stream PCM over WebSocket ─────────────────────────────
        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "     [ BOT IS LISTENING... ]         ");
        ESP_LOGI(TAG, "======================================");

        bool     hasSpoken      = false;
        uint32_t silenceStart   = millis();
        uint32_t recStart       = millis();
        size_t   totalRead      = 0;

        // Small stack buffer — no 480 KB PSRAM allocation needed for mic
        uint8_t micChunk[MIC_CHUNK_BYTES];

        while (true) {
            g_ws.loop();  // Keep socket alive while recording

            size_t got = 0;
            microphone->read(micChunk, sizeof(micChunk), &got, 50);

            if (got > 0) {
                // Stream raw PCM immediately to backend
                if (g_wsConnected) {
                    g_ws.sendBIN(micChunk, got);
                }

                // Silence / speech detection
                int16_t* samples  = (int16_t*)micChunk;
                size_t   nSamples = got / 2;
                int32_t  sum      = 0;
                for (size_t i = 0; i < nSamples; i++) sum += samples[i];
                int16_t  dc       = nSamples ? (int16_t)(sum / (int32_t)nSamples) : 0;

                int maxAmp = 0;
                for (size_t i = 0; i < nSamples; i++) {
                    int v = (int)samples[i] - (int)dc;
                    if (v < 0) v = -v;
                    if (v > maxAmp) maxAmp = v;
                }

                if (maxAmp > SILENCE_THRESHOLD) {
                    if (!hasSpoken)
                        ESP_LOGI(TAG, "Speech detected (amp=%d)", maxAmp);
                    hasSpoken    = true;
                    silenceStart = millis();
                } else {
                    if (hasSpoken && millis() - silenceStart > END_SILENCE_MS) {
                        ESP_LOGI(TAG, "Post-speech silence → stopping");
                        break;
                    }
                    if (!hasSpoken && millis() - silenceStart > INITIAL_SILENCE_MS) {
                        ESP_LOGI(TAG, "No speech detected → stopping");
                        break;
                    }
                }

                totalRead += got;
            }

            if (millis() - recStart > MAX_REC_DURATION_MS) {
                ESP_LOGI(TAG, "Max recording duration reached");
                break;
            }
        }

        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "   [ BOT STOPPED LISTENING ]         ");
        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "Streamed %d bytes total", (int)totalRead);

        // Signal backend that speech collection is complete
        if (g_wsConnected) {
            g_ws.sendTXT("{\"event\":\"speech_done\"}");
        }
        GlobalSpeaker.beep();
        g_displayState = 0;  // Thinking...

        // ── Reset ring buffer for new audio response ───────────────────────
        g_writePos        = 0;
        g_readPos         = 0;
        g_audioStreamDone = false;
        g_endConvo        = false;

        // Spawn audio playback task on Core 1
        xTaskCreatePinnedToCore(
            audioPlaybackTask,
            "audio_play",
            4096,
            nullptr,
            5,
            &g_playbackTaskHandle,
            1   // Core 1 = Application core
        );

        // ── Core 0: keep WebSocket alive until playback is fully done ──────
        uint32_t maxWaitMs = 45000;  // 45s max (covers long LLM + TTS responses)
        uint32_t waitStart = millis();

        while ((!g_audioStreamDone || g_playbackTaskHandle != nullptr)
               && millis() - waitStart < maxWaitMs) {
            g_ws.loop();
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (millis() - waitStart >= maxWaitMs) {
            ESP_LOGW(TAG, "Response timeout — ending conversation");
            g_endConvo = true;
        }

        // Reset speaker to default rate (beeps use 16 kHz)
        GlobalSpeaker.setSampleRate(16000);

        // ── Decide: continue or end conversation ───────────────────────────
        is_conversation_active = !g_endConvo;

        if (!is_conversation_active) {
            g_displayState = 0;
            if (faceDisplay) faceDisplay->Expression.GoTo_Normal();
            SR::sr_resume();
            ESP_LOGI(TAG, "Conversation ended — SR resumed");
        }
    }
}
