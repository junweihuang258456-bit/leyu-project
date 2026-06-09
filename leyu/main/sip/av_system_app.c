/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "av_system_app.h"
#include "audio_processor.h"
#include "esp_board_manager_adapter.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG __attribute__((unused)) = "AV_SYS_APP";

#define CAMERA_SCCB_I2C_PORT 1
#define CAMERA_PIN_SIOD GPIO_NUM_12
#define CAMERA_PIN_SIOC GPIO_NUM_9

#define FILL_AUDIO_INFO(info, rate, bits, ch, dur)                             \
  do {                                                                         \
    (info).sample_rate = (rate);                                               \
    (info).sample_bits = (bits);                                               \
    (info).channels = (ch);                                                    \
    (info).frame_duration = (dur);                                             \
  } while (0)

typedef struct {
  esp_codec_dev_handle_t play_dev;
  esp_codec_dev_handle_t rec_dev;
  int sample_rate;
  int sample_bits;
  int channels;
  int width;
  int height;
  audio_player_state_t playback_state;
} av_system_app_t;

static av_system_app_t s_av_system_app = {0};

static void playback_event_callback_fn(audio_player_state_t state, void *ctx) {
  s_av_system_app.playback_state = state;
}

esp_err_t audio_system_app_restart(void *play_dev_handle,
                                   void *rec_dev_handle) {
  audio_manager_config_t audio_manager_config = DEFAULT_AUDIO_MANAGER_CONFIG();
  audio_manager_config.play_dev = play_dev_handle;
  audio_manager_config.rec_dev = rec_dev_handle;
  audio_manager_config.board_sample_rate = 8000;
  audio_manager_config.board_bits = 8;
  audio_manager_config.board_channels = 1;
  // audio_manager_config.mic_layout = "RM";
  return ESP_OK;
}

esp_err_t audio_system_app_init(audio_sys_config_t *config) {
  static av_processor_afe_config_t afe_config =
      DEFAULT_AV_PROCESSOR_AFE_CONFIG();
  audio_manager_config_t audio_manager_config = DEFAULT_AUDIO_MANAGER_CONFIG();

  esp_board_manager_adapter_info_t bsp_info = {0};
  esp_board_manager_adapter_config_t bsp_config =
      ESP_BOARD_MANAGER_ADAPTER_CONFIG_DEFAULT();
  bsp_config.enable_audio = true;
  esp_board_manager_adapter_init(&bsp_config, &bsp_info);
  audio_manager_config.play_dev = bsp_info.play_dev;
  audio_manager_config.rec_dev = bsp_info.rec_dev;
  strcpy(audio_manager_config.mic_layout, bsp_info.mic_layout);
  audio_manager_config.board_sample_rate = bsp_info.sample_rate;
  audio_manager_config.board_bits = bsp_info.sample_bits;
  audio_manager_config.board_channels = bsp_info.channels;
  audio_manager_init(&audio_manager_config);

  esp_codec_dev_set_out_vol(audio_manager_config.play_dev, 80);
  esp_codec_dev_set_in_gain(audio_manager_config.rec_dev, 36.0);
  s_av_system_app.play_dev = audio_manager_config.play_dev;

  esp_codec_dev_sample_info_t fs = {
      .sample_rate = audio_manager_config.board_sample_rate,
      .channel = audio_manager_config.board_channels,
      .bits_per_sample = audio_manager_config.board_bits,
  };
  esp_codec_dev_open(audio_manager_config.rec_dev, &fs);
  esp_codec_dev_open(audio_manager_config.play_dev, &fs);

  audio_prompt_config_t prompt_config = DEFAULT_AUDIO_PROMPT_CONFIG();
  prompt_config.prompt_task_config.task_stack_in_ext = false;
  audio_prompt_open(&prompt_config);

  av_processor_encoder_config_t recorder_cfg = {0};
#if defined(CONFIG_AUDIO_FORMAT_ID_OPUS)
  recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
  FILL_AUDIO_INFO(recorder_cfg.params.opus.audio_info, 16000, 16, 1, 20);
  recorder_cfg.params.opus.bitrate = 16000;
  recorder_cfg.params.opus.enable_vbr = true;
#elif defined(CONFIG_AUDIO_FORMAT_ID_G711A)
  recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
  FILL_AUDIO_INFO(recorder_cfg.params.g711.audio_info, 8000, 16, 1, 20);
#elif defined(CONFIG_AUDIO_FORMAT_ID_PCM)
  recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
  FILL_AUDIO_INFO(recorder_cfg.params.pcm.audio_info, 16000, 16, 1, 20);
#endif /* defined(CONFIG_AUDIO_FORMAT_ID_OPUS) */

  audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
  memcpy((void *)&recorder_config.encoder_cfg, &recorder_cfg,
         sizeof(av_processor_encoder_config_t));
  memcpy(&recorder_config.afe_config, &afe_config,
         sizeof(av_processor_afe_config_t));
  recorder_config.recorder_event_cb = config->recorder_event_cb;
  recorder_config.recorder_ctx = NULL;
  recorder_config.afe_fetch_task_config.task_stack = 10 * 1024;
  audio_recorder_open(&recorder_config);

  av_processor_decoder_config_t feeder_cfg = {0};
#if defined(CONFIG_AUDIO_FORMAT_ID_OPUS)
  feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
  FILL_AUDIO_INFO(feeder_cfg.params.opus.audio_info, 16000, 16, 1, 20);
#elif defined(CONFIG_AUDIO_FORMAT_ID_G711A)
  feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
  FILL_AUDIO_INFO(feeder_cfg.params.g711.audio_info, 8000, 16, 1, 20);
#elif defined(CONFIG_AUDIO_FORMAT_ID_PCM)
  feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
  FILL_AUDIO_INFO(feeder_cfg.params.pcm.audio_info, 16000, 16, 1, 20);
#endif /* defined(CONFIG_AUDIO_FORMAT_ID_OPUS) */
  audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
  memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
         sizeof(av_processor_decoder_config_t));
  audio_feeder_open(&feeder_config);
  // audio_processor_mixer_open();

  audio_feeder_run();
  return ESP_OK;
}

esp_err_t audio_system_app_open(void *play_dev_handle, void *rec_dev_handle,
                                bool mode) {
  static av_processor_afe_config_t afe_config =
      DEFAULT_AV_PROCESSOR_AFE_CONFIG();
//   esp_codec_dev_sample_info_t fs = {
//       .sample_rate = 8000,
//       .channel = 1,
//       .bits_per_sample = 16,
//   };
//   esp_codec_dev_open(rec_dev_handle, &fs);
//   esp_codec_dev_open(play_dev_handle, &fs);

//   audio_prompt_config_t prompt_config = DEFAULT_AUDIO_PROMPT_CONFIG();
//   prompt_config.prompt_task_config.task_stack_in_ext = false;
//   audio_prompt_open(&prompt_config);

  av_processor_encoder_config_t recorder_cfg = {0};

  if (mode) {
    recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
    FILL_AUDIO_INFO(recorder_cfg.params.g711.audio_info, 8000, 16, 1, 20);
    printf("G711A-mode\n");
  } else {
    recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    FILL_AUDIO_INFO(recorder_cfg.params.pcm.audio_info, 16000, 16, 1, 20);
  }

  audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
  memcpy((void *)&recorder_config.encoder_cfg, &recorder_cfg,
         sizeof(av_processor_encoder_config_t));
  memcpy(&recorder_config.afe_config, &afe_config,
         sizeof(av_processor_afe_config_t));
  recorder_config.recorder_event_cb = NULL;
  recorder_config.recorder_ctx = NULL;
  recorder_config.afe_fetch_task_config.task_core = 1;
  recorder_config.afe_fetch_task_config.task_stack = 10 * 1024;
  audio_recorder_open(&recorder_config);

  av_processor_decoder_config_t feeder_cfg = {0};
  if (mode) {
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
    FILL_AUDIO_INFO(feeder_cfg.params.g711.audio_info, 8000, 16, 1, 20);
  } else {
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    FILL_AUDIO_INFO(feeder_cfg.params.pcm.audio_info, 16000, 16, 1, 20);
  }

  audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
  memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
         sizeof(av_processor_decoder_config_t));
  audio_feeder_open(&feeder_config);
  // audio_processor_mixer_open();

  audio_feeder_run();
  return ESP_OK;
}

esp_err_t audio_system_app_close(void *play_dev_handle, void *rec_dev_handle) {
  audio_feeder_stop();
  audio_recorder_close();
  audio_feeder_close();
//   esp_codec_dev_close(rec_dev_handle);
//   esp_codec_dev_close(play_dev_handle);
  return ESP_OK;
}

audio_player_state_t audio_system_app_get_playback_state(void) {
  return s_av_system_app.playback_state;
}

void *audio_system_app_get_playback_dev(void) {
  return s_av_system_app.play_dev;
}
