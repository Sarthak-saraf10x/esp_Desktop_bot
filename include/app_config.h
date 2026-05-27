#pragma once

#define SCL_PIN 40      // Change from 3 to 40 (your OLED SCL)
#define SDA_PIN 39      // Change from SDA to 39 (your OLED SDA)
// set to analog or i2s microphone
// #define MIC_TYPE MIC_TYPE_ANALOG
#define MIC_TYPE MIC_I2S

#ifdef SEED_XIAO_ESP32S3
#define MIC_SCK GPIO_NUM_42
#define MIC_WS  GPIO_NUM_NC
#define MIC_DIN GPIO_NUM_41
#else
#define MIC_SCK GPIO_NUM_16    // Change from 13 to 16 (your mic BCLK)
#define MIC_WS  GPIO_NUM_15    // Change from 12 to 15 (your mic WS/LRCLK)
#define MIC_DIN GPIO_NUM_7     // Change from 11 to 7 (your mic SD/DOUT)
#endif

// analog microphone
#define MIC_AR   GPIO_NUM_39
#define MIC_OUT	 GPIO_NUM_4 // esp32-s3 range pin (0-20)
#define MIC_GAIN GPIO_NUM_38

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
// ============ SPEAKER (I2S Output) ============
#define I2S_SPK_BCLK GPIO_NUM_9
#define I2S_SPK_WS   GPIO_NUM_46
#define I2S_SPK_DIN  GPIO_NUM_10

// Optional: For playing tones/audio feedback
#define ENABLE_SPEAKER_FEEDBACK 1