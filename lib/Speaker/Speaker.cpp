#include "Speaker.h"
#include "esp_log.h"
#include "../../include/app_config.h"

static const char* TAG = "SPEAKER";

void Speaker::begin() {
    if (initialized) return;
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    // Automatically write zeros to DMA when idle — prevents buzzing
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create new I2S channel: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SPK_BCLK,
            .ws   = (gpio_num_t)I2S_SPK_WS,
            .dout = (gpio_num_t)I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init std mode: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    initialized = true;
    currentSampleRate = 16000;
    ESP_LOGI(TAG, "Speaker initialized (auto_clear_after_cb=true)");
}

void Speaker::setSampleRate(uint32_t sampleRate) {
    if (!initialized || sampleRate == currentSampleRate) return;
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    esp_err_t err = i2s_channel_disable(tx_handle);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
        if (err == ESP_OK) {
            currentSampleRate = sampleRate;
            ESP_LOGI(TAG, "Speaker sample rate -> %lu Hz", (unsigned long)sampleRate);
        } else {
            ESP_LOGE(TAG, "Failed to reconfig clock: %s", esp_err_to_name(err));
        }
        i2s_channel_enable(tx_handle);
    }
}

void Speaker::generateTone(int frequency, int duration_ms) {
    if (!initialized) return;

    int sample_rate = 16000;
    int samples = (sample_rate * duration_ms) / 1000;
    int16_t* audio_buffer = (int16_t*)malloc(samples * sizeof(int16_t));
    if (audio_buffer == NULL) return;

    float period_samples = (float)sample_rate / frequency;
    for (int i = 0; i < samples; i++) {
        audio_buffer[i] = (int16_t)((int)(i / period_samples) % 2) ? 20000 : -20000;
    }

    size_t bytes_written;
    i2s_channel_write(tx_handle, audio_buffer, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    free(audio_buffer);
}

void Speaker::playTone(int frequency, int duration_ms) {
    generateTone(frequency, duration_ms);
}

void Speaker::playChime() {
    playTone(880, 100);
    delay(50);
    playTone(1760, 150);
}

void Speaker::beep() {
    playTone(2000, 50);
}

void Speaker::playPCM(const uint8_t* data, size_t len) {
    if (!initialized) return;
    size_t bytes_written;
    i2s_channel_write(tx_handle, data, len, &bytes_written, portMAX_DELAY);
}

void Speaker::silence() {
    if (!initialized) return;
    // Write one buffer of zeros to flush and silence DMA
    const size_t SILENCE_SAMPLES = 512;
    int16_t zeros[SILENCE_SAMPLES] = {};
    size_t bytes_written;
    i2s_channel_write(tx_handle, zeros, sizeof(zeros), &bytes_written, 100);
}

void Speaker::stop() {
    silence();
}

Speaker GlobalSpeaker;