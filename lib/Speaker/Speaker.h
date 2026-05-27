#ifndef SPEAKER_H
#define SPEAKER_H

#include <Arduino.h>
#include <driver/i2s_std.h>

class Speaker {
public:
    void begin();
    void playTone(int frequency, int duration_ms);
    void playChime();
    void beep();
    void playPCM(const uint8_t* data, size_t len);
    void silence();          // flush DMA with zeros
    void stop();
    void setSampleRate(uint32_t sampleRate);  // reconfigure clock for WAV playback

private:
    void setupI2S();
    void generateTone(int frequency, int duration_ms);
    bool initialized = false;
    uint32_t currentSampleRate = 16000;
    i2s_chan_handle_t tx_handle = nullptr;
};

extern Speaker GlobalSpeaker;

#endif