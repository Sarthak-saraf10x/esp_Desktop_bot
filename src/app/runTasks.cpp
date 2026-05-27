#include "tasks.h"
#include <esp_task_wdt.h>

void runTasks(){
	// Configure Task Watchdog Timer to prevent system hangs
	esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *config);
	esp_task_wdt_config_t config = {
			.timeout_ms = 5 * 1000,
			.trigger_panic = false,
	};
	esp_task_wdt_reconfigure(&config);

	xTaskCreateUniversal(
		mainTask,
		"mainTask",
		1024 * 4,
		NULL,
		4,
		&mainTaskHandle,
		0
	);

	xTaskCreateUniversal(
		conversationTask,
		"convTask",
		1024 * 8, // Need more stack for HTTPClient and PSRAM allocation
		NULL,
		3,
		&convTaskHandle,
		0
	);
}