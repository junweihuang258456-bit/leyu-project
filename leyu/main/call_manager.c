/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"      // FreeRTOS 核心头文件
#include "freertos/task.h"          // FreeRTOS 任务管理
#include "freertos/timers.h"        // FreeRTOS 软件定时器
#include "freertos/event_groups.h"  // FreeRTOS 事件组
#include "esp_log.h"                // ESP-IDF 日志系统
#include "esp_err.h"                // ESP-IDF 错误码定义
#include "driver/gpio.h"            // GPIO 驱动
#include "call_manager.h"           // 本模块头文件
#include <stdlib.h>                 // 标准库
#include "baidu_chat_agents_engine.h" // 百度TTS引擎接口
#include "audio_processor.h"        // 音频处理器
#include "at_3gpp_ts_27_007.h"      // 3GPP TS 27.007 AT指令集
#include "soc/gpio_num.h"
#include "usb_rndis_4g_module.h"    // USB RNDIS 4G模块驱动
#include "brtc_app.h"               // BRCO 应用接口

static const char *TAG = "CALL_MANAGER";  // 日志标签

// ============================================
// 全局变量定义
// ============================================

/**
 * @brief 全局标志，用于指示是否正在播放铃声
 * @note  此变量用于外部模块检查铃声播放状态，避免与AI音频系统冲突
 */
bool g_is_playing_ringtone = false;

/**
 * @brief 检查当前是否正在播放铃声
 * @return true - 正在播放, false - 未播放
 * @note  通过全局标志判断铃声播放状态
 */
bool call_manager_is_ringtone_playing(void)
{
    return g_is_playing_ringtone;
}

// 外部变量声明（来自4G模块驱动和BRTC）
extern char *g_at_command_buffer;    // AT指令发送缓冲区
extern char *g_at_response_buffer;   // AT指令响应缓冲区
extern SemaphoreHandle_t g_at_mutex; // AT指令全局互斥锁
extern at_handle_t g_at_handle;      // AT指令句柄
extern struct baidu_rtc_t s_bdrtc;   // BRTC全局实例

// 外部函数声明（来自button_handler）
extern void button_handler_set_call_state(bool in_call);  // 设置通话状态

// ============================================
// 事件组与事件位定义
// ============================================

static EventGroupHandle_t g_event_group = NULL;  // 事件组句柄

// 事件位定义
#define AUTO_HANGUP_EVENT    (1 << 0)   // 自动挂断事件（5分钟无人接听）
#define RING_STOP_EVENT      (1 << 1)   // 停止铃声播放事件

// ============================================
// 拦截规则与来电信息
// ============================================

/**
 * @brief 拦截规则数组，最多支持 MAX_INTERCEPT_RULES 条规则
 */
static intercept_rule_t g_intercept_rules[MAX_INTERCEPT_RULES];

/**
 * @brief 当前拦截规则数量
 */
static int g_intercept_rule_count = 0;

/**
 * @brief 当前来电信息结构体
 * @note  保存当前来电的电话号码、状态、自动挂断定时器等
 */
static incoming_call_info_t g_current_call = {
    .caller_number = "",           // 来电号码
    .state = CALL_STATE_IDLE,      // 初始状态：空闲
    .auto_hangup_timer = NULL      // 自动挂断定时器
};

// 铃声播放任务函数声明
static void ringtone_playback_task(void *pvParameters);

// ============================================
// 铃声播放控制函数
// ============================================

/**
 * @brief 启动铃声播放任务
 * @return ESP_OK - 成功, 其他 - 失败
 * @note  使用内部铃声播放任务，通过esp_codec_dev播放，避免I2S通道冲突
 */
static esp_err_t start_ringtone_task(void)
{
    ESP_LOGI(TAG, "启动铃声播放任务");
    
    // 设置全局标志，指示正在播放铃声
    g_is_playing_ringtone = true;
    
    // 清除停止事件（如果有之前的事件）
    if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, RING_STOP_EVENT);
    }
    
    // 创建铃声播放任务（使用内部实现，通过esp_codec_dev播放）
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(ringtone_playback_task, "ringtone_task", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建铃声播放任务失败");
        g_is_playing_ringtone = false;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "铃声播放任务已启动");
    return ESP_OK;
}

/**
 * @brief 停止铃声播放任务
 * @note  发送停止事件，等待任务结束，清除全局标志
 */
static void stop_ringtone_task(void)
{
    ESP_LOGI(TAG, "停止铃声播放任务");
    
    // 设置停止标志，通知播放任务停止
    g_is_playing_ringtone = false;
    
    // 发送停止事件（如果任务正在等待）
    if (g_event_group != NULL) {
        xEventGroupSetBits(g_event_group, RING_STOP_EVENT);
    }
    
    // 停止音频播放（如果使用audio_playback_play）
    audio_playback_stop();
    
    // 强制打断 BRTC 引擎的 TTS 语音播报，防止提示音漏到电话另一端
    if (s_bdrtc.engine != NULL) {
        baidu_chat_agent_engine_interrupt(s_bdrtc.engine);
        ESP_LOGI(TAG, "已打断BRTC TTS播报");
    }
    
    // 给任务一些时间来停止
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 恢复系统初始音量
    esp_codec_dev_handle_t play_dev = get_play_dev_handle();
    if (play_dev != NULL) {
        float original_vol = brtc_get_volume();
        esp_codec_dev_set_out_vol(play_dev, (int)original_vol);
        ESP_LOGI(TAG, "已恢复系统初始音量: %d", (int)original_vol);
    }
    
    ESP_LOGI(TAG, "铃声播放任务已停止");
}

/**
 * @brief 独立铃声播放函数（与AI音频系统隔离）
 * @return ESP_OK - 成功, 其他 - 失败
 * @note  
 *   1. 此函数独立管理音频设备，避免与AI音频系统冲突
 *   2. 配置音频参数（16KHz, 双声道, 16bit）
 *   3. 播放 /spiffs/ring.mp3 铃声文件
 *   4. 支持最大20秒播放，可通过事件提前停止
 */
static esp_err_t play_ringtone_independent(void)
{
    // 获取音频设备句柄
    esp_codec_dev_handle_t play_dev = get_play_dev_handle();
    if (play_dev == NULL) {
        ESP_LOGE(TAG, "无法获取音频设备句柄");
        return ESP_FAIL;
    }
    
    // 先停止当前正在播放的音频
    audio_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 设置适当的音量（80%）
    // esp_codec_dev_set_out_vol(play_dev, 80);
    
    // 播放铃声文件（MP3格式，需要通过ESP-ADF解码）
    esp_err_t ret = audio_playback_play("file:///spiffs/ring.mp3");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放铃声失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 等待播放完成或收到停止事件
    const int max_play_time_sec = 20;   // 最大播放时间：20秒
    const int check_interval_ms = 500;  // 检查间隔：500毫秒
    
    for (int elapsed_ms = 0; elapsed_ms < max_play_time_sec * 1000 && g_is_playing_ringtone; elapsed_ms += check_interval_ms) {
        // 检查停止事件
        if (g_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(g_event_group, RING_STOP_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            if (bits & RING_STOP_EVENT) {
                ESP_LOGI(TAG, "收到停止铃声事件，停止播放");
                audio_playback_stop();
                return ESP_OK;
            }
        }
        
        // 等待500毫秒
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
    }
    
    // 播放完成或超时，停止播放
    audio_playback_stop();
    
    return ESP_OK;
}

/**
 * @brief 铃声播放任务（FreeRTOS任务）
 * @param pvParameters 任务参数（未使用）
 * @note  
 *   1. 这是一个独立的后台任务，负责循环播放铃声
 *   2. 每次播放完成后等待3秒再重新播放
 *   3. 检查 RTC_STOP_EVENT 事件来响应停止请求
 *   4. 处理RTC模式切换，避免与AI音频系统冲突
 */
static void ringtone_playback_task(void *pvParameters)
{
    ESP_LOGI(TAG, "铃声播放任务开始运行");
    int play_count = 0;  // 播放计数器
    
    // ===== 新增: 等待TTS来电播报完成 =====
    int wait_tts_count = 0;
    while (brtc_is_playing() && wait_tts_count < 20) { // 最多等10秒
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_tts_count++;
        
        // 检查是否在播报过程中被挂断或接听
        if (g_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(g_event_group, RING_STOP_EVENT, pdFALSE, pdFALSE, 0);
            if (bits & RING_STOP_EVENT) {
                ESP_LOGI(TAG, "收到停止铃声事件，取消播报和铃声");
                goto stop_playback;
            }
        }
    }
    
    // 循环播放，直到收到停止命令
    while (g_is_playing_ringtone) {
        // 检查是否有停止事件
        if (g_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(g_event_group, RING_STOP_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            if (bits & RING_STOP_EVENT) {
                ESP_LOGI(TAG, "收到停止铃声事件，停止播放");
                break;
            }
        }
        
        ESP_LOGI(TAG, "开始播放铃声，第 %d 次", play_count + 1);
        
        // 设置全局标志
        g_is_playing_ringtone = true;
        
        // 检查是否处于RTC模式，RTC模式会占用音频资源
        bool was_rtc_mode = brtc_is_rtc_mode();
        if (was_rtc_mode) {
            ESP_LOGI(TAG, "检测到RTC模式，可能影响铃声播放");
            // 临时切换到AI模式以释放RTC占用的音频资源
            brtc_switch_to_ai_mode();
            vTaskDelay(pdMS_TO_TICKS(500)); // 等待模式切换完成
        }
        
        // 我们移除了原有的 was_playing 检查和强制 stop 逻辑，
        // 防止意外打断前置的 TTS 或后续的音频操作。
        
        // 不应直接 close/open 底层 codec 设备，这会破坏音频管线的内部状态并导致崩溃
        // audio_playback_stop() 已经负责停止当前音频流。
        
        esp_codec_dev_handle_t play_dev = get_play_dev_handle();
        if (play_dev != NULL) {
            // 设置音量
            // esp_codec_dev_set_out_vol(play_dev, 80);
        } else {
            ESP_LOGW(TAG, "无法获取音频设备句柄");
        }
        
        // 播放铃声
        esp_err_t ret = play_ringtone_independent();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "播放铃声失败: %s", esp_err_to_name(ret));
            g_is_playing_ringtone = false;
            vTaskDelay(pdMS_TO_TICKS(1000));  // 失败后等待1秒重试
            continue;
        }
        
        play_count++;
        
        // 每次播放完成后等待3秒再重新播放
        if (g_is_playing_ringtone) {
            ESP_LOGI(TAG, "铃声播放完成，等待3秒后重新播放");
            for (int i = 0; i < 3 && g_is_playing_ringtone; i++) {
                // 检查停止事件
                if (g_event_group != NULL) {
                    EventBits_t bits = xEventGroupWaitBits(g_event_group, RING_STOP_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
                    if (bits & RING_STOP_EVENT) {
                        ESP_LOGI(TAG, "收到停止铃声事件，停止播放");
                        goto stop_playback;  // 跳转到清理代码
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    
stop_playback:
    // 清理工作
    g_is_playing_ringtone = false;
    
    if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, RING_STOP_EVENT);
    }
    
    ESP_LOGI(TAG, "铃声播放任务结束，总共播放了 %d 次", play_count);
    vTaskDelete(NULL);  // 删除当前任务
}

// ============================================
// 自动挂断定时器回调
// ============================================

/**
 * @brief 自动挂断定时器回调函数
 * @param xTimer 定时器句柄
 * @note  
 *   1. 来电5分钟无人接听时触发
 *   2. 使用事件机制通知主任务处理，避免在定时器回调中执行复杂操作
 */
static void auto_hangup_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "五分钟无人接听，自动挂断电话");
    
    // 只在响铃状态下触发挂断
    if (g_current_call.state == CALL_STATE_RINGING) {
        g_current_call.state = CALL_STATE_HANGING_UP;
        
        // 发送事件通知主任务
        if (g_event_group != NULL) {
            xEventGroupSetBits(g_event_group, AUTO_HANGUP_EVENT);
        }
    }
}

// ============================================
// 初始化与反初始化
// ============================================

/**
 * @brief 初始化来电管理器
 * @return ESP_OK - 成功, 其他 - 失败
 * @note  
 *   1. 初始化拦截规则数组
 *   2. 创建事件组
 *   3. 初始化铃声播放器
 *   4. 创建自动挂断定时器（5分钟）
 */
esp_err_t call_manager_init(void)
{
    ESP_LOGI(TAG, "初始化来电管理器");
    
    // 清空拦截规则
    memset(g_intercept_rules, 0, sizeof(g_intercept_rules));
    g_intercept_rule_count = 0;
    
    // 创建事件组
    g_event_group = xEventGroupCreate();
    if (g_event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_FAIL;
    }
    
    // 清空来电信息
    memset(&g_current_call, 0, sizeof(g_current_call));
    g_current_call.state = CALL_STATE_IDLE;
    
    // 创建自动挂断定时器（5分钟 = 300秒 = 300000毫秒）
    g_current_call.auto_hangup_timer = xTimerCreate(
        "auto_hangup",                   // 定时器名称
        pdMS_TO_TICKS(5 * 60 * 1000),    // 周期：5分钟
        pdFALSE,                         // 单次定时器（非周期性）
        NULL,                            // 定时器ID（未使用）
        auto_hangup_timer_callback       // 回调函数
    );
    
    if (g_current_call.auto_hangup_timer == NULL) {
        ESP_LOGE(TAG, "创建自动挂断定时器失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "来电管理器初始化完成");
    return ESP_OK;
}

/**
 * @brief 反初始化来电管理器
 * @note  清理所有资源：停止铃声、删除定时器、删除事件组等
 */
void call_manager_deinit(void)
{
    ESP_LOGI(TAG, "反初始化来电管理器");
    
    // 停止铃声播放
    stop_ringtone_task();
    
    // 删除自动挂断定时器
    if (g_current_call.auto_hangup_timer != NULL) {
        xTimerDelete(g_current_call.auto_hangup_timer, 0);
        g_current_call.auto_hangup_timer = NULL;
    }
    
    // 删除事件组
    if (g_event_group != NULL) {
        vEventGroupDelete(g_event_group);
        g_event_group = NULL;
    }
    
    // 清空拦截规则
    memset(g_intercept_rules, 0, sizeof(g_intercept_rules));
    g_intercept_rule_count = 0;
    
    // 清空来电信息
    memset(&g_current_call, 0, sizeof(g_current_call));
    g_current_call.state = CALL_STATE_IDLE;
}

// ============================================
// 拦截规则管理
// ============================================

/**
 * @brief 添加拦截规则
 * @param phone_number 电话号码
 * @param is_intercepted true-拦截, false-放行
 * @return ESP_OK - 成功, ESP_ERR_INVALID_ARG - 参数错误, ESP_ERR_NO_MEM - 规则已满
 */
esp_err_t call_manager_add_intercept_rule(const char *phone_number, bool is_intercepted)
{
    // 参数检查
    if (phone_number == NULL || strlen(phone_number) == 0) {
        ESP_LOGE(TAG, "电话号码为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查规则数量上限
    if (g_intercept_rule_count >= MAX_INTERCEPT_RULES) {
        ESP_LOGE(TAG, "拦截规则数量已达上限");
        return ESP_ERR_NO_MEM;
    }
    
    // 添加规则
    strncpy(g_intercept_rules[g_intercept_rule_count].phone_number, phone_number, MAX_PHONE_NUMBER_LENGTH - 1);
    g_intercept_rules[g_intercept_rule_count].phone_number[MAX_PHONE_NUMBER_LENGTH - 1] = '\0';
    g_intercept_rules[g_intercept_rule_count].is_intercepted = is_intercepted;
    g_intercept_rule_count++;
    
    ESP_LOGI(TAG, "添加拦截规则: %s, 拦截: %s", phone_number, is_intercepted ? "是" : "否");
    return ESP_OK;
}

/**
 * @brief 清除所有拦截规则
 * @return ESP_OK - 成功
 */
esp_err_t call_manager_clear_intercept_rules(void)
{
    ESP_LOGI(TAG, "清除所有拦截规则");
    memset(g_intercept_rules, 0, sizeof(g_intercept_rules));
    g_intercept_rule_count = 0;
    return ESP_OK;
}

// 通配符匹配：支持 *xxx, xxx*, *, 精确匹配
static bool wildcard_match(const char *pattern, const char *number) {
    // 空指针安全判断
    if (!pattern || !number) return false;

    int pat_len = strlen(pattern);
    int num_len = strlen(number);

    // 1. 全局规则 *
    if (strcmp(pattern, "*") == 0) {
        return true;
    }

    // 2. 前缀匹配 0592*
    if (pattern[pat_len - 1] == '*') {
        int prefix_len = pat_len - 1;
        // 直接比较，不需要复制！更安全更快
        return strncmp(number, pattern, prefix_len) == 0;
    }

    // 3. 后缀匹配 *1560592
    if (pattern[0] == '*') {
        const char *suffix = pattern + 1;
        int suffix_len = strlen(suffix);

        if (num_len < suffix_len)
            return false;

        // 比较最后 suffix_len 个字符
        return strcmp(number + num_len - suffix_len, suffix) == 0;
    }

    // 4. 精确匹配
    return strcmp(pattern, number) == 0;
}

/**
 * @brief 检查号码是否被拦截
 * @param phone_number 电话号码
 * @return true - 被拦截, false - 放行
 * @note  如果号码不在规则列表中，默认放行
 */
bool call_manager_check_intercept(const char *phone_number)
{
    if (phone_number == NULL || strlen(phone_number) == 0) {
        ESP_LOGW(TAG, "电话号码为空，默认拦截");
        return true;
    }
    
    // 遍历规则列表
    for (int i = 0; i < g_intercept_rule_count; i++) {
        if (wildcard_match(g_intercept_rules[i].phone_number, phone_number)) {
            ESP_LOGI(TAG, "号码 %s 在放行规则中，放行: %s", phone_number, 
                    g_intercept_rules[i].is_intercepted ? "是" : "否");
            return g_intercept_rules[i].is_intercepted;
        }
    }
    
    ESP_LOGI(TAG, "号码 %s 不在放行规则中，默认拦截", phone_number);
    return true;
}

// ============================================
// 来电处理
// ============================================

/**
 * @brief 处理来电事件
 * @param caller_number 来电号码
 * @return ESP_OK - 成功处理, 其他 - 错误
 * @note  
 *   1. 检查当前状态（必须处于IDLE状态）
 *   2. 检查拦截规则
 *   3. 如果被拦截，自动拒接
 *   4. 如果放行，播放铃声并启动自动挂断定时器
 */
esp_err_t call_manager_handle_incoming_call(const char *caller_number)
{
    // 参数检查
    if (caller_number == NULL || strlen(caller_number) == 0) {
        ESP_LOGE(TAG, "来电号码为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 状态检查（只有IDLE状态才能处理新来电）
    if (g_current_call.state != CALL_STATE_IDLE) {
        ESP_LOGW(TAG, "当前已有来电，忽略新来电");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "收到来电: %s", caller_number);
    
    // 保存来电号码
    strncpy(g_current_call.caller_number, caller_number, MAX_PHONE_NUMBER_LENGTH - 1);
    g_current_call.caller_number[MAX_PHONE_NUMBER_LENGTH - 1] = '\0';
    
    // 检查拦截规则
    bool should_intercept = call_manager_check_intercept(caller_number);
    
    if (should_intercept) {
        // 被拦截，自动拒接
        ESP_LOGI(TAG, "号码 %s 被拦截，自动拒接", caller_number);
        call_manager_reject_call();
        return ESP_OK;
    }
    
    // 放行，播放铃声
    ESP_LOGI(TAG, "号码 %s 放行，准备播放铃声", caller_number);
    // 设置为来电中状态
    g_current_call.state = CALL_STATE_RINGING;
    
    // ===== 新增: TTS播报来电号码 =====
    if (s_bdrtc.engine != NULL) {
        // 使用堆内存避免栈溢出
        char *tts_text = (char *)malloc(128);
        if (tts_text) {
            snprintf(tts_text, 128, "有新来电，号码是 ");
            int len = strlen(tts_text);
            // 插入空格以实现数字逐个播报（字正腔圆效果）
            for (int i = 0; caller_number[i] != '\0' && len < 120; i++) {
                tts_text[len++] = caller_number[i];
                tts_text[len++] = ' '; 
            }
            tts_text[len] = '\0';
            
            ESP_LOGI(TAG, "发起来电TTS播报: %s", tts_text);
            baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, tts_text);
            brtc_set_playing_state(true); // 设置标志让铃声任务等待播报完成
            free(tts_text);
        }
    }
    
    // 取消TTS与铃声的直接冲突，后续将由 ringtone_task 处理避让
    esp_codec_dev_handle_t play_dev = get_play_dev_handle();
    esp_codec_dev_set_out_vol(play_dev, 80);
    
    // 启动铃声播放任务
    esp_err_t ret = start_ringtone_task();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "启动铃声播放任务失败: %s，继续等待接听", esp_err_to_name(ret));
        // 铃声播放失败不影响接听功能，继续处理
    }
    
    // 启动自动挂断定时器（5分钟）
    if (xTimerStart(g_current_call.auto_hangup_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "启动自动挂断定时器失败");
        stop_ringtone_task();
        g_current_call.state = CALL_STATE_IDLE;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "来电响铃中，启动5分钟自动挂断定时器");
    return ESP_OK;
}

// ============================================
// 电话控制（接听、拒接、挂断）
// ============================================

/**
 * @brief 接听来电
 * @return ESP_OK - 成功, ESP_ERR_INVALID_STATE - 状态错误
 * @note  
 *   1. 只有CALL_STATE_RINGING状态才能接听
 *   2. 停止铃声播放
 *   3. 发送ATA指令接听电话
 *   4. 设置GPIO47为低电平（可能是接听指示灯）
 *   5. 停止自动挂断定时器
 */
esp_err_t call_manager_answer_call(void)
{
    // 状态检查
    if (g_current_call.state != CALL_STATE_RINGING) {
        ESP_LOGW(TAG, "当前状态不是来电中，无法接听");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "接听来电: %s", g_current_call.caller_number);
    
    // 停止铃声播放
    stop_ringtone_task();
    ESP_LOGI(TAG, "停止播放铃声");
    
    // 发送ATA指令接听电话
    if (g_at_command_buffer != NULL && g_at_response_buffer != NULL && g_at_handle != NULL) {
        esp_err_t at_ret;
        
    if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
        memset(g_at_command_buffer, 0, 256);
        memset(g_at_response_buffer, 0, 1024);
        strcpy(g_at_command_buffer, "ATA");  // ATA = Answer
        
        at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
        xSemaphoreGive(g_at_mutex);
        if (at_ret == ESP_OK) {
            ESP_LOGI(TAG, "ATA接听指令发送成功");
            ESP_LOGI(TAG, "ATA指令响应: %s", g_at_response_buffer);
            
            // 切换到 4G 音频输出通道并开启 PA
            // 必须先关闭 PA，防止切换瞬间产生巨大的瞬态电流
            gpio_set_level(GPIO_NUM_48, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            
            gpio_set_level(GPIO_NUM_47, 1); // 1 = 4G输出
            vTaskDelay(pdMS_TO_TICKS(50));  // 等待通道切换稳定
            
            gpio_set_level(GPIO_NUM_48, 1); // 重新开启PA
            
            // 更新状态为通话中
            g_current_call.state = CALL_STATE_IN_CALL;
            
            // 设置 button_handler 的通话状态（用于统一挂断处理）
            button_handler_set_call_state(true);
            ESP_LOGI(TAG, "设置button_handler通话状态为true");
            
            // 停止自动挂断定时器
            if (xTimerIsTimerActive(g_current_call.auto_hangup_timer)) {
                if (xTimerStop(g_current_call.auto_hangup_timer, 0) == pdPASS) {
                    ESP_LOGI(TAG, "停止自动挂断定时器");
                }
            }
            
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "ATA接听指令发送失败 - 错误: %d", at_ret);
            return at_ret;
        }
    } else {
        ESP_LOGE(TAG, "无法获取AT Mutex");
        return ESP_FAIL;
    }
    } else {
        ESP_LOGE(TAG, "AT指令缓冲区或句柄未初始化");
        return ESP_ERR_INVALID_STATE;
    }
}

/**
 * @brief 拒接来电
 * @return ESP_OK - 成功, ESP_ERR_INVALID_STATE - 状态错误
 * @note  
 *   1. CALL_STATE_RINGING或CALL_STATE_IN_CALL状态都可以拒接
 *   2. 停止铃声播放
 *   3. 发送ATH指令挂断电话
 *   4. 重置状态为IDLE
 */
esp_err_t call_manager_reject_call(void)
{
    // 状态检查
    if (g_current_call.state != CALL_STATE_RINGING && g_current_call.state != CALL_STATE_IN_CALL) {
        ESP_LOGW(TAG, "当前状态不是来电中或通话中，无法拒接");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "拒接来电: %s", g_current_call.caller_number);
    
    // 停止铃声播放
    stop_ringtone_task();
    ESP_LOGI(TAG, "停止播放铃声");
    
    // 发送ATH指令挂断电话
    if (g_at_command_buffer != NULL && g_at_response_buffer != NULL && g_at_handle != NULL) {
        esp_err_t at_ret;
        
    if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
        memset(g_at_command_buffer, 0, 256);
        memset(g_at_response_buffer, 0, 1024);
        strcpy(g_at_command_buffer, "ATH");  // ATH = Hang up
        
        at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
        xSemaphoreGive(g_at_mutex);
        if (at_ret == ESP_OK) {
            ESP_LOGI(TAG, "ATH拒接指令发送成功");
            ESP_LOGI(TAG, "ATH指令响应: %s", g_at_response_buffer);
            
            // 重置状态
            g_current_call.state = CALL_STATE_IDLE;
            memset(g_current_call.caller_number, 0, sizeof(g_current_call.caller_number));
            
            // 停止自动挂断定时器
            if (xTimerIsTimerActive(g_current_call.auto_hangup_timer)) {
                if (xTimerStop(g_current_call.auto_hangup_timer, 0) == pdPASS) {
                    ESP_LOGI(TAG, "停止自动挂断定时器");
                }
            }
            
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "ATH拒接指令发送失败 - 错误: %d", at_ret);
            return at_ret;
        }
    } else {
        ESP_LOGE(TAG, "无法获取AT Mutex");
        return ESP_FAIL;
    }
    } else {
        ESP_LOGE(TAG, "AT指令缓冲区或句柄未初始化");
        return ESP_ERR_INVALID_STATE;
    }
}

/**
 * @brief 挂断电话（与拒接功能相同，但语义不同）
 * @return ESP_OK - 成功, ESP_ERR_INVALID_STATE - 状态错误
 * @note  用于通话中主动挂断或响铃时取消接听
 */
esp_err_t call_manager_hangup_call(void)
{
    // 状态检查
    if (g_current_call.state != CALL_STATE_IN_CALL && g_current_call.state != CALL_STATE_RINGING) {
        ESP_LOGW(TAG, "当前状态不是通话中或来电中，无法挂断");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "挂断电话: %s", g_current_call.caller_number);
    
    // 停止铃声播放
    stop_ringtone_task();
    ESP_LOGI(TAG, "停止播放铃声");
    
    // 发送ATH指令挂断电话
    if (g_at_command_buffer != NULL && g_at_response_buffer != NULL && g_at_handle != NULL) {
        esp_err_t at_ret;
        
    if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
        memset(g_at_command_buffer, 0, 256);
        memset(g_at_response_buffer, 0, 1024);
        strcpy(g_at_command_buffer, "ATH");
        
        at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
        xSemaphoreGive(g_at_mutex);
        if (at_ret == ESP_OK) {
            ESP_LOGI(TAG, "ATH挂断指令发送成功");
            ESP_LOGI(TAG, "ATH指令响应: %s", g_at_response_buffer);
            
            // 重置状态
            g_current_call.state = CALL_STATE_IDLE;
            memset(g_current_call.caller_number, 0, sizeof(g_current_call.caller_number));
            
            // 停止自动挂断定时器
            if (xTimerIsTimerActive(g_current_call.auto_hangup_timer)) {
                if (xTimerStop(g_current_call.auto_hangup_timer, 0) == pdPASS) {
                    ESP_LOGI(TAG, "停止自动挂断定时器");
                }
            }
            
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "ATH挂断指令发送失败 - 错误: %d", at_ret);
            return at_ret;
        }
    } else {
        ESP_LOGE(TAG, "无法获取AT Mutex");
        return ESP_FAIL;
    }
    } else {
        ESP_LOGE(TAG, "AT指令缓冲区或句柄未初始化");
        return ESP_ERR_INVALID_STATE;
    }
}

/**
 * @brief 处理电话结束事件（对方挂断）
 * @note 当收到 NO CARRIER 等非请求响应时调用
 *       用于清理通话状态，不发送ATH指令
 */
void call_manager_handle_call_ended(void)
{
    ESP_LOGI(TAG, "处理电话结束事件");
    
    // 只有在通话中或响铃状态才需要处理
    if (g_current_call.state != CALL_STATE_IN_CALL && 
        g_current_call.state != CALL_STATE_RINGING) {
        ESP_LOGD(TAG, "当前状态不是通话中或响铃中，无需处理: %d", g_current_call.state);
        return;
    }
    
    ESP_LOGI(TAG, "电话已结束: %s", g_current_call.caller_number);
    
    // 停止铃声播放
    stop_ringtone_task();
    ESP_LOGI(TAG, "停止播放铃声");
    
    // 重置通话状态
    g_current_call.state = CALL_STATE_IDLE;
    memset(g_current_call.caller_number, 0, sizeof(g_current_call.caller_number));
    
    // 停止自动挂断定时器
    if (xTimerIsTimerActive(g_current_call.auto_hangup_timer)) {
        if (xTimerStop(g_current_call.auto_hangup_timer, 0) == pdPASS) {
            ESP_LOGI(TAG, "停止自动挂断定时器");
        }
    }
    
    ESP_LOGI(TAG, "电话结束处理完成");
}

// ============================================
// 状态查询
// ============================================

/**
 * @brief 获取当前通话状态
 * @return 当前状态（CALL_STATE_IDLE/RINGING/IN_CALL/HANGING_UP）
 */
call_state_t call_manager_get_state(void)
{
    return g_current_call.state;
}

/**
 * @brief 获取来电号码
 * @return 来电号码字符串（如果没有来电则返回空字符串）
 */
const char *call_manager_get_caller_number(void)
{
    return g_current_call.caller_number;
}

// ============================================
// 事件处理
// ============================================

/**
 * @brief 处理来电管理器事件
 * @note  
 *   此函数应该在主循环中定期调用，用于处理：
 *   1. 自动挂断事件（5分钟无人接听）
 *   2. 清理停止铃声事件位
 */
void call_manager_process_events(void)
{
    if (g_event_group == NULL) {
        return;
    }
    
    // 检查自动挂断事件
    EventBits_t bits = xEventGroupGetBits(g_event_group);
    if (bits & AUTO_HANGUP_EVENT) {
        // 清除事件位
        xEventGroupClearBits(g_event_group, AUTO_HANGUP_EVENT);
        
        // 处理自动挂断
        ESP_LOGI(TAG, "处理自动挂断事件");
        if (g_current_call.state == CALL_STATE_HANGING_UP) {
            call_manager_hangup_call();
        }
    }
    
    // 检查停止铃声事件（清理用）
    if (bits & RING_STOP_EVENT) {
        xEventGroupClearBits(g_event_group, RING_STOP_EVENT);
        ESP_LOGD(TAG, "清理停止铃声事件位");
    }
}
