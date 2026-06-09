/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "math.h"
#include "brtc_app.h"

static const char *TAG = "RINGTONE_PLAYER";

// I2S配置
#define I2S_NUM         I2S_NUM_0
#define I2S_SAMPLE_RATE 16000
#define I2S_CHANNEL_NUM 2
#define I2S_DATA_BIT_WIDTH I2S_DATA_BIT_WIDTH_16BIT

// 音频缓冲区配置
#define AUDIO_BUFFER_SIZE 1024

// 播放状态
static bool g_is_playing = false;
static TaskHandle_t g_play_task_handle = NULL;
static SemaphoreHandle_t g_play_mutex = NULL;
static bool g_is_initialized = false;
static i2s_chan_handle_t g_i2s_tx_chan = NULL;

// 前向声明
esp_err_t ringtone_player_stop(void);

// 简单的音调生成器
static void generate_tone(int16_t *buffer, size_t samples, float frequency, float amplitude)
{
    static float phase = 0.0f;
    const float sample_rate = I2S_SAMPLE_RATE;
    const float phase_increment = frequency / sample_rate;
    
    for (size_t i = 0; i < samples; i += 2) { // 立体声
        float sample = amplitude * sinf(2.0f * M_PI * phase);
        int16_t pcm_sample = (int16_t)(sample * 32767.0f);
        
        // 左声道
        buffer[i] = pcm_sample;
        // 右声道
        buffer[i + 1] = pcm_sample;
        
        phase += phase_increment;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

// 播放任务
static void ringtone_play_task(void *pvParameters)
{
    ESP_LOGI(TAG, "铃声播放任务开始");
    
    // 分配音频缓冲区
    int16_t *pcm_buffer = malloc(AUDIO_BUFFER_SIZE * sizeof(int16_t));
    
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        g_play_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // 播放参数
    const float base_frequency = 800.0f; // 更高的频率，更容易听到
    const float amplitude = 0.7f;
    const int tone_duration_ms = 500; // 每个音调持续500ms
    const int silence_duration_ms = 100; // 静音间隔100ms
    
    // 播放循环
    while (g_is_playing) {
        // 播放音调
        ESP_LOGD(TAG, "播放音调: %.1f Hz", base_frequency);
        
        int total_samples = (I2S_SAMPLE_RATE * tone_duration_ms) / 1000;
        int samples_played = 0;
        
        while (samples_played < total_samples && g_is_playing) {
            int samples_to_play = AUDIO_BUFFER_SIZE / 2; // 立体声，所以除以2
            if (samples_played + samples_to_play > total_samples) {
                samples_to_play = total_samples - samples_played;
            }
            
            // 生成音频数据
            generate_tone(pcm_buffer, samples_to_play * 2, base_frequency, amplitude);
            
            // 写入I2S - 使用新的I2S驱动
            size_t bytes_written = 0;
            esp_err_t ret = i2s_channel_write(g_i2s_tx_chan, pcm_buffer, samples_to_play * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(50));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S写入失败: %s", esp_err_to_name(ret));
                // 如果I2S通道无效，尝试重新初始化
                if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_INVALID_ARG) {
                    ESP_LOGW(TAG, "I2S通道可能已关闭，尝试重新初始化");
                    // 这里可以添加重新初始化逻辑，但为了避免复杂化，先停止播放
                }
                break;
            }
            
            samples_played += samples_to_play;
            
            // 让出CPU，避免阻塞其他任务
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        // 静音间隔
        if (g_is_playing) {
            ESP_LOGD(TAG, "静音间隔: %d ms", silence_duration_ms);
            
            // 生成静音数据
            memset(pcm_buffer, 0, AUDIO_BUFFER_SIZE * sizeof(int16_t));
            
            int silence_samples = (I2S_SAMPLE_RATE * silence_duration_ms) / 1000;
            int samples_written = 0;
            
            while (samples_written < silence_samples && g_is_playing) {
                int samples_to_write = AUDIO_BUFFER_SIZE / 2;
                if (samples_written + samples_to_write > silence_samples) {
                    samples_to_write = silence_samples - samples_written;
                }
                
                size_t silence_size = samples_to_write * 2 * sizeof(int16_t); // 立体声
                
                // 写入静音数据
                size_t bytes_written = 0;
                esp_err_t ret = i2s_channel_write(g_i2s_tx_chan, pcm_buffer, silence_size, &bytes_written, pdMS_TO_TICKS(50));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "I2S写入静音数据失败: %s", esp_err_to_name(ret));
                    break;
                }
                
                samples_written += samples_to_write;
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
    
    // 播放结束，写入静音数据清理I2S缓冲区
    memset(pcm_buffer, 0, AUDIO_BUFFER_SIZE * sizeof(int16_t));
    i2s_channel_write(g_i2s_tx_chan, pcm_buffer, AUDIO_BUFFER_SIZE * sizeof(int16_t), NULL, pdMS_TO_TICKS(100));
    
    // 清理资源
    free(pcm_buffer);
    
    ESP_LOGI(TAG, "铃声播放任务结束");
    g_play_task_handle = NULL;
    vTaskDelete(NULL);
}

// 初始化I2S通道
static esp_err_t i2s_init_ringtone(void)
{
    ESP_LOGI(TAG, "初始化I2S通道");
    
    // 首先检查音频系统是否已初始化，带重试机制
    esp_codec_dev_handle_t play_dev = NULL;
    int retry_count = 0;
    const int max_retries = 10;
    
    while (retry_count < max_retries) {
        play_dev = get_play_dev_handle();
        if (play_dev != NULL) {
            break;
        }
        
        ESP_LOGW(TAG, "音频系统未就绪，等待500ms后重试 (%d/%d)", retry_count + 1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(500));
        retry_count++;
    }
    
    if (play_dev == NULL) {
        ESP_LOGE(TAG, "音频系统初始化超时，无法初始化I2S通道");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "音频系统已就绪，继续初始化I2S");
    
    esp_err_t ret = ESP_OK;
    
    // I2S通道配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    // 分配新的I2S通道
    ret = i2s_new_channel(&chan_cfg, &g_i2s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建I2S通道失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // I2S标准模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 4,  // 需要根据实际硬件修改
            .ws = 5,    // 需要根据实际硬件修改
            .dout = 18, // 需要根据实际硬件修改
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // 初始化I2S通道
    ret = i2s_channel_init_std_mode(g_i2s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2S通道失败: %s", esp_err_to_name(ret));
        i2s_del_channel(g_i2s_tx_chan);
        g_i2s_tx_chan = NULL;
        return ret;
    }
    
    // 启用I2S通道
    ret = i2s_channel_enable(g_i2s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用I2S通道失败: %s", esp_err_to_name(ret));
        i2s_channel_disable(g_i2s_tx_chan);
        i2s_del_channel(g_i2s_tx_chan);
        g_i2s_tx_chan = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "I2S通道初始化成功");
    return ESP_OK;
}

// 反初始化I2S通道
static void i2s_deinit_ringtone(void)
{
    ESP_LOGI(TAG, "反初始化I2S通道");
    
    if (g_i2s_tx_chan != NULL) {
        // 禁用通道
        i2s_channel_disable(g_i2s_tx_chan);
        
        // 删除通道
        i2s_del_channel(g_i2s_tx_chan);
        g_i2s_tx_chan = NULL;
        
        ESP_LOGI(TAG, "I2S通道已删除");
    }
}

// 初始化铃声播放器
esp_err_t ringtone_player_init(void)
{
    ESP_LOGI(TAG, "初始化铃声播放器");
    
    if (g_is_initialized) {
        ESP_LOGW(TAG, "铃声播放器已经初始化");
        return ESP_OK;
    }
    
    // 创建互斥锁
    if (g_play_mutex == NULL) {
        g_play_mutex = xSemaphoreCreateMutex();
        if (g_play_mutex == NULL) {
            ESP_LOGE(TAG, "创建互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // 注意：I2S初始化将在第一次播放时进行，避免与音频系统冲突
    // 这样可以确保在音频系统初始化完成后再初始化I2S
    ESP_LOGI(TAG, "铃声播放器基本初始化完成，I2S将在首次播放时初始化");
    
    g_is_initialized = true;
    return ESP_OK;
}

// 反初始化铃声播放器
void ringtone_player_deinit(void)
{
    ESP_LOGI(TAG, "反初始化铃声播放器");
    
    if (!g_is_initialized) {
        ESP_LOGW(TAG, "铃声播放器未初始化");
        return;
    }
    
    // 停止播放
    ringtone_player_stop();
    
    // 反初始化I2S通道
    i2s_deinit_ringtone();
    
    // 删除互斥锁
    if (g_play_mutex != NULL) {
        vSemaphoreDelete(g_play_mutex);
        g_play_mutex = NULL;
    }
    
    g_is_initialized = false;
    ESP_LOGI(TAG, "铃声播放器反初始化完成");
}

// 开始播放铃声
esp_err_t ringtone_player_start(void)
{
    ESP_LOGI(TAG, "开始播放铃声");
    
    if (!g_is_initialized) {
        ESP_LOGE(TAG, "铃声播放器未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 获取互斥锁
    if (xSemaphoreTake(g_play_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "获取互斥锁失败");
        return ESP_ERR_TIMEOUT;
    }
    
    // 检查是否已经在播放
    if (g_is_playing) {
        ESP_LOGW(TAG, "铃声已经在播放中");
        xSemaphoreGive(g_play_mutex);
        return ESP_OK;
    }
    
    // 如果I2S通道未初始化，先初始化它
    if (g_i2s_tx_chan == NULL) {
        ESP_LOGI(TAG, "首次播放，初始化I2S通道");
        esp_err_t ret = i2s_init_ringtone();
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "音频系统未就绪，延迟2秒后重试");
                xSemaphoreGive(g_play_mutex);
                vTaskDelay(pdMS_TO_TICKS(2000));
                
                // 重试一次
                if (xSemaphoreTake(g_play_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                    return ESP_ERR_TIMEOUT;
                }
                
                ret = i2s_init_ringtone();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "重试初始化I2S通道失败: %s", esp_err_to_name(ret));
                    xSemaphoreGive(g_play_mutex);
                    return ret;
                }
            } else {
                ESP_LOGE(TAG, "初始化I2S通道失败: %s", esp_err_to_name(ret));
                xSemaphoreGive(g_play_mutex);
                return ret;
            }
        }
    }
    
    // 设置播放标志
    g_is_playing = true;
    
    // 创建播放任务
    // 使用PSRAM创建铃声播放任务
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(ringtone_play_task, "ringtone_task", 4096, NULL, 5, &g_play_task_handle, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建播放任务失败");
        g_is_playing = false;
        xSemaphoreGive(g_play_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    xSemaphoreGive(g_play_mutex);
    ESP_LOGI(TAG, "开始播放铃声成功");
    return ESP_OK;
}

// 停止播放铃声
esp_err_t ringtone_player_stop(void)
{
    ESP_LOGI(TAG, "停止播放铃声");
    
    if (!g_is_initialized) {
        ESP_LOGE(TAG, "铃声播放器未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 获取互斥锁
    if (xSemaphoreTake(g_play_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "获取互斥锁失败");
        return ESP_ERR_TIMEOUT;
    }
    
    // 检查是否正在播放
    if (!g_is_playing) {
        ESP_LOGW(TAG, "铃声未在播放");
        xSemaphoreGive(g_play_mutex);
        return ESP_OK;
    }
    
    // 设置停止标志
    g_is_playing = false;
    
    // 等待任务结束
    if (g_play_task_handle != NULL) {
        ESP_LOGI(TAG, "等待播放任务结束");
        int wait_cnt = 0;
        // 最多等待2秒
        while (g_play_task_handle != NULL && wait_cnt < 100) {
            vTaskDelay(pdMS_TO_TICKS(20)); 
            wait_cnt++;
        }
        
        // 如果任务还在运行，强制删除（极大概率已死锁）
        if (g_play_task_handle != NULL) {
            ESP_LOGW(TAG, "强制删除播放任务，可能导致内存泄漏");
            vTaskDelete(g_play_task_handle);
            g_play_task_handle = NULL;
        }
    }
    
    xSemaphoreGive(g_play_mutex);
    ESP_LOGI(TAG, "停止播放铃声成功");
    return ESP_OK;
}

// 获取播放状态
bool ringtone_player_is_playing(void)
{
    return g_is_playing;
}