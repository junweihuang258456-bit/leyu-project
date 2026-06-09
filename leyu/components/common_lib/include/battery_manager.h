/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BATTERY_MANAGER_H
#define BATTERY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

/* ADC默认配置 */
#define BATTERY_ADC_UNIT_DEFAULT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL_DEFAULT     ADC_CHANNEL_6     /* GPIO7对应ADC1_CHANNEL_6 */
#define BATTERY_ADC_ATTEN_DEFAULT       ADC_ATTEN_DB_12   /* 0-3.3V量程 */
#define BATTERY_ADC_BITWIDTH_DEFAULT    ADC_BITWIDTH_12   /* 12位分辨率 */

/* 充电检测配置 */
#define CHARGING_DETECT_SAMPLES     3                 /* 检测样本数 */
#define CHARGING_VOLTAGE_RISE_TH    43                /* 充电接入阈值：电压快速上升43mV */
#define CHARGING_VOLTAGE_FALL_TH    36               /* 充电拔出阈值：电压快速下降43mV */
#define CHARGING_DETECT_INTERVAL_MS 100               /* 检测间隔 */

/* 电压转换表条目 */
typedef struct {
    uint16_t adc_value;       /* ADC原始值 */
    uint16_t voltage_mv;      /* 对应电压值(mV) */
    uint8_t  battery_level;   /* 电量百分比 0-100 */
} battery_voltage_table_t;

/* 电池管理器句柄 */
typedef struct battery_manager_s *battery_manager_handle_t;

/**
 * @brief 电池管理器配置结构体
 */
typedef struct {
    /* 外部ADC句柄，如果为NULL则自动创建 */
    adc_oneshot_unit_handle_t external_adc_handle;

    /* ADC配置（仅在external_adc_handle为NULL时使用） */
    adc_unit_t adc_unit;              /* ADC单元，如 ADC_UNIT_1 */
    adc_channel_t adc_channel;        /* ADC通道，如 ADC_CHANNEL_6 */
    adc_atten_t adc_atten;            /* ADC衰减，如 ADC_ATTEN_DB_12 */
    adc_bitwidth_t adc_bitwidth;      /* ADC位宽，如 ADC_BITWIDTH_12 */

    /* 充电状态下的电压转换表 */
    const battery_voltage_table_t *charging_table;
    uint8_t charging_table_size;

    /* 未充电状态下的电压转换表 */
    const battery_voltage_table_t *discharging_table;
    uint8_t discharging_table_size;

    /* 分压电阻比例 (实际电压 = ADC电压 * voltage_divider_ratio) */
    float voltage_divider_ratio;

    /* 充电检测使能 */
    bool enable_charging_detect;
} battery_manager_config_t;

/**
 * @brief 默认电池管理器配置
 */
#define BATTERY_MANAGER_DEFAULT_CONFIG() { \
    .external_adc_handle = NULL, \
    .adc_unit = BATTERY_ADC_UNIT_DEFAULT, \
    .adc_channel = BATTERY_ADC_CHANNEL_DEFAULT, \
    .adc_atten = BATTERY_ADC_ATTEN_DEFAULT, \
    .adc_bitwidth = BATTERY_ADC_BITWIDTH_DEFAULT, \
    .charging_table = NULL, \
    .charging_table_size = 0, \
    .discharging_table = NULL, \
    .discharging_table_size = 0, \
    .voltage_divider_ratio = 2.0f, \
    .enable_charging_detect = true, \
}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池管理器
 * @param config 电池管理器配置
 * @return 电池管理器句柄，失败返回NULL
 */
battery_manager_handle_t battery_manager_init(const battery_manager_config_t *config);

/**
 * @brief 反初始化电池管理器
 * @param handle 电池管理器句柄
 */
void battery_manager_deinit(battery_manager_handle_t handle);

/**
 * @brief 更新电池状态（需要在任务中定期调用）
 * @param handle 电池管理器句柄
 * @return ESP_OK成功，其他失败
 */
esp_err_t battery_manager_update(battery_manager_handle_t handle);

/**
 * @brief 获取转换后的电压值（经过分压电阻计算后的实际电池电压）
 * @param handle 电池管理器句柄
 * @return 电压值(mV)，失败返回0
 */
uint32_t battery_manager_get_voltage(battery_manager_handle_t handle);

/**
 * @brief 获取ADC原始值
 * @param handle 电池管理器句柄
 * @return ADC原始值，失败返回0
 */
uint32_t battery_manager_get_adc_value(battery_manager_handle_t handle);

/**
 * @brief 获取电池电量百分比
 * @param handle 电池管理器句柄
 * @return 电量百分比0-100，失败返回0
 */
uint8_t battery_manager_get_battery_level(battery_manager_handle_t handle);

/**
 * @brief 获取是否处于充电中
 * @param handle 电池管理器句柄
 * @return true正在充电，false未充电
 */
bool battery_manager_is_charging(battery_manager_handle_t handle);

/**
 * @brief 设置充电状态（手动设置，当自动检测不满足需求时使用）
 * @param handle 电池管理器句柄
 * @param charging true正在充电，false未充电
 */
void battery_manager_set_charging_state(battery_manager_handle_t handle, bool charging);

/**
 * @brief 获取ADC采样电压（分压前的电压）
 * @param handle 电池管理器句柄
 * @return ADC采样电压(mV)，失败返回0
 */
uint32_t battery_manager_get_adc_voltage(battery_manager_handle_t handle);

/**
 * @brief 获取ADC oneshot句柄，用于其他模块复用ADC
 * @param handle 电池管理器句柄
 * @return ADC oneshot句柄，失败返回NULL
 * @note 返回的句柄由电池管理器管理，请勿自行释放
 */
adc_oneshot_unit_handle_t battery_manager_get_adc_handle(battery_manager_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_MANAGER_H */
