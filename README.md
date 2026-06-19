# ESP32-S3 AI Desktop Bot

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-green.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-PlatformIO--Arduino-orange.svg)](https://platformio.org/)
[![Backend](https://img.shields.io/badge/backend-FastAPI--Python-blue.svg)](https://fastapi.tiangolo.com/)
[![LLM](https://img.shields.io/badge/brain-Google%20Gemini-violet.svg)](https://ai.google.dev/)

<p align="center">
  <img src="images/final_model.jpg" alt="ESP32-S3 AI Desktop Bot Form Factor" width="500">
</p>

---

## 🎓 Academic Project Frame
This project was designed, developed, and implemented as a **Final Year Capstone Project** in partial fulfillment of the requirements for the degree of **Master of Computer Applications (MCA)**.

* **Academic Year:** 2025 - 2026
* **Developer/Student:** Sarthak Rajesh Saraf (Semester-IV)
* **Institution:** Alard Institute of Management and Sciences, Pune
* **Affiliation:** Savitribai Phule Pune University (SPPU)
* **Project Guide:** Prof. Ashwini Khedkar
* **Project Title:** ESP32-S3 AI Desktop Bot (IoT-based Conversational Agent & Autonomous Productivity Companion)

---

## 📝 Abstract & Project Overview
The **ESP32-S3 AI Desktop Bot** is a hardware-software codesigned, low-latency, voice-to-voice conversational AI smart assistant. Operating as a hybrid edge-cloud system, the bot provides a natural, open-ended voice interface featuring an expressive OLED-animated face that synchronizes with the bot’s operational states (Idle, Listening, Thinking, Speaking).

Unlike traditional voice assistants (e.g., Alexa, Siri) that rely on slow, sequential cloud APIs resulting in a 3–5 second latency, this system introduces several key architectural enhancements:
1. **Offline Wake Word Detection:** Powered locally on the ESP32-S3 hardware using Espressif's ESP-SR (`wn9_hiesp`) model, ensuring user privacy by listening for "Hi ESP" offline.
2. **Persistent Full-Duplex WebSockets:** Streams raw $16\text{ kHz}$ mono $16\text{-bit}$ PCM audio frame-by-frame (1024-byte packets / $32\text{ ms}$ chunks) to the server while the user is still speaking.
3. **Local VAD & Silence Gate:** Analyzes peak amplitude on the edge device to detect end-of-speech, immediately signaling the backend server.
4. **Fast API Backend & Local ML Pipeline:** Transcribes audio instantly in memory using **Faster-Whisper** and synthesizes response chunks sentence-by-sentence using **Piper TTS** with NumPy-based resampling.
5. **Agentic Tool Loop via Model Context Protocol (MCP):** Connects the Google Gemini brain to external resources (Web Search, Weather, Clipboard, Document compilation) using a persistent background tool socket that processes actions in under $300\text{ ms}$.
6. **Dual-Core FreeRTOS Partitioning:** Isolates the high-frequency recording and network communication on Core 0 while keeping OLED rendering and speaker playback on Core 1, pulling from a custom $1\text{ MB}$ PSRAM ring buffer to completely eliminate network jitter and audio clipping.

---

## ⚡ System Architecture & Data Flow

Below is the complete sequence of events from when the user wakes the bot to when it finishes speaking.

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant ESP_C0 as [ESP32 Core 0: Control & WS]
    participant ESP_C1 as [ESP32 Core 1: Playback]
    participant PSRAM as [ESP32 PSRAM Ring Buffer]
    participant BE as [FastAPI Backend Server]
    participant Gemini as [Google Gemini LLM Agent]
    participant MCP as [Persistent MCP Tools]
    participant TTS as [Piper TTS + Resampler]

    %% Phase 1: Wake & Start
    Note over User, ESP_C0: Phase 1: Activation
    ESP_C0->>ESP_C0: WakeNet detects local wake word "Hi ESP"
    ESP_C0->>ESP_C1: Play greeting chime
    ESP_C0->>ESP_C0: Pause wake-word engine (sr_pause)
    ESP_C0->>BE: Establish WebSocket to `/ws/conversation` with Auth headers

    %% Phase 2: Recording & Streaming
    Note over User, BE: Phase 2: Audio Ingestion
    loop While User is Speaking (Max 15 seconds)
        ESP_C0->>ESP_C0: Read 1024-byte raw PCM chunk from I2S Mic (16kHz)
        ESP_C0->>BE: Stream binary audio frame over WebSocket
        BE->>BE: Accumulate bytes in memory `pcm_buffer`
        ESP_C0->>ESP_C0: Run local VAD: measure peak amplitude
    end
    Note over ESP_C0: User stops speaking for 1.0 second (silence timer)
    ESP_C0->>BE: Send {"event": "speech_done"} control frame
    ESP_C0->>ESP_C0: Play local confirmation beep & show "Thinking" face

    %% Phase 3: Backend Processing & LLM Tool Loop
    Note over BE, Gemini: Phase 3: AI Inference & Tool Loop
    BE->>BE: Transcribe `pcm_buffer` using Whisper (tiny.en)
    BE->>ESP_C0: Send {"event": "transcript", "text": "..."}
    BE->>Gemini: Pass text query + chat history (SDK: google-genai)
    
    opt When Gemini decides tools are needed (e.g., Clipboard, Web Search, Weather)
        Gemini->>BE: Request tool call (function_calls)
        BE->>MCP: Execute tool on persistent MCP Session
        MCP-->>BE: Return tool output
        BE->>Gemini: Feed tool result back to LLM
    end
    
    Gemini->>BE: Stream output tokens

    %% Phase 4: TTS & Playback
    Note over BE, ESP_C1: Phase 4: Output Synthesis & Playback
    loop Stream Response Sentences
        BE->>BE: Group tokens into sentences (split on `.?!,;:`)
        BE->>TTS: Synthesize sentence using Piper (22.05kHz PCM)
        TTS->>TTS: Resample to 16kHz using NumPy interpolation
        TTS->>BE: Return 16kHz mono PCM chunks
        BE->>ESP_C0: Stream binary audio chunks over WebSocket
        ESP_C0->>PSRAM: Buffer chunks into 1 MB ring buffer (ringWrite)
        
        Note over ESP_C0: When buffer has >=16KB (pre-buffer) or stream done:
        ESP_C0->>ESP_C1: Spawn audioPlaybackTask
        loop Playback
            ESP_C1->>PSRAM: Read PCM bytes (ringRead)
            ESP_C1->>ESP_C1: Apply 3.5x Volume Boost & Clip limits
            ESP_C1->>ESP_C1: Output to I2S Speaker (MAX98357A)
        end
    end

    BE->>ESP_C0: Send {"event": "audio_done", "end_conversation": bool}
    ESP_C1->>ESP_C1: Playback ends, terminate audioPlaybackTask
    ESP_C0->>ESP_C0: Resume local wake-word engine (sr_resume)
```

### 1. Input Audio Capture (Edge)
* **MEMS Microphone:** An INMP441 microphone captures audio signals and digitizes them using an internal delta-sigma ADC. 
* **I2S Configuration:** Streamed over I2S (Inter-IC Sound) mono channel at $16,000\text{ Hz}$ sampling rate with $16\text{-bit}$ resolution.
* **On-Device VAD:** Core 0 measures the peak amplitude of each $32\text{ ms}$ chunk. If the amplitude drops below 600 (`SILENCE_THRESHOLD`) for more than $1.0\text{ s}$ (`END_SILENCE_MS`) after active speech, the loop breaks and a JSON control frame `{"event": "speech_done"}` is transmitted.

### 2. Bidirectional WebSocket Layer
* **Multiplexed Connection:** Operates over a single persistent WebSocket endpoint (`/ws/conversation`).
* **Client to Server (Uplink):** streams binary raw PCM chunks (1024 bytes) during speech, followed by textual JSON command packets.
* **Server to Client (Downlink):** transmits Whisper transcriptions as text JSON captions, and streams synthesized response voice as binary chunks (4KB).

### 3. Agentic Intelligence & MCP Tools
* **Personality RAG:** Injects the bot's identity, hardware specifications, and creator Sarthak Rajesh Saraf's credentials directly in-memory into the Gemini system prompt context window, allowing instant answers to identity-related queries.
* **Model Context Protocol (MCP):** Connects the Gemini LLM to a persistent background socket executing six key tools in under $300\text{ ms}$:
  1. `web_search`: Queries DuckDuckGo for live facts.
  2. `get_weather`: Translates city names and fetches coordinates via Open-Meteo geocoding.
  3. `sync_text_to_clipboard`: Syncs generated code/text blocks directly to Sarthak's Android clipboard via the Join API.
  4. `generate_document`: Dynamically compiles Word (`.docx`) or PDF documents and delivers them to the user via Telegram/Gmail.
  5. `get_location`: Resolves coordinates based on the server IP address.
  6. `get_current_time`: Obtains system time, timezone, and calendar date.

### 4. Resampling, Buffering & Output Playback (Edge)
* **Linear Audio Resampling:** Piper TTS outputs at $22.05\text{ kHz}$. To match the ESP32 hardware output, the backend uses pure-NumPy linear interpolation (`np.interp`) to resample the audio to $16\text{ kHz}$ in-memory.
* **PSRAM Ring Buffer:** The ESP32-S3 allocates a circular FIFO buffer of $1\text{ MB}$ inside its external SPI RAM (PSRAM) to store response audio streams, holding up to $32.7\text{ seconds}$ of speech to prevent network jitter.
* **Pre-Buffering & Digital Signal Processing (DSP):** Playback waits until the buffer accumulates $16\text{ KB}$ ($\sim 500\text{ ms}$) of audio to guarantee gapless playback. Before outputting to the I2S Class-D MAX98357A DAC, Core 1 applies a `3.5x` digital volume boost with software clipping limits.

---

## 🛠️ Hardware Setup & Pin Mapping

The physical model is built around the **ESP32-S3-DevKitC-1-N16R8** connected to a digital MEMS microphone, I2S DAC, and OLED display.

| Component | Signal | ESP32-S3 GPIO | Connection Description |
| :--- | :--- | :--- | :--- |
| **INMP441 Microphone** | VDD | 3.3V | Power Supply (3.3V) |
| | GND | GND | Ground Connection |
| | L/R | GND | Select Left Channel (Mono Mode) |
| | SCK (BCLK) | GPIO 16 | I2S Bit Clock |
| | WS (LRCK) | GPIO 15 | I2S Word Select / Frame Clock |
| | SD (DIN) | GPIO 7 | I2S Serial Data Input |
| **MAX98357A DAC Amp** | VIN | 5V | Power Supply (5V for high speaker volume) |
| | GND | GND | Ground Connection |
| | BCLK | GPIO 9 | I2S Bit Clock |
| | LRC (WS) | GPIO 46 | I2S Word Select |
| | DIN (DOUT) | GPIO 10 | I2S Serial Data Output |
| **SSD1306 OLED Display**| VCC | 3.3V | Power Supply (3.3V) |
| | GND | GND | Ground Connection |
| | SCL | GPIO 40 | I2C Clock Line |
| | SDA | GPIO 39 | I2C Data Line |

---

## 💻 Software Stack & Dependencies

### Edge Firmware (C++)
* **Framework:** Arduino ESP32 Core (using `pioarduino` platform fork supporting ESP-SR).
* **OLED Graphics:** `olikraus/U8g2` (handles fast monochrome page-rendering).
* **Communication:** `links2004/WebSockets` (asynchronous WebSocket client).
* **Serialization:** `bblanchon/ArduinoJson` (JSON parsing).
* **Acoustics:** Espressif Speech Recognition Framework (`ESP-SR` and `ESP-Skainet` models).

### Backend Server (Python)
* **Framework:** FastAPI / Uvicorn (Asynchronous Python ASGI backend).
* **Speech-to-Text:** Faster-Whisper (`tiny.en` model running via CTranslate2 on CPU).
* **Cognitive Agent:** Google Gemini SDK (`google-genai` package).
* **Text-to-Speech:** Piper TTS ONNX runtime (`en_US-lessac-medium.onnx`).
* **Database:** MongoDB Atlas (Motor Async driver).
* **Audio DSP:** NumPy, FFmpeg.

---

## 🚀 Installation & Deployment

### 1. Firmware Configuration & Flashing
1. Install **Visual Studio Code** and the **PlatformIO IDE** extension.
2. Clone the repository and open the `ESP32s3-Desktop_Bot` directory in VS Code.
3. Edit `src/boot/wifi_manager.cpp` (or `src/app/tasks/ConversationTask.cpp`) to set up your network and server configurations:
   ```cpp
   #define SERVER_HOST "192.168.1.9" // Change to your local backend server IP
   #define SERVER_PORT 5000
   ```
4. Verify your I2S and I2C pin declarations in `include/app_config.h`.
5. Connect your ESP32-S3 via USB, then compile and flash the firmware:
   ```bash
   pio run -t upload
   ```

### 2. Backend Server Deployment
1. Ensure `python 3.9+` and `ffmpeg` are installed on your host OS.
2. Navigate to the backend directory (`project_esp32`):
   ```bash
   cd ../project_esp32
   ```
3. Initialize the Python virtual environment and install packages:
   ```bash
   python -m venv venv
   source venv/bin/activate
   pip install -r requirements.txt
   ```
4. Download the Piper voice files and place them in the backend root directory:
   * [en_US-lessac-medium.onnx](https://github.com/rhasspy/piper/releases/download/v0.0.2/voice-en_US-lessac-medium.onnx)
   * [en_US-lessac-medium.onnx.json](https://github.com/rhasspy/piper/releases/download/v0.0.2/voice-en_US-lessac-medium.onnx.json)
5. Create a `.env` file in the root backend directory:
   ```env
   GEMINI_API_KEYS=your_gemini_api_key_here
   TELEGRAM_BOT_TOKEN=your_telegram_bot_token
   TELEGRAM_CHAT_ID=your_telegram_chat_id
   JOIN_API_KEY=your_join_api_key
   JOIN_DEVICE_ID=your_join_device_id
   MONGODB_URI=mongodb+srv://your_connection_string
   BOT_SECRET_KEY=dfgF.sd:Oklfgdhdsa034kJDJdsfbjsdnd/dsad
   ```
6. Run the FastAPI application:
   ```bash
   python run.py
   ```
   The backend will boot and listen on `0.0.0.0:5000`.

---

## 📊 Latency Profiling

The end-to-end voice-to-voice conversation latency benchmarked over a stable Wi-Fi connection is summarized below:

```
[0.00s] User stops speaking
[1.00s] VAD Silence Window (ESP32-S3 waits 1.0s to confirm end-of-speech)
[1.02s] VAD Transmit (ESP32-S3 transmits JSON "speech_done" control packet)
[1.05s] Network Uplink (Packet travels over Wi-Fi to FastAPI server)
[1.50s] transcription (Faster-Whisper processes final PCM buffer)
[1.95s] LLM Reasoning & Tool Call (Gemini agent processes text and returns reply)
[2.23s] TTS Generation (Piper synthesizes the first sentence to 22.05 kHz PCM)
[2.25s] Linear Resampling (NumPy resamples audio to 16 kHz in memory)
[2.28s] Network Downlink (FastAPI streams the first binary audio frame)
[2.78s] ESP32 Pre-buffering (Core 1 buffers 16KB of audio to prevent jitter)
[2.79s] Audio Playback (Speaker plays the first sound sample)
========================================================================
Total Latency (including VAD wait): ~2.79 seconds
Net System Processing Delay (excluding VAD wait): ~1.79 seconds
```

---

## 🔍 Troubleshooting & Optimization

1. **OLED Stays Blank:**
   * Double-check your I2C pin declarations in `app_config.h`. SSD1306 I2C addresses are usually `0x3C` or `0x3D`. Probing SDA/SCL lines with a logic analyzer can confirm clock generation at 400kHz.
2. **Audio Popping/Distortion:**
   * The MAX98357A requires a solid power connection. Powering it with 5V rather than 3.3V provides a cleaner, louder audio signal. Ensure the speaker terminals are securely soldered.
3. **Audio Playback Truncated:**
   * Ensure PlatformIO has successfully enabled PSRAM. Check your build log to verify the macro flags `-mfix-esp32-psram-cache-issue` and ensure your device uses an `N16R8` module that provides the 8MB Octal PSRAM.
4. **WebSocket Reconnecting Loops:**
   * The ESP32-S3 does not support 5GHz Wi-Fi networks. Ensure the router is broadcasting a 2.4GHz SSID and that the ESP32 is placed within range. Confirm the backend server is reachable on the local IP configured in `ConversationTask.cpp`.

---

## 📜 Acknowledgements & References
* Based on the original [ESP32-WakeWord](https://github.com/jahrulnr/ESP32-WakeWord) repository by jahrulnr.
* [Espressif ESP-SR Framework](https://github.com/espressif/esp-sr) for local wake word models.
* [U8g2 Library](https://github.com/olikraus/u8g2) by olikraus.
* [Piper TTS Engine](https://github.com/rhasspy/piper) by Rhasspy.
* Original face animations inspired by [ESP32-Eyes](https://github.com/playfultechnology/esp32-eyes).
* Developed under the academic supervision of Alard Institute of Management and Sciences, Pune.
