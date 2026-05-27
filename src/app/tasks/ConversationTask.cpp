#include "app/tasks.h"
#include <esp_log.h>
#include <WiFi.h>
#include "app/audio/microphone.h"
#include "Speaker.h"
#include <csr.h>

static const char* TAG = "ConvTask";
TaskHandle_t convTaskHandle = nullptr;

bool is_conversation_active = false;

#define SERVER_HOST "192.168.1.9"
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

        // 4. Send via raw WiFiClient (same approach as the working button sketch)
        if (WiFi.status() == WL_CONNECTED) {
            WiFiClient client;
            if (!client.connect(SERVER_HOST, SERVER_PORT)) {
                ESP_LOGE(TAG, "Failed to connect to server");
                is_conversation_active = false;
            } else {
                // Build WAV file in PSRAM: 44-byte header + PCM data
                uint32_t wavDataSize = totalBytesRead;
                uint32_t wavTotalSize = 44 + wavDataSize;

                // Build multipart parts
                String boundary = "----ESP32Boundary" + String(millis());
                String headerPart = "--" + boundary + "\r\n"
                    "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n"
                    "Content-Type: audio/wav\r\n\r\n";
                String footerPart = "\r\n--" + boundary + "--\r\n";

                size_t contentLength = headerPart.length() + wavTotalSize + footerPart.length();

                // Send HTTP request headers
                client.println("POST " SERVER_PATH " HTTP/1.1");
                client.println("Host: " SERVER_HOST ":" + String(SERVER_PORT));
                client.println("Content-Type: multipart/form-data; boundary=" + boundary);
                client.println("Content-Length: " + String(contentLength));
                client.println("Connection: close");
                client.println();

                // Send multipart header
                client.print(headerPart);

                // Send WAV header (44 bytes)
                uint8_t wavHeader[44];
                buildWavHeader(wavHeader, wavDataSize);
                client.write(wavHeader, 44);

                // Stream PCM data in chunks
                const size_t CHUNK = 512;
                for (size_t sent = 0; sent < totalBytesRead; sent += CHUNK) {
                    size_t toSend = totalBytesRead - sent;
                    if (toSend > CHUNK) toSend = CHUNK;
                    client.write(audioBuffer + sent, toSend);
                }

                // Send multipart footer
                client.print(footerPart);

                ESP_LOGI(TAG, "Sent %d bytes to server, waiting for response...", contentLength);

                // 5. Wait for response headers
                unsigned long timeout = millis();
                while (!client.available() && millis() - timeout < 10000) delay(10);

                if (!client.available()) {
                    ESP_LOGE(TAG, "No response from server");
                    is_conversation_active = false;
                } else {
                    // Read HTTP headers; detect transfer encoding
                    bool headersEnded = false;
                    bool isChunked = false;
                    int contentLength = -1;
                    bool isFirstLine = true;
                    int httpStatus = 200;

                    while (client.connected() && !headersEnded) {
                        if (client.available()) {
                            String line = client.readStringUntil('\n');
                            line.trim();
                            if (isFirstLine) {
                                isFirstLine = false;
                                // Parse HTTP/1.1 200 OK -> 200
                                int spaceIdx = line.indexOf(' ');
                                if (spaceIdx != -1) {
                                    httpStatus = line.substring(spaceIdx + 1, spaceIdx + 4).toInt();
                                    ESP_LOGI(TAG, "HTTP Status: %d", httpStatus);
                                }
                                continue;
                            }
                            
                            if (line == "") {
                                headersEnded = true;
                            } else {
                                String lower = line;
                                lower.toLowerCase();
                                if (lower.indexOf("transfer-encoding: chunked") >= 0) {
                                    isChunked = true;
                                } else if (lower.startsWith("content-length:")) {
                                    contentLength = lower.substring(15).toInt();
                                } else if (lower.startsWith("x-end-conversation:")) {
                                    if (lower.indexOf("true") >= 0) {
                                        is_conversation_active = false;
                                        ESP_LOGI(TAG, "Server requested conversation close.");
                                    }
                                }
                                ESP_LOGD(TAG, "Header: %s", line.c_str());
                            }
                        }
                    }
                    
                    if (httpStatus != 200) {
                        ESP_LOGE(TAG, "Server returned error %d, ending conversation", httpStatus);
                        is_conversation_active = false;
                    }
                    
                    ESP_LOGI(TAG, "Response: chunked=%d, content-length=%d", isChunked, contentLength);

                    // 6. Buffer entire response into PSRAM, then play
                    // Server uses chunked transfer-encoding; decode it properly so
                    // chunk-size hex lines don't end up in the audio buffer (noise).
                    ESP_LOGI(TAG, "Buffering response audio...");
                    // Signal mainTask: speaking state
                    g_displayState = 2;  // Speaking
                    if (faceDisplay) faceDisplay->Expression.GoTo_Happy();

                    // Allocate 512KB for response (plenty for short TTS clips)
                    const size_t RSP_BUF_MAX = 512 * 1024;
                    uint8_t* rspBuf = (uint8_t*)heap_caps_malloc(RSP_BUF_MAX, MALLOC_CAP_SPIRAM);
                    size_t rspLen = 0;

                    if (rspBuf) {
                        unsigned long dataTimeout = millis();

                        if (isChunked) {
                            // --- Chunked Transfer-Encoding decode ---
                            bool chunkedDone = false;
                            while (client.connected() && !chunkedDone
                                   && rspLen < RSP_BUF_MAX
                                   && millis() - dataTimeout < 10000) {

                                if (!client.available()) { delay(1); continue; }

                                // Read chunk-size line (hex + \r\n)
                                String chunkSizeLine = client.readStringUntil('\n');
                                chunkSizeLine.trim();
                                if (chunkSizeLine.length() == 0) continue;

                                int chunkSize = (int)strtol(chunkSizeLine.c_str(), NULL, 16);
                                if (chunkSize == 0) { chunkedDone = true; break; }

                                // Read exactly chunkSize payload bytes
                                int remaining = chunkSize;
                                while (remaining > 0 && client.connected()
                                       && millis() - dataTimeout < 10000) {
                                    if (client.available()) {
                                        size_t toRead = min((size_t)remaining, RSP_BUF_MAX - rspLen);
                                        int got = client.read(rspBuf + rspLen, toRead);
                                        if (got > 0) {
                                            rspLen += got;
                                            remaining -= got;
                                            dataTimeout = millis();
                                        }
                                    } else {
                                        delay(1);
                                    }
                                }

                                // Consume trailing \r\n after chunk payload
                                client.readStringUntil('\n');
                                dataTimeout = millis();
                            }
                        } else {
                            // --- Plain (non-chunked) read ---
                            size_t target = (contentLength > 0 && (size_t)contentLength < RSP_BUF_MAX)
                                            ? (size_t)contentLength : RSP_BUF_MAX;
                            while (client.connected() && rspLen < target
                                   && millis() - dataTimeout < 10000) {
                                if (client.available()) {
                                    int got = client.read(rspBuf + rspLen, target - rspLen);
                                    if (got > 0) { rspLen += got; dataTimeout = millis(); }
                                } else {
                                    delay(1);
                                }
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
                        ESP_LOGI(TAG, "Playback done");
                        vTaskDelay(pdMS_TO_TICKS(200)); // Delay to prevent feedback loop
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate response buffer");
                        is_conversation_active = false;
                    }
                }
                client.stop();
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
