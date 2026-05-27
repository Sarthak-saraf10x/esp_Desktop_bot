/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

#if !defined(ARDUINO_PARTITION_esp_sr_32) && !defined(ARDUINO_PARTITION_esp_sr_16) && !defined(ARDUINO_PARTITION_esp_sr_8)
#warning Compatible partition must be selected for ESP_SR to work
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "model_path.h"

#include "driver/i2s_common.h"
#include "csr.h"
#include "esp32-hal-log.h"

#undef ESP_GOTO_ON_FALSE
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, format, ...) \
  do {                                                        \
    if (unlikely(!(a))) {                                     \
      ESP_LOGE(SR::TAG, format, ##__VA_ARGS__);               \
      ret = err_code;                                         \
      goto goto_tag;                                          \
    }                                                         \
  } while (0)

#undef ESP_RETURN_ON_FALSE
#define ESP_RETURN_ON_FALSE(a, err_code, format, ...) \
  do {                                                \
    if (unlikely(!(a))) {                             \
      ESP_LOGE(SR::TAG, format, ##__VA_ARGS__);       \
      return err_code;                                \
    }                                                 \
  } while (0)

#define NEED_DELETE    BIT0
#define FEED_DELETED   BIT1
#define DETECT_DELETED BIT2
#define PAUSE_FEED     BIT3
#define PAUSE_DETECT   BIT4
#define RESUME_FEED    BIT5
#define RESUME_DETECT  BIT6


namespace SR {

typedef struct {
  wakenet_state_t wakenet_mode;
  esp_mn_state_t state;
  int command_id;
  int phrase_id;
} sr_result_t;

typedef struct {
  model_iface_data_t *model_data;
  const esp_mn_iface_t *multinet;
  const esp_afe_sr_iface_t *afe_handle;
  esp_afe_sr_data_t *afe_data;
  int16_t *afe_in_buffer;
  sr_mode_t mode;
  uint8_t i2s_rx_chan_num;
  sr_event_cb user_cb;
  void *user_cb_arg;
  sr_fill_cb fill_cb;
  void *fill_cb_arg;
  TaskHandle_t feed_task;
  TaskHandle_t detect_task;
  TaskHandle_t handle_task;
  QueueHandle_t result_que;
  EventGroupHandle_t event_group;
} sr_data_t;

static int SR_CHANNEL_NUM = 3;
static srmodel_list_t *models = NULL;
static SR::sr_data_t *g_sr_data = NULL;

const char* TAG = "CSR";

esp_err_t sr_set_mode(sr_mode_t mode);

void sr_handler_task(void *pvParam) {
  while (true) {
    taskYIELD();
    
    SR::sr_result_t result;
    if (xQueueReceive(SR::g_sr_data->result_que, &result, portMAX_DELAY) != pdTRUE) {
      ESP_LOGI(SR::TAG, "data nothing");
      continue;
    }

    if (WAKENET_DETECTED == result.wakenet_mode) {
      if (SR::g_sr_data->user_cb) {
        SR::g_sr_data->user_cb(SR::g_sr_data->user_cb_arg, SR_EVENT_WAKEWORD, -1, -1);
        ESP_LOGI(SR::TAG, "SR_EVENT_WAKEWORD");
      }
      continue;
    }

    if (WAKENET_CHANNEL_VERIFIED == result.wakenet_mode) {
      if (SR::g_sr_data->user_cb) {
        SR::g_sr_data->user_cb(SR::g_sr_data->user_cb_arg, SR_EVENT_WAKEWORD_CHANNEL, result.command_id, -1);
        ESP_LOGI(SR::TAG, "SR_EVENT_WAKEWORD_CHANNEL");
      }
      continue;
    }

    if (ESP_MN_STATE_DETECTED == result.state) {
      if (SR::g_sr_data->user_cb) {
        SR::g_sr_data->user_cb(SR::g_sr_data->user_cb_arg, SR_EVENT_COMMAND, result.command_id, result.phrase_id);
        ESP_LOGI(SR::TAG, "SR_EVENT_COMMAND");
      }
      continue;
    }

    if (ESP_MN_STATE_TIMEOUT == result.state) {
      if (SR::g_sr_data->user_cb) {
        SR::g_sr_data->user_cb(SR::g_sr_data->user_cb_arg, SR_EVENT_TIMEOUT, -1, -1);
        ESP_LOGI(SR::TAG, "SR_EVENT_TIMEOUT");
      }
      continue;
    }
  }
  vTaskDeleteWithCaps(NULL);
}

static void audio_feed_task(void *arg) {
  size_t bytes_read = 0;
  int audio_chunksize = SR::g_sr_data->afe_handle->get_feed_chunksize(SR::g_sr_data->afe_data);
  ESP_LOGI(SR::TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, SR_CHANNEL_NUM);

  /* Allocate audio buffer and check for result */
  int16_t *audio_buffer = (int16_t*) heap_caps_malloc(audio_chunksize * sizeof(int16_t) * SR_CHANNEL_NUM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (NULL == audio_buffer) {
    esp_system_abort("No mem for audio buffer");
  }
  SR::g_sr_data->afe_in_buffer = audio_buffer;
	TickType_t lastWakeTime = xTaskGetTickCount();
	TickType_t updateFrequency = 1;

  while (true) {
    EventBits_t bits = xEventGroupGetBits(SR::g_sr_data->event_group);
    if (NEED_DELETE & bits) {
      xEventGroupSetBits(SR::g_sr_data->event_group, FEED_DELETED);
      break;
    }
    if (PAUSE_FEED & bits) {
      xEventGroupWaitBits(SR::g_sr_data->event_group, PAUSE_FEED | RESUME_FEED, 1, 1, portMAX_DELAY);
    }

    /* Read audio data from I2S bus */
    //ToDo: handle error
    if (SR::g_sr_data->fill_cb == NULL) {
      ESP_LOGW(SR::TAG, "fill_cb is null");
      vTaskDelay(100);
      continue;
    }
    esp_err_t err = SR::g_sr_data->fill_cb(
      SR::g_sr_data->fill_cb_arg, (char *)audio_buffer, audio_chunksize * SR::g_sr_data->i2s_rx_chan_num * sizeof(int16_t), &bytes_read, portMAX_DELAY
    );
    if (err != ESP_OK) {
      ESP_LOGW(SR::TAG, "fill_cb is err: %s", esp_err_to_name(err));
      vTaskDelay(100);
      continue;
    }

    /* Channel Adjust */
    if (SR::g_sr_data->i2s_rx_chan_num == 1) {
      if (SR_CHANNEL_NUM == 3) {
          for (int i = audio_chunksize - 1; i >= 0; i--) {
            audio_buffer[i * SR_CHANNEL_NUM + 2] = 0;
            audio_buffer[i * SR_CHANNEL_NUM + 1] = 0;
            audio_buffer[i * SR_CHANNEL_NUM + 0] = audio_buffer[i];
          }
      }
    } else if (SR::g_sr_data->i2s_rx_chan_num == 2) {
      for (int i = audio_chunksize - 1; i >= 0; i--) {
        audio_buffer[i * SR_CHANNEL_NUM + 2] = 0;
        audio_buffer[i * SR_CHANNEL_NUM + 1] = audio_buffer[i * 2 + 1];
        audio_buffer[i * SR_CHANNEL_NUM + 0] = audio_buffer[i * 2 + 0];
      }
    } else {
      ESP_LOGW(SR::TAG, "i2s_rx_chan_num is invalid: %d", SR::g_sr_data->i2s_rx_chan_num);
      vTaskDelay(100);
      continue;
    }

    /* Feed samples of an audio stream to the AFE_SR */
    SR::g_sr_data->afe_handle->feed(SR::g_sr_data->afe_data, audio_buffer);

		vTaskDelayUntil(&lastWakeTime, updateFrequency);
  }
  vTaskDeleteWithCaps(NULL);
}

static void audio_detect_task(void *arg) {
  int afe_chunksize = SR::g_sr_data->afe_handle->get_fetch_chunksize(SR::g_sr_data->afe_data);
  int mu_chunksize = SR::g_sr_data->multinet->get_samp_chunksize(SR::g_sr_data->model_data);
  assert(mu_chunksize == afe_chunksize);
  ESP_LOGI(SR::TAG, "------------detect start------------");

  while (true) {
    taskYIELD();
   
    EventBits_t bits = xEventGroupGetBits(SR::g_sr_data->event_group);
    if (NEED_DELETE & bits) {
      xEventGroupSetBits(SR::g_sr_data->event_group, DETECT_DELETED);
      break;
    }
    if (PAUSE_DETECT & bits) {
      xEventGroupWaitBits(SR::g_sr_data->event_group, PAUSE_DETECT | RESUME_DETECT, 1, 1, portMAX_DELAY);
      continue;
    }

    afe_fetch_result_t *res = SR::g_sr_data->afe_handle->fetch(SR::g_sr_data->afe_data);
    if (!res || res->ret_value == ESP_FAIL) {
      ESP_LOGW(SR::TAG, "failed fetch afe data: %s", res != nullptr ? esp_err_to_name(res->ret_value) : "null");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (SR::g_sr_data->mode == SR_MODE_WAKEWORD) {
      if (res->wakeup_state == WAKENET_DETECTED) {
        ESP_LOGD(SR::TAG, "wakeword detected");
        SR::sr_result_t result = {
          .wakenet_mode = WAKENET_DETECTED,
          .state = ESP_MN_STATE_DETECTING,
          .command_id = 0,
          .phrase_id = 0,
        };
        xQueueSend(SR::g_sr_data->result_que, &result, 0);
      } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
        SR::sr_set_mode(SR_MODE_OFF);
        ESP_LOGD(SR::TAG, "AFE_FETCH_CHANNEL_VERIFIED, channel index: %d", res->trigger_channel_id);
        SR::sr_result_t result = {
          .wakenet_mode = WAKENET_CHANNEL_VERIFIED,
          .state = ESP_MN_STATE_DETECTING,
          .command_id = res->trigger_channel_id,
          .phrase_id = 0,
        };
        xQueueSend(SR::g_sr_data->result_que, &result, 0);
      }
    }

    if (SR::g_sr_data->mode == SR_MODE_COMMAND) {

      esp_mn_state_t mn_state = ESP_MN_STATE_DETECTING;
      mn_state = SR::g_sr_data->multinet->detect(SR::g_sr_data->model_data, res->data);

      if (ESP_MN_STATE_DETECTING == mn_state) {
        continue;
      }

      if (ESP_MN_STATE_TIMEOUT == mn_state) {
        esp_mn_results_t *mn_result = SR::g_sr_data->multinet->get_results(SR::g_sr_data->model_data);
        SR::sr_set_mode(SR_MODE_OFF);
        ESP_LOGD(SR::TAG, "Time out, text: %s", mn_result->string);
        SR::sr_result_t result = {
          .wakenet_mode = WAKENET_NO_DETECT,
          .state = mn_state,
          .command_id = 0,
          .phrase_id = 0,
        };
        xQueueSend(SR::g_sr_data->result_que, &result, 0);
        continue;
      }

      if (ESP_MN_STATE_DETECTED == mn_state) {
        SR::sr_set_mode(SR_MODE_OFF);
        esp_mn_results_t *mn_result = SR::g_sr_data->multinet->get_results(SR::g_sr_data->model_data);
        for (int i = 0; i < mn_result->num; i++) {
          ESP_LOGD(SR::TAG, "TOP %d, command_id: %d, phrase_id: %d, prob: %f", i + 1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->prob[i]);
          if (i % 10 == 0) taskYIELD();
        }

        int sr_command_id = mn_result->command_id[0];
        int sr_phrase_id = mn_result->phrase_id[0];
        ESP_LOGD(SR::TAG, "Detected command : %d, phrase: %d", sr_command_id, sr_phrase_id);
        SR::sr_result_t result = {
          .wakenet_mode = WAKENET_NO_DETECT,
          .state = mn_state,
          .command_id = sr_command_id,
          .phrase_id = sr_phrase_id,
        };
        xQueueSend(SR::g_sr_data->result_que, &result, 0);
        continue;
      }
      ESP_LOGE(SR::TAG, "Exception unhandled");
    }
  }
  vTaskDeleteWithCaps(NULL);
}

esp_err_t sr_set_mode(sr_mode_t mode) {
  ESP_RETURN_ON_FALSE(NULL != SR::g_sr_data, ESP_ERR_INVALID_STATE, "SR is not running");
  switch (mode) {
    case SR_MODE_OFF:
      if (SR::g_sr_data->mode == SR_MODE_WAKEWORD) {
        SR::g_sr_data->afe_handle->disable_wakenet(SR::g_sr_data->afe_data);
      }
      break;
    case SR_MODE_WAKEWORD:
      if (SR::g_sr_data->mode != SR_MODE_WAKEWORD) {
        SR::g_sr_data->afe_handle->enable_wakenet(SR::g_sr_data->afe_data);
      }
      break;
    case SR_MODE_COMMAND:
      if (SR::g_sr_data->mode == SR_MODE_WAKEWORD) {
        SR::g_sr_data->afe_handle->disable_wakenet(SR::g_sr_data->afe_data);
      }
      break;
    default: return ESP_FAIL;
  }
  SR::g_sr_data->mode = mode;
  return ESP_OK;
}

esp_err_t sr_setup(
  sr_fill_cb fill_cb, void *fill_cb_arg, sr_channels_t rx_chan, sr_mode_t mode, const SR::csr_cmd_t sr_commands[], size_t cmd_number, sr_event_cb cb, void *cb_arg
) {
  if(NULL != SR::g_sr_data){
    ESP_LOGE(SR::TAG, "SR already running");
    return ESP_ERR_INVALID_STATE;
  }

  SR::g_sr_data = (SR::sr_data_t*) heap_caps_calloc(1, sizeof(SR::sr_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if(NULL == SR::g_sr_data){
    ESP_LOGE(SR::TAG, "Failed create sr data");
   ::sr_stop();
    return ESP_ERR_NO_MEM;
  }

  SR::g_sr_data->result_que = xQueueCreate(3, sizeof(SR::sr_result_t));
  if(NULL == SR::g_sr_data->result_que) {
    ESP_LOGE(SR::TAG, "Failed create result queue");
    ::sr_stop();
    return ESP_ERR_NO_MEM;
  }

  SR::g_sr_data->event_group = xEventGroupCreate();
  if(NULL == SR::g_sr_data->event_group) {
    ESP_LOGE(SR::TAG, "Failed create event_group");
    ::sr_stop();
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ret_val;
  SR::g_sr_data->user_cb = cb;
  SR::g_sr_data->user_cb_arg = cb_arg;
  SR::g_sr_data->fill_cb = fill_cb;
  SR::g_sr_data->fill_cb_arg = fill_cb_arg;
  SR::g_sr_data->i2s_rx_chan_num = rx_chan + 1;
  SR::g_sr_data->mode = mode;

  // Init Model
  ESP_LOGD(SR::TAG, "init model");
  SR::models = esp_srmodel_init("model");
  
  if (SR::models == nullptr) {
      ESP_LOGE(SR::TAG, "Failed to load WakeWord/MN models! Did you flash srmodels.bin to the model partition?");
      return ESP_FAIL;
  }

  // Load WakeWord Detection
  // https://docs.espressif.com/projects/esp-sr/en/latest/esp32/audio_front_end/migration_guide.html
  afe_config_t *afe_config;
  if (rx_chan == SR_CHANNELS_MONO) {
      SR_CHANNEL_NUM = 1;
      afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
  } else {
      SR_CHANNEL_NUM = 3;
      afe_config = afe_config_init("MMR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
  }
  
  afe_config->wakenet_model_name = esp_srmodel_filter(SR::models, ESP_WN_PREFIX, "hiesp");
  afe_config->aec_init = false;
  afe_config = afe_config_check(afe_config);
  afe_config_print(afe_config);
  SR::g_sr_data->afe_handle = esp_afe_handle_from_config(afe_config);
  ESP_LOGD(SR::TAG, "load wakenet '%s'", afe_config->wakenet_model_name);
  SR::g_sr_data->afe_data = SR::g_sr_data->afe_handle->create_from_config(afe_config);

  // Lower WakeNet threshold for faster/easier wake word detection.
  // Default is usually ~0.9 (very strict). 0.5 gives a good balance:
  // lower = more sensitive (easier to trigger, may false-fire),
  // higher = stricter (need very clear pronunciation).
  // Adjust between 0.3 (very loose) – 0.9 (very strict) to taste.
  if (SR::g_sr_data->afe_handle->set_wakenet_threshold) {
    SR::g_sr_data->afe_handle->set_wakenet_threshold(SR::g_sr_data->afe_data, 1, 0.5f);
    ESP_LOGI(SR::TAG, "WakeNet threshold set to 0.5 (more sensitive)");
  }

  // Load Custom Command Detection
  char *mn_name = esp_srmodel_filter(SR::models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  ESP_LOGD(SR::TAG, "load multinet '%s'", mn_name);
  SR::g_sr_data->multinet = esp_mn_handle_from_name(mn_name);
  ESP_LOGD(SR::TAG, "load model_data '%s'", mn_name);
  SR::g_sr_data->model_data = SR::g_sr_data->multinet->create(mn_name, 5760);

  // Add commands
  esp_mn_commands_alloc((esp_mn_iface_t *)SR::g_sr_data->multinet, (model_iface_data_t *)SR::g_sr_data->model_data);
  ESP_LOGI(SR::TAG, "add %d commands", cmd_number);
  for (size_t i = 0; i < cmd_number; i++) {
    esp_mn_commands_add(sr_commands[i].command_id, (char *)(sr_commands[i].phoneme));
    ESP_LOGI(SR::TAG, "  cmd[%d] phrase[%d]:'%s'", sr_commands[i].command_id, i, sr_commands[i].str);
    if (i % 5 == 0) taskYIELD();
  }

  // Load commands
  esp_mn_error_t *err_id = esp_mn_commands_update();
  if (err_id) {
    for (int i = 0; i < err_id->num; i++) {
      ESP_LOGE(SR::TAG, "err cmd id:%d", err_id->phrases[i]->command_id);
      if (i % 5 == 0) taskYIELD();
    }
  }
  
  return ESP_OK;
}

esp_err_t sr_start(int feedCore, int detectCore) {
  //Start tasks
  esp_err_t ret_val = xTaskCreateUniversal(&SR::audio_feed_task, "SR Feed Task", 4 * 1024, NULL, 6, &SR::g_sr_data->feed_task, feedCore);
  if(pdPASS != ret_val) {
    ESP_LOGE(SR::TAG, "Failed create audio feed task");
    ::sr_stop();
    return ESP_FAIL;
  }

  vTaskDelay(10);
  ret_val = xTaskCreateUniversal(&SR::audio_detect_task, "SR Detect Task", 8 * 1024, NULL, 8, &SR::g_sr_data->detect_task, detectCore);
  if(pdPASS != ret_val) {
    ESP_LOGE(SR::TAG, "Failed create audio detect task");
    ::sr_stop();
    return ESP_FAIL;
  }

  ret_val = xTaskCreateUniversal(&SR::sr_handler_task, "SR Handler Task", 6 * 1024, NULL, configMAX_PRIORITIES - 1, &SR::g_sr_data->handle_task, feedCore);
  if(pdPASS != ret_val) {
    ESP_LOGE(SR::TAG, "Failed create audio handler task");
    ::sr_stop();
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t sr_stop(void) {
  ESP_RETURN_ON_FALSE(NULL != SR::g_sr_data, ESP_ERR_INVALID_STATE, "SR is not running");

  /**
     * Waiting for all task stopped
     * TODO: A task creation failure cannot be handled correctly now
     * */
  vTaskDeleteWithCaps(SR::g_sr_data->handle_task);
  xEventGroupSetBits(SR::g_sr_data->event_group, NEED_DELETE);
  xEventGroupWaitBits(SR::g_sr_data->event_group, NEED_DELETE | FEED_DELETED | DETECT_DELETED, 1, 1, portMAX_DELAY);

  if (SR::g_sr_data->result_que) {
    vQueueDelete(SR::g_sr_data->result_que);
    SR::g_sr_data->result_que = NULL;
  }

  if (SR::g_sr_data->event_group) {
    vEventGroupDelete(SR::g_sr_data->event_group);
    SR::g_sr_data->event_group = NULL;
  }

  if (SR::g_sr_data->model_data) {
    SR::g_sr_data->multinet->destroy(SR::g_sr_data->model_data);
  }

  if (SR::g_sr_data->afe_data) {
    SR::g_sr_data->afe_handle->destroy(SR::g_sr_data->afe_data);
  }

  if (SR::g_sr_data->afe_in_buffer) {
    heap_caps_free(SR::g_sr_data->afe_in_buffer);
    SR::g_sr_data->afe_in_buffer = nullptr;
  }

  heap_caps_free(SR::g_sr_data);
  SR::g_sr_data = NULL;
  return ESP_OK;
}

esp_err_t sr_pause(void) {
  ESP_RETURN_ON_FALSE(NULL != SR::g_sr_data, ESP_ERR_INVALID_STATE, "SR is not running");
  xEventGroupSetBits(SR::g_sr_data->event_group, PAUSE_FEED | PAUSE_DETECT);
  ESP_LOGI(TAG, "SR paused");
  return ESP_OK;
}

esp_err_t sr_resume(void) {
  ESP_RETURN_ON_FALSE(NULL != SR::g_sr_data, ESP_ERR_INVALID_STATE, "SR is not running");
  xEventGroupSetBits(SR::g_sr_data->event_group, RESUME_FEED | RESUME_DETECT);
  ESP_LOGI(TAG, "resuming SR");
  return ESP_OK;
}

}

#endif  // CONFIG_IDF_TARGET_ESP32S3
