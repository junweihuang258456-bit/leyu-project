/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "esp_board_manager_includes.h"
#include "dev_audio_codec.h"
#include "esp_board_manager_adapter.h"
#include "esp_lcd_types.h"
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT
#include "esp_lcd_touch.h"
#include "dev_lcd_touch_i2c.h"
#endif  /* CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT */

#define DEFAULT_PLAY_VOLUME      30
#define DEFAULT_REC_GAIN         32.0
#define DEFAULT_REC_CHANNEL_GAIN 32.0
#define REC_CHANNEL_MASK         BIT(2)

static const char *TAG = "ESP_BOARD_MANAGER_ADAPTER";

typedef struct {
    void  *lcd_handles;
#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    dev_audio_codec_handles_t  *play_dev_handles;
    dev_audio_codec_handles_t  *rec_dev_handles;
#endif  /* CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT */
    esp_board_manager_adapter_config_t  config;
} esp_board_manager_adapter_basic_t;

static esp_board_manager_adapter_basic_t basic_info;

static esp_err_t calculate_adc_audio_info(esp_board_manager_adapter_info_t *info)
{
    dev_audio_codec_config_t *adc_cfg = NULL;
    int ret = esp_board_manager_get_device_config("audio_adc", (void **)&adc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get audio_adc config: %s", esp_err_to_name(ret));
        return ret;
    }
    char mask_bin[9];
    for (int i = 7; i >= 0; --i) {
        mask_bin[7 - i] = ((adc_cfg->adc_channel_mask >> i) & 1) ? '1' : '0';
    }
    mask_bin[8] = '\0';
    ESP_LOGI(TAG, "adc_cfg->adc_channel_mask: 0b%s (hex: 0x%" PRIx32 ")", mask_bin, adc_cfg->adc_channel_mask);
    if (adc_cfg->adc_channel_mask == 0b0111) {
        memcpy(info->mic_layout, "RMNM", 4);
        info->sample_rate = 16000;
        info->sample_bits = 32;
        info->channels = 2;
    } else if (adc_cfg->adc_channel_mask == 0b0011) {
        memcpy(info->mic_layout, "MR", 2);
        info->sample_rate = 16000;
        info->sample_bits = 16;
        info->channels = 2;
    } else if (adc_cfg->adc_channel_mask == 0b0001) {
        memcpy(info->mic_layout, "M", 1);
        info->sample_rate = 16000;
        info->sample_bits = 16;
        info->channels = 1;
    } else {
        ESP_LOGE(TAG, "Unsupported adc channel mask: %lx", adc_cfg->adc_channel_mask);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t esp_board_manager_adapter_init(const esp_board_manager_adapter_config_t *config, esp_board_manager_adapter_info_t *info)
{
    esp_log_level_set("BOARD_DEVICE", ESP_LOG_WARN);
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Initializing board manager adapter");
    if (info == NULL || config == NULL) {
        ESP_LOGE(TAG, "Invalid parameter: info or config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(esp_board_manager_adapter_info_t));
    basic_info.config = *config;

#if CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT
        esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_GPIO_EXPANDER);
#endif  /* CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT */

    if (config->enable_lcd) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize LCD: %s", esp_err_to_name(ret));
        }
    }
    if (config->enable_audio) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_DAC);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize audio dac: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_ADC);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize audio adc: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    if (config->enable_camera) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize camera: %s", esp_err_to_name(ret));
        }
    }
    if (config->enable_sdcard) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_FS_SDCARD);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize sdcard: %s", esp_err_to_name(ret));
        }
    }
#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (config->enable_lcd_backlight) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize lcd backlight: %s", esp_err_to_name(ret));
        }
    }
#endif /* CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT */
    if (config->enable_touch) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize touch: %s", esp_err_to_name(ret));
            // return ret;
        }
    }
#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    ESP_LOGI(TAG, "Initialize audio codec");
    if (config->enable_audio) {
        ret = esp_board_manager_get_device_handle("audio_dac", (void **)&basic_info.play_dev_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get audio_dac handle: %s", esp_err_to_name(ret));
            return ret;
        }
        info->play_dev = basic_info.play_dev_handles ? basic_info.play_dev_handles->codec_dev : NULL;
        if (info->play_dev) {
            ret = esp_codec_dev_set_out_vol(info->play_dev, DEFAULT_PLAY_VOLUME);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set playback volume: %s", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGI(TAG, "Playback device configured with volume: %d", DEFAULT_PLAY_VOLUME);
        }

        ret = esp_board_manager_get_device_handle("audio_adc", (void **)&basic_info.rec_dev_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get audio_adc handle: %s", esp_err_to_name(ret));
            return ret;
        }
        info->rec_dev = basic_info.rec_dev_handles ? basic_info.rec_dev_handles->codec_dev : NULL;
        if (info->rec_dev) {
            ret = esp_codec_dev_set_in_gain(info->rec_dev, DEFAULT_REC_GAIN);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set recording gain: %s", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGI(TAG, "Recording device configured with gain: %.1f", DEFAULT_REC_GAIN);
        }
        ESP_LOGI(TAG, "Audio codec initialized successfully");
        ret = calculate_adc_audio_info(info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to calculate ADC audio info: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Mic layout: %.*s", 8, (char *)info->mic_layout);
        ESP_LOGI(TAG, "Sample rate: %d Hz", info->sample_rate);
        ESP_LOGI(TAG, "Sample bits: %d", info->sample_bits);
        ESP_LOGI(TAG, "Channels: %d", info->channels);
    } else {
        info->rec_dev = NULL;
        info->play_dev = NULL;
    }
#else
    info->play_dev = NULL;
    info->rec_dev = NULL;
#endif  /* CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT */

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    if (config->enable_lcd) {
        void *lcd_handle = NULL;
        ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
        if (ret == ESP_OK && lcd_handle) {
            dev_display_lcd_handles_t *__lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
            info->lcd_panel = __lcd_handles->panel_handle;
            basic_info.lcd_handles = __lcd_handles;
            ESP_LOGI(TAG, "LCD panel handle: %p", info->lcd_panel);
        } else {
            ESP_LOGW(TAG, "LCD device not available: %s", esp_err_to_name(ret));
            info->lcd_panel = NULL;
        }
    } else {
        info->lcd_panel = NULL;
    }
#else
    info->lcd_panel = NULL;
#endif  /* CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT */

#if CONFIG_ESP_BOARD_DEV_FATFS_SDCARD_SUPPORT
    if (config->enable_sdcard) {
        ret = esp_board_manager_get_device_handle("fs_sdcard", &info->sdcard_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD card device not available: %s", esp_err_to_name(ret));
            info->sdcard_handle = NULL;
        } else {
            ESP_LOGI(TAG, "SD card handle: %p", info->sdcard_handle);
        }
    } else {
        info->sdcard_handle = NULL;
    }
#else
    info->sdcard_handle = NULL;
#endif  /* CONFIG_ESP_BOARD_DEV_FATFS_SDCARD_SUPPORT */

    ESP_LOGI(TAG, "Board: %s", g_esp_board_info.name);
    ESP_LOGI(TAG, "BSP Manager initialization completed successfully");

#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (config->enable_lcd_backlight) {
        esp_board_manager_adapter_set_lcd_backlight(100);
    }
#endif  /* CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT */

#ifdef CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
    if (config->enable_camera) {
        dev_camera_handle_t *camera_handle = NULL;
        ret = esp_board_device_get_handle(ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&camera_handle);
        memset(info->camera_dev_path, 0, sizeof(info->camera_dev_path));
        strncpy(info->camera_dev_path, camera_handle->dev_path, sizeof(info->camera_dev_path) - 1);
        ESP_LOGI(TAG, "Camera device path: %s", info->camera_dev_path);
    }
#endif  /* CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT */

    return ESP_OK;
}

esp_err_t esp_board_manager_adapter_deinit(void)
{
    esp_err_t ret = esp_board_manager_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize board manager: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t esp_board_manager_adapter_set_lcd_backlight(int brightness_percent)
{
#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (!basic_info.config.enable_lcd_backlight) {
        ESP_LOGE(TAG, "LCD backlight control not enabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%,", brightness_percent);
    static periph_ledc_handle_t *ledc_handle = NULL;
    if (ledc_handle == NULL) {
        ESP_BOARD_RETURN_ON_ERROR(esp_board_manager_get_device_handle("lcd_brightness", (void **)&ledc_handle), TAG, "Get LEDC control device handle failed");
    }
    dev_ledc_ctrl_config_t *dev_ledc_cfg = NULL;
    esp_err_t config_ret = esp_board_manager_get_device_config("lcd_brightness", (void *)&dev_ledc_cfg);
    if (config_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LEDC peripheral config '%s': %s", "lcd_brightness", esp_err_to_name(config_ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "dev_ledc_cfg.ledc_name: %s, name: %s, type: %s", dev_ledc_cfg->ledc_name, dev_ledc_cfg->name, dev_ledc_cfg->type);
    periph_ledc_config_t *ledc_config = NULL;
    esp_board_manager_get_periph_config(dev_ledc_cfg->ledc_name, (void **)&ledc_config);
    uint32_t duty = (brightness_percent * ((1 << (uint32_t)ledc_config->duty_resolution) - 1)) / 100;
    ESP_LOGI(TAG, "duty_cycle: %" PRIu32 ", speed_mode: %d, channel: %d, duty_resolution: %d", duty, ledc_handle->speed_mode, ledc_handle->channel, ledc_config->duty_resolution);
    ESP_BOARD_RETURN_ON_ERROR(ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty), TAG, "LEDC set duty failed");
    ESP_BOARD_RETURN_ON_ERROR(ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel), TAG, "LEDC update duty failed");
    return ESP_OK;
#else
    ESP_LOGE(TAG, "LCD backlight control not supported");
    return ESP_ERR_NOT_SUPPORTED;
#endif  /* CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT */
}

esp_err_t esp_board_manager_adapter_get_lcd_resolution(int *width, int *height)
{
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    if (!basic_info.config.enable_lcd) {
        ESP_LOGE(TAG, "LCD not enabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (width == NULL || height == NULL) {
        ESP_LOGE(TAG, "Invalid parameter: width or height is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    dev_display_lcd_config_t *lcd_cfg = NULL;
    esp_board_manager_get_device_config("display_lcd", (void **)&lcd_cfg);
    *width = lcd_cfg->lcd_width;
    *height = lcd_cfg->lcd_height;
    ESP_LOGD(TAG, "LCD resolution: %d x %d", *width, *height);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "LCD not supported");
    return ESP_ERR_NOT_SUPPORTED;
#endif  /* CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT */
}
