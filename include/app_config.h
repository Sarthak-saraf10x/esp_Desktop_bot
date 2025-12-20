#pragma once

#define SCL_PIN 3
#define SDA_PIN SDA

// set to analog or i2s microphone
// #define MIC_TYPE MIC_TYPE_ANALOG
#define MIC_TYPE MIC_I2S

// i2s microphone
#ifdef SEED_XIAO_ESP32S3
#define MIC_SCK GPIO_NUM_42
#define MIC_WS  GPIO_NUM_NC
#define MIC_DIN GPIO_NUM_41
#else
#define MIC_SCK GPIO_NUM_13
#define MIC_WS  GPIO_NUM_12
#define MIC_DIN GPIO_NUM_11
#endif

// analog microphone
#define MIC_AR   GPIO_NUM_39
#define MIC_OUT	 GPIO_NUM_4 // esp32-s3 range pin (0-20)
#define MIC_GAIN GPIO_NUM_38

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64