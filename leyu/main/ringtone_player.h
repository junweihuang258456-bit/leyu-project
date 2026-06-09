/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化铃声播放器
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t ringtone_player_init(void);

/**
 * @brief 反初始化铃声播放器
 */
void ringtone_player_deinit(void);

/**
 * @brief 开始播放铃声
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t ringtone_player_start(void);

/**
 * @brief 停止播放铃声
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t ringtone_player_stop(void);

/**
 * @brief 获取播放状态
 * @return true 正在播放，false 未播放
 */
bool ringtone_player_is_playing(void);

#ifdef __cplusplus
}
#endif