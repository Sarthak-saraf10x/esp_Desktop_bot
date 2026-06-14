#include "app/tasks.h"
#include <esp_log.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "app/audio/microphone.h"
#include "Speaker.h"
#include "greetings_audio.h"
#include <csr.h>

static const char* TAG = "ConvTask";
TaskHandle_t convTaskHandle = nullptr;

bool is_conversation_active = false;
//http://10.211.100.156:5000
#define SERVER_HOST "10.211.100.156"
#define SERVER_PORT 5000
#define SERVER_PATH "/audio_stream"

// Up to 15 seconds of 16kHz 16-bit audio (mono)
#define MAX_REC_DURATION_SEC 15
#define SAMPLE_RATE      16000
#define AUDIO_BUF_SIZE   (SAMPLE_RATE * 2 * MAX_REC_DURATION_SEC)

// Write a minimal 44-byte WAV header into buf
static void buildWavHeader(uint8_t* buf, uint32_t audioDataBytes) {
    uint32_t byteRate   = SAMPLE_RATE * 1 * 2;   // sampleRate * channels * bytesPerSample
    uint16_t blockAlign = 1 * 2;
    uint32_t chunkSize  = 36 + audioDataBytes;

    memcpy(buf,      "RIFF", 4);
    memcpy(buf + 4,  &chunkSize,      4);
    memcpy(buf + 8,  "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t subchunk1Size = 16; memcpy(buf + 16, &subchunk1Size, 4);
    uint16_t audioFmt = 1;      memcpy(buf + 20, &audioFmt, 2);
    uint16_t channels = 1;      memcpy(buf + 22, &channels, 2);
    uint32_t sr = SAMPLE_RATE;  memcpy(buf + 24, &sr, 4);
    memcpy(buf + 28, &byteRate, 4);
    memcpy(buf + 32, &blockAlign, 2);
    uint16_t bps = 16;          memcpy(buf + 34, &bps, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &audioDataBytes, 4);
}

struct AudioDownloadState {
    uint8_t* buf;
    size_t maxLen;
    volatile size_t downloadedLen;
    volatile size_t expectedLen;
    volatile bool downloadComplete;
    volatile bool downloadFailed;
    WiFiClient* stream;
    HTTPClient* http;
};

static void audioDownloadTask(void* pvParameters) {
    AudioDownloadState* state = (AudioDownloadState*)pvParameters;
    unsigned long dataTimeout = millis();
    
    while (state->downloadedLen < state->expectedLen && state->http->connected()) {
        if (state->stream->available()) {
            size_t toRead = state->expectedLen - state->downloadedLen;
            if (toRead > 4096) toRead = 4096;
            
            int got = state->stream->read(state->buf + state->downloadedLen, toRead);
            if (got > 0) {
                state->downloadedLen += got;
                dataTimeout = millis();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        
        if (millis() - dataTimeout > 5000) {
            ESP_LOGW("AudioDownload", "Download timeout");
            state->downloadFailed = true;
            break;
        }
    }
    state->downloadComplete = true;
    vTaskDelete(NULL);
}

void conversationTask(void *param) {
    while (1) {
        if (!is_conversation_active) {
            // Wait for wake word notification
            uint32_t notif = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (notif != 1) continue;
            
            is_conversation_active = true;
            ESP_LOGI(TAG, "Conversation started!");
            
            // Set display to happy/speaking while playing the greeting
            g_displayState = 2; // Speaking
            if (faceDisplay) {
                faceDisplay->LookFront();
                faceDisplay->Expression.GoTo_Happy();
            }
            
            // Play a random greeting spoken word
            int greetingIdx = esp_random() % GREETINGS_COUNT;
            const GreetingAudio& greeting = GREETINGS_LIST[greetingIdx];
            ESP_LOGI(TAG, "Playing greeting: %s", greeting.name);
            GlobalSpeaker.playPCM(greeting.data, greeting.length);
        } else {
            ESP_LOGI(TAG, "Conversation continuing...");
            GlobalSpeaker.beep(); // Indicate bot is listening for the wenext turn
        }

        // 1. Pause SR
        SR::sr_pause();

        // Signal mainTask: recording state
        g_displayState = 1;  // Recording
        if (faceDisplay) {
            faceDisplay->LookFront();
            faceDisplay->Expression.GoTo_Skeptic();

        }

        // 2. Allocate recording buffer in PSRAM
        uint8_t* audioBuffer = (uint8_t*)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!audioBuffer) {
            ESP_LOGE(TAG, "Failed to allocate audio buffer in PSRAM");
            is_conversation_active = false;
            SR::sr_resume();
            continue;
        }

        // 3. Record audio
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "      [ BOT IS LISTENING... ]       ");
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Recording up to %d seconds (silence detection enabled)...", MAX_REC_DURATION_SEC);
        size_t totalBytesRead = 0;
        uint32_t startTime = millis();

        bool hasSpoken = false;
        uint32_t silenceStartTime = millis();
        const int SILENCE_THRESHOLD = 400; // Amplitude threshold to detect speech
        const uint32_t END_SILENCE_MS = 1500; // Stop after 1.5s of silence if speech started
        const uint32_t INITIAL_SILENCE_MS = 5000; // Stop after 5s if no speech at all

        while (totalBytesRead < AUDIO_BUF_SIZE) {
            size_t chunk = AUDIO_BUF_SIZE - totalBytesRead;
            if (chunk > 4096) chunk = 4096;
            size_t bytesRead = 0;
            microphone->read(audioBuffer + totalBytesRead, chunk, &bytesRead, 100);
            
            if (bytesRead > 0) {
                int16_t* samples = (int16_t*)(audioBuffer + totalBytesRead);
                size_t numSamples = bytesRead / 2;
                
                // Calculate DC offset to avoid false amplitude readings
                int32_t sum = 0;
                for (size_t i = 0; i < numSamples; i++) {
                    sum += samples[i];
                }
                int16_t dcOffset = numSamples > 0 ? (sum / numSamples) : 0;
                
                int maxAmplitude = 0;
                for (size_t i = 0; i < numSamples; i++) {
                    int absVal = samples[i] - dcOffset;
                    if (absVal < 0) absVal = -absVal;
                    if (absVal > maxAmplitude) {
                        maxAmplitude = absVal;
                    }
                }
                
                if (maxAmplitude > SILENCE_THRESHOLD) {
                    if (!hasSpoken) {
                        ESP_LOGI(TAG, "Speech detected (amplitude: %d).", maxAmplitude);
                    }
                    hasSpoken = true;
                    silenceStartTime = millis(); // Reset silence timer
                } else {
                    if (hasSpoken) {
                        if (millis() - silenceStartTime > END_SILENCE_MS) {
                            ESP_LOGI(TAG, "Silence detected for %d ms. Stopping recording.", END_SILENCE_MS);
                            totalBytesRead += bytesRead;
                            break;
                        }
                    } else {
                        if (millis() - silenceStartTime > INITIAL_SILENCE_MS) {
                            ESP_LOGI(TAG, "No speech detected for %d ms. Stopping.", INITIAL_SILENCE_MS);
                            totalBytesRead += bytesRead;
                            break;
                        }
                    }
                }
            }
            
            totalBytesRead += bytesRead;
            if (millis() - startTime > (MAX_REC_DURATION_SEC * 1000 + 2000)) {
                ESP_LOGI(TAG, "Max recording duration reached.");
                break;
            }
        }

        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "     [ BOT STOPPED LISTENING ]      ");
        ESP_LOGI(TAG, "====================================");
        ESP_LOGI(TAG, "Recording done. %d bytes", totalBytesRead);
        GlobalSpeaker.beep();
        g_displayState = 0;  // Back to idle while connecting

        // 4. Send via HTTPClient (bulletproof HTTP/1.1 implementation)
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            WiFiClient client;

            String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + String(SERVER_PATH);
            if (!http.begin(client, url)) {
                ESP_LOGE(TAG, "Failed to begin HTTPClient");
                is_conversation_active = false;
            } else {
                http.setTimeout(30000);

                uint32_t wavDataSize = totalBytesRead;
                uint32_t wavTotalSize = 44 + wavDataSize;

                // Build multipart parts
                String boundary = "----ESP32Boundary" + String(millis());
                String headerPart = "--" + boundary + "\r\n"
                    "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n"
                    "Content-Type: audio/wav\r\n\r\n";
                String footerPart = "\r\n--" + boundary + "--\r\n";

                size_t contentLength = headerPart.length() + wavTotalSize + footerPart.length();

                // Allocate one full buffer in PSRAM for the payload
                uint8_t* payloadBuf = (uint8_t*)heap_caps_malloc(contentLength, MALLOC_CAP_SPIRAM);
                if (!payloadBuf) {
                    ESP_LOGE(TAG, "Failed to allocate payload buffer in PSRAM");
                    is_conversation_active = false;
                } else {
                    // 1. Copy Header
                    size_t offset = 0;
                    memcpy(payloadBuf + offset, headerPart.c_str(), headerPart.length());
                    offset += headerPart.length();

                    // 2. Copy WAV Header
                    buildWavHeader(payloadBuf + offset, wavDataSize);
                    offset += 44;

                    // 3. Copy Audio Data
                    memcpy(payloadBuf + offset, audioBuffer, wavDataSize);
                    offset += wavDataSize;

                    // 4. Copy Footer
                    memcpy(payloadBuf + offset, footerPart.c_str(), footerPart.length());
                    offset += footerPart.length();

                    // Set request headers
                    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                    http.addHeader("User-Agent", "ESP32HTTPClient");
                    http.addHeader("Accept", "*/*");
                    http.addHeader("X-Bot-Secret-Key", "dfgF.sd:Oklfgdhdsa034kJDJdsfbjsdnd/dsad");

                    // Tell HTTPClient which response headers to capture
                    const char* headerKeys[] = {"Content-Length", "Transfer-Encoding", "X-End-Conversation"};
                    http.collectHeaders(headerKeys, 3);

                    ESP_LOGI(TAG, "Sending HTTP POST with %d bytes...", contentLength);
                    
                    int httpResponseCode = http.POST(payloadBuf, contentLength);
                    
                    heap_caps_free(payloadBuf); // Free immediately after sending

                    if (httpResponseCode > 0) {
                        ESP_LOGI(TAG, "HTTP Response code: %d", httpResponseCode);
                        
                        if (httpResponseCode != 200) {
                            ESP_LOGE(TAG, "Server returned error %d", httpResponseCode);
                            is_conversation_active = false;
                        }

                        // Read collected response headers
                        int responseContentLength = http.header("Content-Length").toInt();
                        bool isChunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
                        String endConvoHeader = http.header("X-End-Conversation");
                        if (endConvoHeader.indexOf("true") >= 0) {
                            is_conversation_active = false;
                        }

                        ESP_LOGI(TAG, "Response: content-length=%d, chunked=%d", responseContentLength, isChunked);

                        // Signal mainTask: speaking state
                        g_displayState = 2;  // Speaking
                        if (faceDisplay) faceDisplay->Expression.GoTo_Happy();

                        // ========================================================
                        // DOWNLOAD-THEN-PLAY: Buffer all audio into PSRAM first,
                        // then play smoothly. The ~82KB download takes only 1-2s.
                        // (The long wait before this point is server processing.)
                        // ========================================================
                        const size_t RSP_BUF_MAX = 1024 * 1024 * 3 / 2; // 1.5 MB (covers up to 48s response)
                        uint8_t* rspBuf = (uint8_t*)heap_caps_malloc(RSP_BUF_MAX, MALLOC_CAP_SPIRAM);

                        if (rspBuf) {
                            WiFiClient* stream = http.getStreamPtr();
                            unsigned long dlStart = millis();

                            size_t expectedLen = (responseContentLength > 0) ? (size_t)responseContentLength : RSP_BUF_MAX;
                            if (expectedLen > RSP_BUF_MAX) expectedLen = RSP_BUF_MAX;
                            
                            // Initialize download state
                            AudioDownloadState dlState;
                            dlState.buf = rspBuf;
                            dlState.maxLen = RSP_BUF_MAX;
                            dlState.downloadedLen = 0;
                            dlState.expectedLen = expectedLen;
                            dlState.downloadComplete = false;
                            dlState.downloadFailed = false;
                            dlState.stream = stream;
                            dlState.http = &http;

                            // Spawn background download task on Core 0 (WiFi core)
                            xTaskCreatePinnedToCore(
                                audioDownloadTask,
                                "audio_download",
                                4096,
                                &dlState,
                                5, // Priority 5
                                NULL,
                                0 // Core 0
                            );

                            size_t playOffset = 0;
                            bool parsedHeader = false;
                            uint32_t wavSampleRate = 16000;
                            size_t pcmOffset = 44;
                            const float VOLUME_BOOST = 3.5f;
                            const size_t CHUNK_SAMPLES = 512;
                            int16_t outBuf[CHUNK_SAMPLES];

                            const size_t PRE_BUFFER_SIZE = 64 * 1024; // 2 seconds of audio pre-buffering
                            bool startPlaying = false;

                            // Loop runs while downloading is active OR we have unplayed data in buffer
                            while (!dlState.downloadComplete || playOffset < dlState.downloadedLen) {
                                // 1. Parse WAV header as soon as we have enough bytes (or download completes)
                                if (!parsedHeader && (dlState.downloadedLen >= 44 || dlState.downloadComplete)) {
                                    bool foundData = false;
                                    if (dlState.downloadedLen >= 12 && memcmp(rspBuf, "RIFF", 4) == 0 && memcmp(rspBuf + 8, "WAVE", 4) == 0) {
                                        if (dlState.downloadedLen >= 28) {
                                            memcpy(&wavSampleRate, rspBuf + 24, 4);
                                            ESP_LOGI(TAG, "WAV sample rate: %lu Hz", (unsigned long)wavSampleRate);
                                            if (wavSampleRate != 16000) {
                                                ESP_LOGW(TAG, "Adjusting speaker to %lu Hz", (unsigned long)wavSampleRate);
                                                GlobalSpeaker.setSampleRate(wavSampleRate);
                                            }
                                        }
                                        
                                        // Walk chunks to find data offset
                                        size_t pos = 12;
                                        while (pos + 8 <= dlState.downloadedLen) {
                                            uint32_t subChunkSize;
                                            memcpy(&subChunkSize, rspBuf + pos + 4, 4);
                                            if (memcmp(rspBuf + pos, "data", 4) == 0) {
                                                pcmOffset = pos + 8;
                                                ESP_LOGI(TAG, "WAV 'data' chunk at offset %d", (int)pcmOffset);
                                                foundData = true;
                                                break;
                                            }
                                            pos += 8 + subChunkSize;
                                        }
                                    }
                                    
                                    // If we found the 'data' chunk or the download is fully complete, finish parsing
                                    if (foundData || dlState.downloadComplete) {
                                        if (!foundData) {
                                            ESP_LOGW(TAG, "WAV 'data' chunk not found, defaulting to offset 44");
                                            pcmOffset = 44;
                                        }
                                        playOffset = pcmOffset;
                                        parsedHeader = true;
                                    }
                                }

                                // 2. Check if pre-buffering is finished
                                if (!startPlaying && parsedHeader) {
                                    if (dlState.downloadedLen >= pcmOffset + PRE_BUFFER_SIZE || dlState.downloadComplete) {
                                        ESP_LOGI(TAG, "Pre-buffering complete (%d bytes). Starting playback.", (int)dlState.downloadedLen);
                                        startPlaying = true;
                                    }
                                }

                                // 3. Play audio chunk if buffering is complete and we have data
                                if (startPlaying && playOffset < dlState.downloadedLen) {
                                    size_t bytesAvailable = dlState.downloadedLen - playOffset;
                                    if (bytesAvailable < 2) {
                                        if (dlState.downloadComplete) {
                                            playOffset = dlState.downloadedLen; // Discard trailing odd byte
                                        } else {
                                            delay(5);
                                        }
                                        continue;
                                    }

                                    size_t bytesToPlay = (bytesAvailable < CHUNK_SAMPLES * 2) ? bytesAvailable : CHUNK_SAMPLES * 2;
                                    
                                    // If still downloading, wait for full chunks to prevent micro-stutters
                                    if (bytesToPlay < CHUNK_SAMPLES * 2 && !dlState.downloadComplete) {
                                        delay(5);
                                        continue;
                                    }
                                    
                                    size_t samplesToPlay = bytesToPlay / 2;
                                    if (samplesToPlay > 0) {
                                        for (size_t i = 0; i < samplesToPlay; i++) {
                                            int16_t sample;
                                            memcpy(&sample, rspBuf + playOffset + i * 2, 2);
                                            float boosted = sample * VOLUME_BOOST;
                                            if (boosted >  32767) boosted =  32767;
                                            if (boosted < -32768) boosted = -32768;
                                            outBuf[i] = (int16_t)boosted;
                                        }
                                        GlobalSpeaker.playPCM((uint8_t*)outBuf, samplesToPlay * 2);
                                        playOffset += samplesToPlay * 2;
                                    }
                                } else {
                                    // Caught up with download but download is not complete yet
                                    if (dlState.downloadFailed) {
                                        ESP_LOGE(TAG, "Audio download failed or timed out. Ending playback.");
                                        break;
                                    }
                                    delay(5);
                                }
                            }

                            // Safety check: ensure background task has terminated before freeing buffer
                            while (!dlState.downloadComplete) {
                                delay(10);
                            }

                            ESP_LOGI(TAG, "Finished playback of %d bytes (downloaded %d bytes total in %lu ms)", 
                                     (int)(playOffset - pcmOffset), (int)dlState.downloadedLen, millis() - dlStart);
                            heap_caps_free(rspBuf);
                        } else {
                            ESP_LOGE(TAG, "Failed to allocate audio response buffer");
                        }

                        // Reset speaker to default 16kHz for beep/tone sounds
                        GlobalSpeaker.setSampleRate(16000);

                    } else {
                        ESP_LOGE(TAG, "HTTP error: %s", http.errorToString(httpResponseCode).c_str());
                        is_conversation_active = false;
                    }
                } // end payloadBuf else
                http.end();
            } // end http.begin else
        } else {
            ESP_LOGE(TAG, "WiFi not connected");
            is_conversation_active = false;
        }

        heap_caps_free(audioBuffer);

        // 7. Resume SR — back to idle animation
        if (!is_conversation_active) {
            g_displayState = 0;
            if (faceDisplay) faceDisplay->Expression.GoTo_Normal();
            SR::sr_resume();
            ESP_LOGI(TAG, "Conversation ended, SR resumed.");
        }
    }
}

