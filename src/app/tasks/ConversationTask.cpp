#include "app/tasks.h"
#include <esp_log.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "app/audio/microphone.h"
#include "Speaker.h"
#include <csr.h>

static const char* TAG = "ConvTask";
TaskHandle_t convTaskHandle = nullptr;

bool is_conversation_active = false;

#define SERVER_HOST "sarthak10x-espdesktopbot.hf.space"
#define SERVER_PORT 443
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

void conversationTask(void *param) {
    while (1) {
        if (!is_conversation_active) {
            // Wait for wake word notification
            uint32_t notif = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (notif != 1) continue;
            
            is_conversation_active = true;
            ESP_LOGI(TAG, "Conversation started!");
            GlobalSpeaker.beep();
        } else {
            ESP_LOGI(TAG, "Conversation continuing...");
            GlobalSpeaker.beep(); // Indicate bot is listening for the next turn
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
            WiFiClientSecure client;
            client.setInsecure(); // Ignore SSL certificates

            String url = "https://" + String(SERVER_HOST) + String(SERVER_PATH);
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

                    // Set headers
                    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                    http.addHeader("User-Agent", "python-requests/2.33.1"); // Mimic requests
                    http.addHeader("Accept", "*/*");
                    http.addHeader("X-Bot-Secret-Key", "dfgF.sd:Oklfgdhdsa034kJDJdsfbjsdnd/dsad");

                    ESP_LOGI(TAG, "Sending HTTP POST with %d bytes...", contentLength);
                    
                    int httpResponseCode = http.POST(payloadBuf, contentLength);
                    
                    heap_caps_free(payloadBuf); // Free immediately after sending

                    if (httpResponseCode > 0) {
                        ESP_LOGI(TAG, "HTTP Response code: %d", httpResponseCode);
                        
                        if (httpResponseCode != 200) {
                            ESP_LOGE(TAG, "Server returned error %d", httpResponseCode);
                            is_conversation_active = false;
                        }

                        // Check headers
                        bool isChunked = false;
                        int responseContentLength = -1;
                        
                        // Parse relevant headers if available
                        for (int i = 0; i < http.headers(); i++) {
                            String headerName = http.headerName(i);
                            String headerVal = http.header(i);
                            headerName.toLowerCase();
                            if (headerName == "transfer-encoding" && headerVal.indexOf("chunked") >= 0) isChunked = true;
                            if (headerName == "content-length") responseContentLength = headerVal.toInt();
                            if (headerName == "x-end-conversation" && headerVal.indexOf("true") >= 0) {
                                is_conversation_active = false;
                            }
                        }

                        ESP_LOGI(TAG, "Response: chunked=%d, content-length=%d", isChunked, responseContentLength);

                        // Signal mainTask: speaking state
                        g_displayState = 2;  // Speaking
                        if (faceDisplay) faceDisplay->Expression.GoTo_Happy();

                        // Allocate 512KB for response (plenty for short TTS clips)
                        const size_t RSP_BUF_MAX = 512 * 1024;
                        uint8_t* rspBuf = (uint8_t*)heap_caps_malloc(RSP_BUF_MAX, MALLOC_CAP_SPIRAM);
                        size_t rspLen = 0;

                        if (rspBuf) {
                            WiFiClient* stream = http.getStreamPtr();
                            unsigned long dataTimeout = millis();
                            
                            // Since HTTPClient already handles chunk decoding transparently, 
                            // stream->read() gives us the raw payload!
                            while (http.connected() && rspLen < RSP_BUF_MAX && millis() - dataTimeout < 30000) {
                                if (stream->available()) {
                                    int got = stream->read(rspBuf + rspLen, RSP_BUF_MAX - rspLen);
                                    if (got > 0) {
                                        rspLen += got;
                                        dataTimeout = millis();
                                    }
                                } else {
                                    delay(1);
                                }
                            }


                        ESP_LOGI(TAG, "Received %d bytes of audio response", rspLen);

                        // Find PCM data: walk WAV chunk tree to find 'data' sub-chunk
                        // (handles non-standard headers with extra chunks before data)
                        size_t pcmOffset = 0;
                        if (rspLen >= 12 && memcmp(rspBuf, "RIFF", 4) == 0 && memcmp(rspBuf + 8, "WAVE", 4) == 0) {
                            size_t pos = 12;
                            while (pos + 8 <= rspLen) {
                                uint32_t subChunkSize;
                                memcpy(&subChunkSize, rspBuf + pos + 4, 4);
                                if (memcmp(rspBuf + pos, "data", 4) == 0) {
                                    pcmOffset = pos + 8;
                                    ESP_LOGI(TAG, "WAV 'data' chunk found at offset %d", (int)pcmOffset);
                                    break;
                                }
                                pos += 8 + subChunkSize;
                            }
                            if (pcmOffset == 0) {
                                // Fallback: just skip standard 44-byte header
                                pcmOffset = 44;
                                ESP_LOGW(TAG, "WAV 'data' not found, defaulting to offset 44");
                            }
                        }

                        // Apply volume boost and play in chunks
                        const float VOLUME_BOOST = 3.5f;  // Increase for louder output (clamp prevents distortion)
                        const size_t CHUNK_SAMPLES = 512;
                        int16_t outBuf[CHUNK_SAMPLES];

                        size_t offset = pcmOffset;
                        while (offset + 2 <= rspLen) {
                            size_t samplesAvail = (rspLen - offset) / 2;
                            size_t samplesToPlay = (samplesAvail < CHUNK_SAMPLES) ? samplesAvail : CHUNK_SAMPLES;

                            for (size_t i = 0; i < samplesToPlay; i++) {
                                int16_t sample;
                                memcpy(&sample, rspBuf + offset + i * 2, 2);
                                float boosted = sample * VOLUME_BOOST;
                                if (boosted >  32767) boosted =  32767;
                                if (boosted < -32768) boosted = -32768;
                                outBuf[i] = (int16_t)boosted;
                            }

                            GlobalSpeaker.playPCM((uint8_t*)outBuf, samplesToPlay * 2);
                            offset += samplesToPlay * 2;
                        }

                        heap_caps_free(rspBuf);
                        } else {
                            ESP_LOGE(TAG, "Failed to connect to server: %s", http.errorToString(httpResponseCode).c_str());
                            is_conversation_active = false;
                        }
                    }
                }
                http.end();
            }
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
