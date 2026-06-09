/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated device configuration file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_board_device.h"
#include "dev_audio_codec.h"
// 移除SD卡相关头文件
// #include "dev_fatfs_sdcard.h"
// #include "driver/sdmmc_host.h"
// #include "driver/sdmmc_types.h"

// Device configuration structures
const static dev_audio_codec_config_t esp_bmgr_audio_dac_cfg = {
    .name = "audio_dac",
    .chip = "es8311",
    .type = "audio_codec",
    .adc_enabled = true,
    .adc_max_channel = 1,
    .adc_channel_mask = 0x1,
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 1,
    .dac_channel_mask = 0x1,
    .dac_init_gain = 0,
    .pa_cfg = {
            .name = "gpio_pa_control",
            .port = 46,
            .active_level = 1,
            .gain = 6.0,
        },
    .i2c_cfg = {
            .name = "i2c_master",
            .port = 0,
            .address = 48,
            .frequency = 400000,
        },
    .i2s_cfg = {
            .name = "i2s_audio_out",
            .port = 0,
        },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = false,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

const static dev_audio_codec_config_t esp_bmgr_audio_adc_cfg = {
    .name = "audio_adc",
    .chip = "es8311",
    .type = "audio_codec",
    .adc_enabled = true,
    .adc_max_channel = 1,
    .adc_channel_mask = 0x1,
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 1,
    .dac_channel_mask = 0x1,
    .dac_init_gain = 0,
    .pa_cfg = {
            .name = "gpio_pa_control",
            .port = 46,
            .active_level = 1,
            .gain = 6.0,
        },
    .i2c_cfg = {
            .name = "i2c_master",
            .port = 0,
            .address = 48,
            .frequency = 400000,
        },
    .i2s_cfg = {
            .name = "i2s_audio_in",
            .port = 0,
        },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = false,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

// 移除SD卡配置结构体
// const static dev_fatfs_sdcard_config_t esp_bmgr_fs_sdcard_cfg = {
//     .name = "fs_sdcard",
//     .mount_point = "/sdcard",
//     .vfs_config = {
//             .format_if_mount_failed = false,
//             .max_files = 5,
//             .allocation_unit_size = 16384,
//         },
//     .frequency = SDMMC_FREQ_HIGHSPEED,
//     .slot = SDMMC_HOST_SLOT_1,
//     .bus_width = 1,
//     .slot_flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
//     .pins = {
//             .clk = 6,
//             .cmd = 7,
//             .d0 = 4,
//             .d1 = -1,
//             .d2 = -1,
//             .d3 = -1,
//             .d4 = -1,
//             .d5 = -1,
//             .d6 = -1,
//             .d7 = -1,
//             .cd = -1,
//             .wp = -1,
//         },
// };

// Device descriptor array
const esp_board_device_desc_t g_esp_board_devices[] = {
    {
        .next = &g_esp_board_devices[1],
        .name = "audio_dac",
        .type = "audio_codec",
        .cfg = &esp_bmgr_audio_dac_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_dac_cfg),
    },
    {
        .next = NULL,  // 移除SD卡设备后，这里改为NULL
        .name = "audio_adc",
        .type = "audio_codec",
        .cfg = &esp_bmgr_audio_adc_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_adc_cfg),
    },
    // 移除SD卡设备描述符
    // {
    //     .next = NULL,
    //     .name = "fs_sdcard",
    //     .type = "fatfs_sdcard",
    //     .cfg = &esp_bmgr_fs_sdcard_cfg,
    //     .cfg_size = sizeof(esp_bmgr_fs_sdcard_cfg),
    // },
};