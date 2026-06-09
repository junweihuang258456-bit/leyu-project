/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "battery_manager.h"
#include "battery_module.h"
#include "brtc_app.h"
#include "audio_processor.h"

static const char *TAG = "BATTERY_MODULE";

/* 关机请求标志，0：无请求，1：有关机请求（定义在brtc_app.c中） */
extern int g_shutdown_request;

/* 低电量关机阈值（单位：mV） */
#define BATTERY_SHUTDOWN_THRESHOLD  3500

/* 低电量关机标志，防止重复触发 */
static bool s_low_battery_shutdown_triggered = false;

/* 电池监控任务静态内存 */
#define BATTERY_TASK_STACK_SIZE  4096

/* ADC句柄，供外部复用 */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static battery_manager_handle_t s_battery_mgr = NULL;

/*
 * 充电状态下的电压-电量转换表（ADC值递增顺序）
 * 实际测量数据（充电中）：
 *   ADC=965 -> 3661mV
 *   ADC=967 -> 3659mV
 *   ADC=968 -> 3673mV
 *   ADC=978 -> 3706mV
 *   ADC=981 -> 3724mV
 *   ADC=1042 -> 3935mV
 *   ADC=1043 -> 3939mV
 *   ADC=1047 -> 3949mV
 *   ADC=1110 -> 4196mV
 *   ADC=1125 -> 4248mV
 *   ADC=1126 -> 4254mV
 */
static const battery_voltage_table_t charging_table[] = {
    /* ADC值, 电压(mV), 电量(%)
     * 注意：充电时电压虚高，相同电量对应的电压比放电时高
     * 但电量百分比应该与放电表保持一致（基于实际电池电量）
     */
    {900, 3100, 0},     /* 空电 3.1V */
    {920, 3200, 5},
    {940, 3300, 10},
    {950, 3400, 20},
    {960, 3500, 30},
    {965, 3661, 40},    /* 实际测量，充电电压虚高约100-150mV */
    {967, 3659, 42},    /* 实际测量 */
    {968, 3673, 44},    /* 实际测量 */
    {978, 3706, 47},    /* 实际测量 */
    {981, 3724, 50},    /* 实际测量 */
    {1042, 3935, 64},   /* 实际测量，对应放电表3836mV的63-64% */
    {1043, 3939, 65},   /* 实际测量 */
    {1047, 3949, 66},   /* 实际测量 */
    {1080, 4050, 80},   /* 修正：100%=4196mV，按比例推算 */
    {1100, 4100, 90},   /* 修正 */
    {1110, 4196, 98},   /* 实际测量，充电中 */
    {1125, 4248, 99},   /* 实际测量，充电中 */
    {1126, 4252, 100},  /* 实际测量，充电中，满电 */
};

/*
 * 放电状态下的电压-电量转换表（ADC值递增顺序）
 * 实际测量数据（未充电）：
 *   ADC=933 -> 3546mV
 *   ADC=935 -> 3552mV
 *   ADC=937 -> 3562mV
 *   ADC=949 -> 3602mV
 *   ADC=1011 -> 3836mV
 *   ADC=1012 -> 3838mV
 *   ADC=1015 -> 3851mV
 *   ADC=1102 -> 4164mV
 *   ADC=1109 -> 4192mV
 */
static const battery_voltage_table_t discharging_table[] = {
    /* ADC值, 电压(mV), 电量(%) */
    {880, 3100, 0},     /* 空电 3.1V */
    {900, 3200, 5},
    {920, 3300, 10},
    {933, 3546, 25},    /* 实际测量 */
    {935, 3552, 28},    /* 实际测量 */
    {937, 3562, 30},    /* 实际测量 */
    {949, 3602, 35},    /* 实际测量 */
    {960, 3640, 40},
    {970, 3670, 45},
    {980, 3700, 50},
    {1011, 3836, 63},   /* 实际测量 */
    {1012, 3838, 64},   /* 实际测量 */
    {1015, 3851, 65},   /* 实际测量 */
    {1040, 3900, 75},   /* 修正：100%=4192mV，按比例推算 */
    {1070, 4000, 85},   /* 修正 */
    {1090, 4150, 93},   /* 修正 */
    {1102, 4164, 96},   /* 实际测量，未充电 */
    {1106, 4180, 98},   /* 修正：斜率约4mV/ADC */
    {1109, 4192, 100},  /* 实际测量，未充电，满电 */
};

void battery_monitor_task(void *pvParameters)
{
    adc_oneshot_unit_handle_t adc_handle = (adc_oneshot_unit_handle_t)pvParameters;

    /* 配置电池管理器 */
    battery_manager_config_t config = {
        .external_adc_handle = adc_handle, /* 使用外部传入的ADC句柄 */
        .adc_unit = ADC_UNIT_1,           /* ADC1 */
        .adc_channel = ADC_CHANNEL_6,     /* GPIO7对应ADC1_CHANNEL_6 */
        .adc_atten = ADC_ATTEN_DB_12,     /* 0-3.3V量程 */
        .adc_bitwidth = ADC_BITWIDTH_12,  /* 12位分辨率 */
        .charging_table = charging_table,
        .charging_table_size = sizeof(charging_table) / sizeof(charging_table[0]),
        .discharging_table = discharging_table,
        .discharging_table_size = sizeof(discharging_table) / sizeof(discharging_table[0]),
        .voltage_divider_ratio = 65.0f,  /* 分压比例：4200mV/115ADC≈36.5，根据实际调整 */
        .enable_charging_detect = true,
    };

    /* 初始化电池管理器 */
    s_battery_mgr = battery_manager_init(&config);
    if (s_battery_mgr == NULL) {
        ESP_LOGE(TAG, "电池管理器初始化失败");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "电池监控任务启动");

    while (1) {
        /* 更新电池状态 */
        battery_manager_update(s_battery_mgr);

        /* 获取并打印电池信息 */
        uint32_t adc_value = battery_manager_get_adc_value(s_battery_mgr);
        uint32_t adc_voltage = battery_manager_get_adc_voltage(s_battery_mgr);
        uint32_t battery_voltage = battery_manager_get_voltage(s_battery_mgr);
        uint8_t battery_level = battery_manager_get_battery_level(s_battery_mgr);
        bool is_charging = battery_manager_is_charging(s_battery_mgr);

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "ADC原始值: %lu", adc_value);
        ESP_LOGI(TAG, "ADC电压: %lu mV", adc_voltage);
        ESP_LOGI(TAG, "电池电压: %lu mV", battery_voltage);
        ESP_LOGI(TAG, "电池电量: %d%%", battery_level);
        ESP_LOGI(TAG, "充电状态: %s", is_charging ? "充电中" : "未充电");
        ESP_LOGI(TAG, "========================================");

        /* 检查低电量关机条件 */
        if (!is_charging && battery_voltage <= BATTERY_SHUTDOWN_THRESHOLD && !s_low_battery_shutdown_triggered) {
            /* 设置关机标志，由系统监控任务处理实际关机操作 */
            s_low_battery_shutdown_triggered = true;
            g_shutdown_request = 1;
            ESP_LOGI(TAG, "关机标志已设置");
            ESP_LOGI(TAG, "电池电压过低(%lu mV)，触发自动关机", battery_voltage);

            /* 使用Mp3播报电量不足提示 */

    audio_prompt_play("file:///spiffs/low_battery_shutdown.mp3");
                ESP_LOGI(TAG, "播报: 电量不足，自动关机");
      

            

        }

        /* 每6秒更新一次 */
        vTaskDelay(pdMS_TO_TICKS(6000));
    }

    /* 反初始化（实际上不会执行到这里） */
    battery_manager_deinit(s_battery_mgr);
    vTaskDelete(NULL);
}

void battery_module_init(void)
{
    ESP_LOGI(TAG, "电池管理示例程序启动");

    /* 创建ADC句柄，在创建任务之前完成 */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC单元创建失败: %d", ret);
        return;
    }

    /* 配置ADC通道 */
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_6, &chan_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC通道配置失败: %d", ret);
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return;
    }

    ESP_LOGI(TAG, "ADC句柄创建成功");

    /* 创建电池监控任务，使用PSRAM */
    xTaskCreatePinnedToCoreWithCaps(battery_monitor_task, "battery_task", BATTERY_TASK_STACK_SIZE, (void *)s_adc_handle, 1, NULL, 1, MALLOC_CAP_SPIRAM);
}

adc_oneshot_unit_handle_t battery_module_get_adc_handle(void)
{
    return s_adc_handle;
}

battery_manager_handle_t battery_module_get_manager_handle(void)
{
    return s_battery_mgr;
}
