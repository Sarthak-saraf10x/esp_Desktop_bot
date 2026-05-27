#include "app/tasks.h"
#include <esp_log.h>
#include <esp32-hal-log.h>
#include "Mochi.h"

TaskHandle_t mainTaskHandle = nullptr;

// Shared display state — written by ConversationTask, read by mainTask
// 0 = idle (Mochi animation)
// 1 = recording (faceDisplay Focused)
// 2 = speaking  (faceDisplay Happy)
volatile int g_displayState = 0;

void mainTask(void *param) {
    const char* TAG = "mainTask";

    TickType_t lastWakeTime = xTaskGetTickCount();
    // 30ms is about 33fps for smooth eye animation
    TickType_t updateFrequency = pdMS_TO_TICKS(30);

    // Wait for display to initialise
    while (!display)
        taskYIELD();

    while (1) {
        vTaskDelayUntil(&lastWakeTime, updateFrequency);

        // Always show the animated robot face
        if (faceDisplay) {
            display->clearBuffer();
            faceDisplay->Update();
        }
    }
}