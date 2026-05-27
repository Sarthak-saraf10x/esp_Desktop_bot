#pragma once

#include "boot/init.h"
#include "display_list.h"

extern TaskHandle_t mainTaskHandle;
extern TaskHandle_t convTaskHandle;

// Shared display state (set by ConversationTask, read by mainTask):
// 0 = idle (Mochi animation), 1 = recording, 2 = speaking
extern volatile int g_displayState;

void runTasks();

void mainTask(void *param);
void conversationTask(void *param);
