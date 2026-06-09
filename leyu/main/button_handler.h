/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _BUTTON_HANDLER_H_
#define _BUTTON_HANDLER_H_

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 初始化按键处理
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t button_handler_init(void);

/**
 * @brief 反初始化按键处理
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t button_handler_deinit(void);

/**
 * @brief 按键测试初始化（用于调试）
 */
void button_test_init(void);

/**
 * @brief 按键扫描测试函数（用于调试）
 */
void button_scan_test(void);

/**
 * @brief 获取当前播放状态
 * @return true 正在播放，false 停止播放
 */
bool brtc_is_playing(void);

/**
 * @brief 增加音量
 */
void brtc_volume_up(void);

/**
 * @brief 减少音量
 */
void brtc_volume_down(void);

/**
 * @brief 使用AT指令拨打电话
 * @param phone_number 要拨打的电话号码
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t at_make_phone_call(const char *phone_number);

/**
 * @brief 使用AT指令挂断电话
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t at_hang_up_phone_call(void);

/**
 * @brief 获取当前电话状态
 * @return true 正在通话中，false 未在通话
 */
bool button_handler_is_in_call(void);

/**
 * @brief 设置电话状态
 * @param in_call true 正在通话中，false 未在通话
 * @note 用于外部模块（如AT响应处理）设置通话状态
 */
void button_handler_set_call_state(bool in_call);

/**
 * @brief 统一挂断清理函数
 * @note 用于主动挂断、对方挂断（NO CARRIER）等所有挂断场景的统一处理
 *       设置GPIO47为低电平，设置电话状态为非通话中
 */
void hang_up_cleanup(void);

#endif /* _BUTTON_HANDLER_H_ */