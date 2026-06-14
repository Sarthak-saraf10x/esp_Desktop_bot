import os
import wave
import subprocess
import piper
import numpy as np

# Greetings dictionary
greetings = {
    "listening": "I'm listening",
    "do_for_you": "Hello! What can I do for you?",
    "assist_you": "Hey! How can I assist you right now?",
    "on_your_mind": "Hey there! What's on your mind?"
}

# Create temp directories
os.makedirs("temp_audio", exist_ok=True)

# Path to the piper model
model_path = "../project_esp32/en_US-lessac-medium.onnx"
print("Loading Piper Model...")
piper_voice = piper.PiperVoice.load(model_path)
print("Piper Model loaded.")

header_content = """#pragma once
#include <Arduino.h>

struct GreetingAudio {
    const char* name;
    const uint8_t* data;
    size_t length;
};
"""

for key, text in greetings.items():
    print(f"Generating audio for: '{text}'...")
    
    # Paths
    temp_wav = f"temp_audio/{key}_temp.wav"
    raw_path = f"temp_audio/{key}.raw"
    
    # Synthesize to wav file using wave module
    with wave.open(temp_wav, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(22050) # Piper en_US-lessac-medium is 22050Hz
        
        # Synthesize audio stream
        audio_stream = piper_voice.synthesize(text)
        for audio_chunk in audio_stream:
            audio_data = None
            if hasattr(audio_chunk, 'audio_int16_bytes') and audio_chunk.audio_int16_bytes:
                audio_data = audio_chunk.audio_int16_bytes
            elif hasattr(audio_chunk, '_audio_int16_bytes') and audio_chunk._audio_int16_bytes:
                audio_data = audio_chunk._audio_int16_bytes
            elif hasattr(audio_chunk, 'audio_int16_array') and audio_chunk.audio_int16_array is not None:
                audio_data = audio_chunk.audio_int16_array.tobytes()
            elif hasattr(audio_chunk, '_audio_int16_array') and audio_chunk._audio_int16_array is not None:
                audio_data = audio_chunk._audio_int16_array.tobytes()
            elif hasattr(audio_chunk, 'audio_float_array') and audio_chunk.audio_float_array is not None:
                float_array = audio_chunk.audio_float_array
                int16_array = (float_array * 32767).astype(np.int16)
                audio_data = int16_array.tobytes()
            else:
                continue
            
            if audio_data:
                wav_file.writeframes(audio_data)
            
    # Convert wav to 16kHz, 16-bit, mono PCM raw
    res = subprocess.run([
        "ffmpeg", "-y", "-i", temp_wav, 
        "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", "16000", 
        raw_path
    ], capture_output=True, text=True)
    
    if res.returncode != 0:
        print("FFmpeg error:", res.stderr)
        exit(1)
        
    # Read raw data
    with open(raw_path, "rb") as f:
        pcm_data = f.read()
        
    # Generate C array
    array_name = f"AUDIO_GREETING_{key.upper()}"
    header_content += f"\n// '{text}' - {len(pcm_data)} bytes\n"
    header_content += f"const uint8_t {array_name}[] PROGMEM = {{\n"
    
    # Write bytes formatted as hex
    hex_lines = []
    for i in range(0, len(pcm_data), 12):
        chunk = pcm_data[i:i+12]
        hex_lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk))
    header_content += ",\n".join(hex_lines) + "\n};\n"
    
    # Cleanup temp files
    if os.path.exists(temp_wav):
        os.unlink(temp_wav)
    if os.path.exists(raw_path):
        os.unlink(raw_path)

# Add structure array
header_content += "\nconst GreetingAudio GREETINGS_LIST[] = {\n"
for key, text in greetings.items():
    array_name = f"AUDIO_GREETING_{key.upper()}"
    header_content += f'    {{ "{key}", {array_name}, sizeof({array_name}) }},\n'
header_content += "};\n"
header_content += f"const size_t GREETINGS_COUNT = {len(greetings)};\n"

# Save header
output_h_path = "/home/sarthak-10/code/esp/ESP32s3-Desktop_Bot/include/greetings_audio.h"
with open(output_h_path, "w") as f:
    f.write(header_content)

print(f"Header file successfully written to {output_h_path}")
