/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in
 * which case, it is free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#pragma once

#include "av_processor_type.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_thread.h"
#include "mqtt5_client.h"

#include "audio_processor.h"
#include "baidu_chat_agents_engine.h"
#include "baidu_rtc_client.h"
#include "basic_board.h"
#include "uart_command.h"

#include "blufi_example.h"
// #include "brtc_app.h"
#include "button_handler.h"
#include "call_manager.h"
#include "contact_manager.h"
#include "http_ota.h"
#include "mqtt_example.h"
#include "ota.h"
#include "phone_numbers.h"

#include "baidu_chat_agents_engine.h"
#include "esp_codec_dev.h"
#include "esp_err.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *appid;
  char *userId;
  char *workflow;
  char *license_key;
  char *config_server_url;
  int instance_id;
} baidu_rtc_config_t;

struct baidu_rtc_t {
  baidu_rtc_config_t config;
  esp_gmf_oal_thread_t read_thread;
  esp_gmf_oal_thread_t play_ctrl_thread;
  esp_gmf_oal_thread_t button_wakeup_thread; // 按键唤醒处理线程
  BaiduChatAgentEngine *engine;
  EventGroupHandle_t join_event;
  EventGroupHandle_t wait_destory_event;
  EventGroupHandle_t wakeup_event;
  QueueHandle_t audio_player_queue;
  bool byte_rtc_running;
  bool data_proc_running;
  bool is_connected;
  bool is_playing;                        // 播放状态标志
  float current_volume;                   // 当前音量值
  esp_codec_dev_handle_t play_dev_handle; // 播放设备句柄
  esp_codec_dev_handle_t rec_dev_handle;  // 录音设备句柄
  bool is_rtc_mode;                       // RTC模式标志
  void *rtc_client;                       // BRTC客户端实例引用
  bool is_destroying;                     // 引擎正在销毁标志
  bool is_rtc_stream_up;                  // RTC流已建立标志
  audio_player_state_t playback_state;    // 播放状态
};

/**
 * @brief  Initialize the baidu RTC module.
 *
 * @return
 *     - ESP_OK: Initialization was successful.
 *     - Other: Appropriate erro code indicating the failure reason.
 */
esp_err_t brtc_init(void);

/**
 * @brief  Deinitialize the baidu RTC module..
 *
 * @return
 *     - ESP_OK: Deinitialization was successful.
 */
esp_err_t brtc_deinit(void);

/**
 * @brief  Get the wakeup event handle for external use.
 *
 * @return
 *     - EventGroupHandle_t: The wakeup event handle, or NULL if not
 * initialized.
 */
EventGroupHandle_t brtc_get_wakeup_event_handle(void);

/**
 * @brief  设置音量
 * @param  volume_db 音量值（0-100dB）
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t brtc_set_volume_db(float volume_db);

/**
 * @brief  获取当前音量
 * @return 当前音量值
 */
float brtc_get_volume(void);

/**
 * @brief  设置播放状态
 * @param  is_playing true表示播放中，false表示停止
 */
void brtc_set_playing_state(bool is_playing);

/**
 * @brief  获取当前播放状态
 * @return true表示播放中，false表示停止
 */
bool brtc_is_playing(void);

/**
 * @brief  获取当前RTC模式状态
 * @return true表示RTC模式，false表示AI模式
 */
bool brtc_is_rtc_mode(void);

/**
 * @brief  增加音量（步长5dB）
 */
void brtc_volume_up(void);

/**
 * @brief  减少音量（步长5dB）
 */
void brtc_volume_down(void);

/**
 * @brief  使用新的房间参数重新初始化brtc
 *
 * @param appid 百度appid
 * @param token 鉴权使用串
 * @param userid 用户id
 * @param room_name 房间号
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t brtc_reinit_with_new_room(const char *appid, const char *token,
                                    const char *userid, const char *room_name);

/**
 * @brief  销毁brtc引擎
 */
void byte_rtc_engine_destroy(void);

/**
 * @brief  设置RTC模式
 * @param  enable true表示启用RTC模式，false表示禁用
 * @param  rtc_client BRTC客户端实例指针
 */
void brtc_set_rtc_mode(bool enable, void *rtc_client);

/**
 * @brief  设置RTC流建立状态
 * @param  is_stream_up true表示流已建立，false表示流未建立
 */
void brtc_set_rtc_stream_up(bool is_stream_up);

/**
 * @brief  从RTC房间退出并恢复AI对话功能
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t brtc_switch_to_ai_mode(void);

/**
 * @brief  获取播放设备句柄
 * @return 播放设备句柄，如果未初始化则返回NULL
 */
esp_codec_dev_handle_t get_play_dev_handle(void);

/**
 * @brief  获取录音设备句柄
 * @return 录音设备句柄，如果未初始化则返回NULL
 */
esp_codec_dev_handle_t get_rec_dev_handle(void);

/**
 * @brief  打开音频管道
 */
void audio_pipe_open(void);

/**
 * @brief  获取BaiduChatAgentEngine引擎句柄
 * @return BaiduChatAgentEngine引擎句柄，如果未初始化则返回NULL
 */
BaiduChatAgentEngine *brtc_get_engine_handle(void);

extern bool is_sip_mode;
extern bool is_sip_flag;
void audio_pipe_sip_mode(bool enable);
void brtc_sip_error(void);
void brtc_sip_end(void);

#ifdef __cplusplus
}
#endif
