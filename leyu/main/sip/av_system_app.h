/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_gmf_afe.h"
#include "audio_processor.h"
#include "video_processor.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  Audio system configuration
 */
typedef struct {
    recorder_event_callback_t  recorder_event_cb;  /*!< Callback for recorder events (e.g. VAD, wakeup) */
} audio_sys_config_t;

/**
 * @brief  Video system configuration
 */
typedef struct {
    video_render_decode_callback_t  video_render_decode_cb;  /*!< Callback for video decode and display */
    video_capture_frame_callback_t  video_capture_frame_cb;  /*!< Callback for captured video frames */
} video_sys_config_t;

/**
 * @brief  Initialize the audio system application
 *
 * @param[in]  config  Audio system configuration, can be NULL
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t audio_system_app_init(audio_sys_config_t *config);

/**
 * @brief  Get current audio playback state
 *
 * @return
 */
audio_player_state_t audio_system_app_get_playback_state(void);

/**
 * @brief  Get the audio playback device handle
 *
 * @return
 *       - Playback  codec device handle  On success
 *       - NULL      If not initialized
 */
void *audio_system_app_get_playback_dev(void);

/**
 * @brief  Initialize the video system application
 *
 * @param[in]  config  Video system configuration
 *
 * @return
 *       - ESP_OK  On success
 *       - Other   Error code on failure
 */
esp_err_t video_system_app_init(video_sys_config_t *config);

/**
 * @brief  Get the video render handle
 *
 * @return
 *       - Video  render handle  On success
 *       - NULL   If not initialized
 */
void *video_system_app_get_render_handle(void);

/**
 * @brief  Get the LCD panel handle
 *
 * @return
 *       - LCD   panel handle  On success
 *       - NULL  If not initialized
 */
void *video_system_app_get_panel_handle(void);

/**
 * @brief  Deinitialize the audio system application
 *
 * @return
 *       - ESP_OK  On success
 */
esp_err_t audio_system_app_deinit(void);

/**
 * @brief  Deinitialize the video system application
 *
 * @return
 *       - ESP_OK  On success
 */
esp_err_t video_system_app_deinit(void);

esp_err_t audio_system_app_open(void *play_dev_handle, void *rec_dev_handle, bool mode);
esp_err_t audio_system_app_close(void *play_dev_handle, void *rec_dev_handle);


#ifdef __cplusplus
}
#endif  /* __cplusplus */