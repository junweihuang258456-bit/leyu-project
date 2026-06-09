/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_adc/adc_oneshot.h"
#include "battery_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池模块
 *
 * 创建ADC句柄和电池监控任务，开始监测电池电压和电量
 */
void battery_module_init(void);

/**
 * @brief 获取ADC oneshot句柄，供其他模块复用
 * @return ADC oneshot句柄，未初始化返回NULL
 * @note 返回的句柄由电池模块管理，请勿自行释放
 */
adc_oneshot_unit_handle_t battery_module_get_adc_handle(void);

/**
 * @brief 获取电池管理器句柄
 * @return 电池管理器句柄，未初始化返回NULL
 */
battery_manager_handle_t battery_module_get_manager_handle(void);

#ifdef __cplusplus
}
#endif
