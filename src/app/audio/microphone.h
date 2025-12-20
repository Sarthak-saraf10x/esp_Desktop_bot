#pragma once
#include <app_config.h>
#include <I2SMicrophone.h>
#include <AnalogMicrophone.h>

enum MIC_HW {
	MIC_ANALOG = 0,
	MIC_I2S,
	MIC_MAX
};

class Microphone {

public:
	Microphone(MIC_HW micType){
		this->micType = micType;
	}

	~Microphone(){
		switch (this->micType) {
		case MIC_ANALOG:
			if (amic) {
				delete amic;
			}
			break;
		case MIC_I2S:
			if (imic) {
				delete imic;
			}
			break;
		}
	}

	inline void init() {
		switch (this->micType) {
		case MIC_ANALOG:
			amic = new AnalogMicrophone(MIC_OUT, MIC_GAIN, MIC_AR);
			amic->init();
			amic->setGain(INPUT);
			amic->setAttackRelease(true);
			amic->start();
			break;
		case MIC_I2S:
			imic = new I2SMicrophone(
					(gpio_num_t)MIC_DIN,    // Data pin
					(gpio_num_t)MIC_SCK,    // Clock pin
					(gpio_num_t)MIC_WS,     // Word select pin
					I2S_NUM_0               // Port number
			);
			imic->init(16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
			imic->start();
			break;
		}
	}

	inline esp_err_t read(void *out, size_t len, size_t *bytes_read, uint32_t timeout_ms){
		switch (this->micType) {
		case MIC_ANALOG: {
			// Calculate how many 16-bit samples we need
			int samples_needed = len / sizeof(int16_t);
			int samples_read = 0;

			if (amic && amic->isActive()) {
				samples_read = amic->readSamples(
					reinterpret_cast<int16_t*>(out),
					samples_needed, 
					timeout_ms);
			}

			if (samples_read > 0) {
					*bytes_read = samples_read * sizeof(int16_t);
					return ESP_OK;
			}

			*bytes_read = 0;
			break;
		}
		case MIC_I2S:
			if (imic && imic->isActive()) {
				// Read audio samples from microphone
				size_t samples_requested = len / sizeof(int16_t);
				size_t samples_read = 0;

				auto res = imic->readSamples(
					reinterpret_cast<int16_t*>(out),
					samples_requested,
					&samples_read,
					pdTICKS_TO_MS(timeout_ms)
				);

				*bytes_read = samples_read * sizeof(int16_t);
				return res;
			}

			break;
		}

		return ESP_FAIL;
	};

	inline int level() {
		switch (this->micType) {
		case MIC_ANALOG:
			if (amic) {
				return amic->readLevel();
			}
			break;
		case MIC_I2S:
			if (imic) {
				return imic->readLevel();
			}
			break;
		}

		return 0;

	}

private:
	MIC_HW micType;
	I2SMicrophone* imic;
	AnalogMicrophone* amic;
};

extern Microphone* microphone;