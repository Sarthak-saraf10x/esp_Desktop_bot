#include "app/tasks.h"
#include <esp_log.h>
#include <esp32-hal-log.h>

TaskHandle_t mainTaskHandle = nullptr;

void mainTask(void *param) {
	const char* TAG = "mainTask";

  TickType_t lastWakeTime = xTaskGetTickCount();
  TickType_t updateFrequency = pdMS_TO_TICKS(61);
	size_t updateDelay = 0;
	const char* lastEvent;

	// wait notification initiate
	while (!notification)
		taskYIELD();


	while(1) {
		vTaskDelayUntil(&lastWakeTime, updateFrequency);
		display->clearBuffer();

		if (!notification->has(NOTIFICATION_DISPLAY) && updateDelay == 0) {
			displaySoundDetector();
	    display->sendBuffer();
			continue;
		}

		void* event = notification->has(NOTIFICATION_DISPLAY)
			? notification->consume(NOTIFICATION_DISPLAY, updateFrequency)
			: nullptr;
		if ((event && strcmp((const char*)event, EVENT_DISPLAY_WAKEWORD) == 0) || strcmp(lastEvent, EVENT_DISPLAY_WAKEWORD) == 0) {
			if (updateDelay == 0) {
				updateDelay = millis() + 3000;
				lastEvent = EVENT_DISPLAY_WAKEWORD;
				faceDisplay->LookFront();
				faceDisplay->Expression.GoTo_Happy();
			}
			displayHappyFace();
			// send buffer will handled by Face class
		}

		if (updateDelay <= millis()) {
			updateDelay = 0;
			lastEvent = "";
		}

		// Handle any notifications that might be relevant to SR
		if (notification && notification->has(NOTIFICATION_COMMAND)) {
			void* event = notification->consume(NOTIFICATION_COMMAND);
			if (event) {
				const char* command = (const char*)event;
				ESP_LOGI(TAG, "Received command notification: %s", command);

				// Handle command notifications if needed
				if (strcmp(command, "pause_sr") == 0) {
					ESP_LOGI(TAG, "Pausing speech recognition");
					SR::sr_pause();
				} else if (strcmp(command, "resume_sr") == 0) {
					ESP_LOGI(TAG, "Resuming speech recognition");
					SR::sr_resume();
				}
			}
		}
	}
}