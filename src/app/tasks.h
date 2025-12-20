#pragma once

#include "boot/init.h"
#include "display_list.h"

extern TaskHandle_t mainTaskHandle;

void runTasks();

void mainTask(void *param);
