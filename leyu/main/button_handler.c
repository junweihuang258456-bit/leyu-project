/**
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

#include "button_handler.h"
#include "at_3gpp_ts_27_007.h" // 添加AT指令相关头文件
#include "audio_processor.h"
#include "baidu_chat_agents_engine.h" // 添加TTS相关头文件
#include "brtc_app.h"
#include "call_manager.h" // 添加来电管理头文件
#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h" // 添加esp_netif_t定义
#include "esp_rtc.h"
#include "esp_sleep.h"  // 添加睡眠模式头文件
#include "esp_system.h" // 添加系统关机头文件
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_example.h"
#include "phone_numbers.h" // 添加电话号码管理头文件
#include "sip/sip_service.h"
#include "sip_service.h"         // 添加SIP服务头文件
#include "usb_rndis_4g_module.h" // 添加4G模块相关头文件
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern esp_blufi_callbacks_t example_callbacks;
extern struct baidu_rtc_t s_bdrtc;
extern bool gl_sta_connected;
extern bool gl_sta_is_connecting;
extern void example_wifi_connect(void);

static uint8_t long_press_count_gpio40;

// ============================================
// 按键事件类型定义
// ============================================

typedef enum {
  BUTTON_EVENT_SHORT_PRESS, // 短按事件
  BUTTON_EVENT_LONG_PRESS,  // 长按事件
  BUTTON_EVENT_DOUBLE_CLICK // 双击事件
} button_event_type_t;

// 按键事件结构体
typedef struct {
  button_event_type_t type;
  gpio_num_t gpio;
} button_event_t;

// 按键事件队列句柄
static QueueHandle_t button_event_queue = NULL;

#define TAG "BUTTON_HANDLER"

// 声明外部AT上下文变量和缓冲区
extern void *g_at_ctx; // 使用void*指针避免类型定义问题
extern char *g_at_command_buffer;
extern char *g_at_response_buffer;
extern SemaphoreHandle_t g_at_mutex;

// 声明外部AT句柄
extern at_handle_t g_at_handle;

// ============================================
// 电话状态管理
// ============================================

/**
 * @brief 电话状态标志
 * @note  true - 正在通话中, false - 未在通话
 *       此标志用于跟踪当前是否处于通话状态，影响按键行为
 */
static bool g_is_in_call = false;

/**
 * @brief 获取电话状态
 * @return true - 正在通话中, false - 未在通话
 */
bool button_handler_is_in_call(void) { return g_is_in_call; }

/**
 * @brief 设置电话状态
 * @param in_call true - 正在通话中, false - 未在通话
 * @note 用于外部模块（如AT响应处理）设置通话状态
 */
void button_handler_set_call_state(bool in_call) {
  g_is_in_call = in_call;
  ESP_LOGI(TAG, "电话状态已设置为: %s", in_call ? "通话中" : "非通话中");
}

// AT指令拨号函数声明
esp_err_t at_make_phone_call(const char *phone_number);

// ============================================
// 按键GPIO与状态定义
// ============================================

// 按键GPIO定义
#define BUTTON_GPIO_38 GPIO_NUM_38
#define BUTTON_GPIO_39 GPIO_NUM_39
#define BUTTON_GPIO_40 GPIO_NUM_40
#define BUTTON_GPIO_4 GPIO_NUM_4

// 按键状态定义（上拉输入，按下为低电平）
#define BUTTON_RELEASED 1 // 按键释放（高电平）
#define BUTTON_PRESSED 0  // 按键按下（低电平）

// 消抖时间定义 (毫秒)
#define DEBOUNCE_TIME_MS 50

// 长按时间定义 (毫秒) - 2秒关机
#define LONG_PRESS_TIME_MS 2000

// 提示音播放时间 (毫秒) - 2.5秒
#define SHUTDOWN_PROMPT_DURATION_MS 2500

// 唤醒事件位定义
#define BUTTON_WAKEUP_BIT (1 << 0)

// 按键信息结构体
typedef struct {
  gpio_num_t gpio_num;       // GPIO编号
  uint8_t last_state;        // 上次状态
  uint64_t last_change_time; // 上次状态变化时间
  uint64_t press_start_time; // 按下开始时间
  bool long_press_triggered; // 长按是否已触发
  uint64_t last_short_press_time; // 上次短按时间
  const char *name;          // 按键名称
} button_info_t;

// 按键信息数组（4个按键）
static button_info_t buttons[] = {
    {BUTTON_GPIO_38, BUTTON_RELEASED, 0, 0, false, 0, "按键38"},
    {BUTTON_GPIO_39, BUTTON_RELEASED, 0, 0, false, 0, "按键39"},
    {BUTTON_GPIO_40, BUTTON_RELEASED, 0, 0, false, 0, "按键40"},
    {BUTTON_GPIO_4, BUTTON_RELEASED, 0, 0, false, 0, "按键4(唤醒/关机)"}};

// 按键扫描任务句柄
static TaskHandle_t button_scan_task_handle = NULL;

// 按键事件处理任务句柄
static TaskHandle_t button_event_task_handle = NULL;

// 外部函数声明 - 用于获取唤醒事件句柄
EventGroupHandle_t brtc_get_wakeup_event_handle(void);

// 外部函数声明 - 用于音量控制
bool brtc_is_playing(void);
void brtc_volume_up(void);
void brtc_volume_down(void);
void shutdown_V(void);

#include "sip_service.h"
extern esp_rtc_handle_t s_esp_sip;

// ============================================
// 按键短按处理函数 - 核心挂断逻辑
// ============================================

/**
 * @brief 处理按键短按事件
 * @param gpio_num 按键GPIO号
 * @note
 *   按键功能映射：
 *   - GPIO38(按键38): 音量增加 / 子女1一键拨号
 *   - GPIO39(按键39): 音量减少 / 子女2一键拨号
 *   - GPIO40(按键40): 接听来电 / 挂断通话 / SOS一键拨号
 *   - GPIO4(按键4):   拒接来电 / 唤醒设备 / 长按2秒关机
 */
static void handle_button_short_press(gpio_num_t gpio_num) {
  switch (gpio_num) {
  // ============================================
  // 按键39: 音量减少 / 子女1拨号
  // ============================================
  case BUTTON_GPIO_39:
    ESP_LOGI(TAG, "按键39功能：音量减少，子女1一键拨号");
    // if (!is_sip_mode) {
    //   is_sip_mode = true;
    //   brtc_volume_up();
    //   vTaskDelay(pdMS_TO_TICKS(2 * 1000));
    //   is_sip_mode = false;
    // } else {
    //   brtc_volume_up();
    // }
    // brtc_set_playing_state(false);
    // 只有在播放状态时才允许调节音量
    if (g_is_in_call || brtc_is_playing()) {
      ESP_LOGI(TAG, "处理音量减少事件");
      brtc_volume_down();
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // 从PSRAM获取子女1绑定号码
      const char *child1_number = get_phone_number_by_key(0); // 0表示子女1
      if (child1_number != NULL && strlen(child1_number) > 0) {
        ESP_LOGI(TAG, "子女1绑定号码: %s", child1_number);
        // 使用封装的AT指令拨打电话函数
        esp_err_t call_ret = at_make_phone_call(child1_number);
        if (call_ret != ESP_OK) {
          ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
        }
      } else {
        ESP_LOGW(TAG, "子女1绑定号码为空或未设置，播放提示音");
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "子女1绑定号码为空或未设置");
      }
    }
    break;

  // ============================================
  // 按键38: 音量减少 / 子女2拨号
  // ============================================
  case BUTTON_GPIO_38:
    ESP_LOGI(TAG, "按键38功能：音量增加，子女2一键拨号");
    // 只有在播放状态时才允许调节音量
    if (g_is_in_call || brtc_is_playing()) {
      ESP_LOGI(TAG, "处理音量增加事件");
      brtc_volume_up();
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // 从PSRAM获取子女2绑定号码
      const char *child2_number = get_phone_number_by_key(1); // 1表示子女2
      if (child2_number != NULL && strlen(child2_number) > 0) {
        ESP_LOGI(TAG, "子女2绑定号码: %s", child2_number);
        // 使用封装的AT指令拨打电话函数
        esp_err_t call_ret = at_make_phone_call(child2_number);
        if (call_ret != ESP_OK) {
          ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
        }
      } else {
        ESP_LOGW(TAG, "子女2绑定号码为空或未设置，播放提示音");
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "子女2绑定号码为空或未设置");
      }
    }
    break;

  // ============================================
  // 按键40: 接听/挂断/SOS拨号（核心通话控制按键）
  // ============================================
  case BUTTON_GPIO_40:
    ESP_LOGI(TAG, "按键40功能：接听来电或SOS拨号");

    long_press_count_gpio40 = 0;

    // --------------------------------------------------
    // 情况1: SIP来电中 - 接听SIP通话, 挂断SIP通话
    // --------------------------------------------------
    if (is_sip_begin) {
      esp_rtc_bye(s_esp_sip);
    } else if (is_sip_incoming) {
      if (is_sip_event_incoming) {
        esp_rtc_answer(s_esp_sip);
      }
    }
    // --------------------------------------------------
    // 情况2: 4G来电响铃中 - 接听4G来电
    // --------------------------------------------------
    else if (call_manager_get_state() == CALL_STATE_RINGING) {
      ESP_LOGI(TAG, "有来电，执行接听操作");
      is_sip_mode = false;
      esp_err_t ret = call_manager_answer_call();
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "接听来电失败 - 错误: %d", ret);
      }
    }
    // --------------------------------------------------
    // 情况3: 通话中或播放中 - 【挂断电话】（核心挂断逻辑1）
    // --------------------------------------------------
    else if (g_is_in_call) {
      // 通话中则挂断
      at_hang_up_phone_call(); // 调用AT指令挂断函数
    } else if (brtc_is_playing()) {
      // 播放中则暂停
      ESP_LOGI(TAG, "打断/暂停事件");
      if (s_bdrtc.engine) {
          // 先强制打断当前正在播放的 TTS
          baidu_chat_agent_engine_interrupt(s_bdrtc.engine);
          vTaskDelay(pdMS_TO_TICKS(100)); // 稍作延时确保引擎清空队列
          // 反馈打断成功
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "好的，马上闭嘴");
      }
    }
    // --------------------------------------------------
    // 情况4: 未通话状态 - SOS一键拨号
    // --------------------------------------------------
    else {
      // 未通话则sos呼叫
      const char *sos_number = get_phone_number_by_key(2); // 2表示SOS
      if (sos_number != NULL && strlen(sos_number) > 0) {
        ESP_LOGI(TAG, "SOS绑定号码: %s", sos_number);
        // 使用封装的AT指令拨打电话函数
        esp_err_t call_ret = at_make_phone_call(sos_number);
        if (call_ret != ESP_OK) {
          ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
        }
      } else {
        ESP_LOGW(TAG, "SOS绑定号码为空或未设置");
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "SOS绑定号码为空或未设置");
      }
    }
    // 检查WiFi连接状态
    if (gl_sta_connected == false && gl_sta_is_connecting == false) {
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                               "WiFi未连接，请配网...");
      example_wifi_connect();
    }
    break;
  // ============================================
  // 按键4: 拒接/唤醒/关机
  // ============================================
  case BUTTON_GPIO_4:
    ESP_LOGI(TAG, "按键4功能：拒接来电或唤醒设备");

    // --------------------------------------------------
    // 情况1: SIP响铃中 - 拒接SIP通话
    // --------------------------------------------------
    if (is_sip_event_calling || is_sip_event_incoming) {
      esp_rtc_bye(s_esp_sip);
    }
    // --------------------------------------------------
    // 情况2: 4G来电响铃中 - 【拒接来电】（核心挂断逻辑2）
    // --------------------------------------------------
    else if (call_manager_get_state() == CALL_STATE_RINGING) {
      ESP_LOGI(TAG, "有来电，执行拒接操作");
      is_sip_mode = false;
      esp_err_t ret = call_manager_reject_call(); // 调用拒接函数
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "拒接来电失败 - 错误: %d", ret);
      }
    }
    // --------------------------------------------------
    // 情况3: 其他情况 - 唤醒设备
    // --------------------------------------------------
    else {
      ESP_LOGI(TAG, "处理唤醒事件");
      // 获取唤醒事件句柄并设置唤醒事件
      EventGroupHandle_t wakeup_handle = brtc_get_wakeup_event_handle();
      if (wakeup_handle != NULL) {
        xEventGroupSetBits(wakeup_handle, BUTTON_WAKEUP_BIT);
        ESP_LOGI(TAG, "GPIO4唤醒键已触发唤醒事件");
      } else {
        ESP_LOGW(TAG, "唤醒事件句柄获取失败");
      }
    }
    break;
  default:
    break;
  }
}

// ============================================
// GPIO4长按关机处理
// ============================================

/**
 * @brief 处理GPIO4长按关机事件
 * @note  长按2秒触发关机，播放关机提示音后进入深度睡眠
 */
static void handle_gpio4_long_press(void) {
  ESP_LOGI(TAG, "GPIO4长按2秒，准备关机");

  is_sip_mode = true;

  // 发送关机离线报文
  extern bool send_heartbeat_packet(const char *status);
  send_heartbeat_packet("off");

  // 播放关机提示音
  shutdown_V();

  // 等待提示音播放完成（2.5秒）
  vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_PROMPT_DURATION_MS));
  is_sip_mode = false;

  ESP_LOGI(TAG, "执行关机操作");
  gpio_set_level(GPIO_NUM_48, 0); // 先关闭 PA，防止断电瞬间产生爆音
  vTaskDelay(pdMS_TO_TICKS(100)); // 延时等待放电
  esp_deep_sleep_start();
}

#include "wifi_provisioning/manager.h"
#include "esp_wifi.h"
extern bool check_wifi_config_saved(void);

/**
 * @brief 处理GPIO4双击事件
 * @note  双击触发重启配网，播放提示音后重启设备
 */
static void handle_gpio4_double_click(void) {
  ESP_LOGI(TAG, "GPIO4双击，准备重启配网");
  
  if (check_wifi_config_saved() == false) {
    ESP_LOGI(TAG, "当前未配网，直接重启以重新进入配网模式");
  } else {
    ESP_LOGI(TAG, "当前已配网，清除配置并重启");
    esp_wifi_restore();
    wifi_prov_mgr_reset_provisioning();
  }
  
  // 停止云端TTS（因为网络配置已清除或即将重启，云端TTS会失败）
  if (s_bdrtc.engine) {
      baidu_chat_agent_engine_interrupt(s_bdrtc.engine);
  }
  
  // 使用本地离线语音播放“等待配网”提示音，该函数会阻塞直到播放完毕
  ESP_LOGI(TAG, "播放本地配网提示音");
  audio_prompt_play("file:///spiffs/wifi_wait.mp3");
  
  // 稍微延时以确保音频缓冲区完全清空
  vTaskDelay(pdMS_TO_TICKS(500));
  
  // 重启前先关闭PA，防止爆音
  gpio_set_level(GPIO_NUM_48, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // 发送关机离线报文
  extern bool send_heartbeat_packet(const char *status);
  send_heartbeat_packet("off");
  
  esp_restart();
}

// ============================================
// 按键扫描任务
// ============================================

/**
 * @brief 按键扫描任务
 * @param pvParameters 任务参数
 * @note
 *   1. 轮询方式扫描所有按键状态
 *   2. 消抖处理（50ms）
 *   3. 检测短按和长按（仅GPIO4支持长按）
 *   4. 根据播放状态动态调整扫描频率
 */
static void button_scan_task(void *pvParameters) {
  uint64_t current_time;
  uint8_t current_state;

  while (1) {
    for (int i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
      current_time = esp_timer_get_time() / 1000; // 转换为毫秒
      current_state = gpio_get_level(buttons[i].gpio_num);

      // 检查状态变化
      if (current_state != buttons[i].last_state) {
        // 检查消抖时间
        if ((current_time - buttons[i].last_change_time) > DEBOUNCE_TIME_MS) {
          buttons[i].last_state = current_state;
          buttons[i].last_change_time = current_time;

          if (current_state == BUTTON_PRESSED) {
            // 按键按下
            ESP_LOGI(TAG, "%s 被按下", buttons[i].name);
            buttons[i].press_start_time = current_time;
            buttons[i].long_press_triggered = false;
          } else {
            // 按键释放
            ESP_LOGI(TAG, "%s 被释放", buttons[i].name);

            // 如果没有触发过长按，则处理为短按
            if (!buttons[i].long_press_triggered) {
              if (buttons[i].gpio_num == BUTTON_GPIO_4 && (current_time - buttons[i].last_short_press_time) < 500) {
                // 500ms内的第二次短按，视为双击
                ESP_LOGI(TAG, "检测到 %s 双击", buttons[i].name);
                button_event_t evt = {.type = BUTTON_EVENT_DOUBLE_CLICK, .gpio = buttons[i].gpio_num};
                if (button_event_queue) xQueueSend(button_event_queue, &evt, 0);
                buttons[i].last_short_press_time = 0; // 重置双击计时
              } else {
                buttons[i].last_short_press_time = current_time;
                button_event_t evt = {.type = BUTTON_EVENT_SHORT_PRESS, .gpio = buttons[i].gpio_num};
                if (button_event_queue) xQueueSend(button_event_queue, &evt, 0);
              }
            }
          }
        }
      }

      // 检测长按（只在GPIO4上实现长按关机功能）
      if (buttons[i].gpio_num == BUTTON_GPIO_4 &&
          current_state == BUTTON_PRESSED && !buttons[i].long_press_triggered) {
        uint64_t press_duration = current_time - buttons[i].press_start_time;
        if (press_duration >= LONG_PRESS_TIME_MS) {
          buttons[i].long_press_triggered = true;
          button_event_t evt = {.type = BUTTON_EVENT_LONG_PRESS, .gpio = BUTTON_GPIO_4};
          if (button_event_queue) xQueueSend(button_event_queue, &evt, 0);
        }
      }

      // 检测长按（只在GPIO40上实现长按配网功能） - 已移除，改为GPIO4双击
    }

    // 根据播放状态调整扫描频率
    if (brtc_is_playing()) {
      vTaskDelay(pdMS_TO_TICKS(10)); // 播放中时10ms扫描一次
    } else {
      vTaskDelay(pdMS_TO_TICKS(50)); // 播放停止时50ms扫描一次，降低CPU占用
    }
  }
}

// ============================================
// 按键事件处理任务
// ============================================

/**
 * @brief 按键事件处理任务
 * @param pvParameters 任务参数
 * @note  从队列获取按键事件并执行相应操作
 */
static void button_event_task(void *pvParameters) {
  button_event_t event;

  while (1) {
    if (xQueueReceive(button_event_queue, &event, portMAX_DELAY) == pdPASS) {
      if (event.type == BUTTON_EVENT_SHORT_PRESS) {
        handle_button_short_press(event.gpio);
      } else if (event.type == BUTTON_EVENT_LONG_PRESS) {
        if (event.gpio == BUTTON_GPIO_4) {
          handle_gpio4_long_press();
        }
      } else if (event.type == BUTTON_EVENT_DOUBLE_CLICK) {
        if (event.gpio == BUTTON_GPIO_4) {
          handle_gpio4_double_click();
        }
      }
    }
  }
}

// ============================================
// 按键处理初始化
// ============================================

/**
 * @brief 初始化按键处理
 * @return ESP_OK 成功，其他值表示失败
 * @note
 *   1. 配置GPIO为上拉输入模式
 *   2. 创建按键事件队列
 *   3. 创建按键扫描任务（轮询方式）
 *   4. 创建按键事件处理任务
 */
esp_err_t button_handler_init(void) {
  ESP_LOGI(TAG, "初始化按键处理");

  // 配置GPIO引脚
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BUTTON_GPIO_38) | (1ULL << BUTTON_GPIO_39) |
                      (1ULL << BUTTON_GPIO_40) | (1ULL << BUTTON_GPIO_4),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE, // 启用上拉电阻
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE // 不使用中断，使用轮询方式
  };

  esp_err_t ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GPIO配置失败: %s", esp_err_to_name(ret));
    return ret;
  }

  // 创建按键事件队列
  button_event_queue = xQueueCreate(10, sizeof(button_event_t));
  if (!button_event_queue) {
    ESP_LOGE(TAG, "创建按键事件队列失败");
    return ESP_FAIL;
  }

  // 创建按键扫描任务
  BaseType_t task_ret =
      xTaskCreatePinnedToCoreWithCaps(button_scan_task, "button_scan_task",
                  4 * 1024, // 任务堆栈大小 - 增加至4KB避免ESP_LOG引发栈溢出
                  NULL,     // 任务参数
                  12,       // 任务优先级
                  &button_scan_task_handle, 1, MALLOC_CAP_SPIRAM);

  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "创建按键扫描任务失败");
    vQueueDelete(button_event_queue);
    button_event_queue = NULL;
    return ESP_FAIL;
  }

  // 创建按键事件处理任务
  task_ret = xTaskCreatePinnedToCoreWithCaps(button_event_task, "button_event_task",
                         8 * 1024, // 任务堆栈大小 - 增至8KB处理TTS/AT等复杂操作
                         NULL,     // 任务参数
                         12,       // 任务优先级，与扫描任务同级或低级
                         &button_event_task_handle, 1, MALLOC_CAP_SPIRAM);

  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "创建按键事件处理任务失败");
    vTaskDelete(button_scan_task_handle);
    button_scan_task_handle = NULL;
    vQueueDelete(button_event_queue);
    button_event_queue = NULL;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "按键处理初始化完成");
  ESP_LOGI(TAG, "按键配置信息:");
  ESP_LOGI(TAG, "GPIO38 - 音量增加/子女1拨号");
  ESP_LOGI(TAG, "GPIO39 - 音量减少/子女2拨号");
  ESP_LOGI(TAG, "GPIO40 - 接听/挂断/SOS拨号");
  ESP_LOGI(TAG, "GPIO4  - 短按:拒接/唤醒, 长按2秒:关机");
  return ESP_OK;
}

// ============================================
// AT指令拨打电话
// ============================================

/**
 * @brief 使用AT指令拨打电话
 * @param phone_number 要拨打的电话号码
 * @return ESP_OK 成功，其他值表示失败
 * @note
 *   1. 设置GPIO47和GPIO48为高电平（通话状态指示）
 *   2. 设置通话状态标志 g_is_in_call = true
 *   3. 发送 +++ 退出命令模式
 *   4. 发送 ATD 指令拨打电话
 */
esp_err_t at_make_phone_call(const char *phone_number) {
  if (phone_number == NULL || strlen(phone_number) == 0) {
    ESP_LOGE(TAG, "电话号码为空");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "开始拨打电话: %s", phone_number);

  // 使用AT指令拨打电话
  if (g_at_command_buffer != NULL && g_at_response_buffer != NULL &&
      g_at_handle != NULL) {
    // 切换到 4G 音频输出通道并开启 PA
    // 必须先关闭 PA，防止切换瞬间产生巨大的瞬态电流
    gpio_set_level(GPIO_NUM_48, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    gpio_set_level(GPIO_NUM_47, 1); // 1 = 4G输出
    vTaskDelay(pdMS_TO_TICKS(50));  // 等待通道切换稳定
    
    gpio_set_level(GPIO_NUM_48, 1); // 重新开启PA

    // 设置电话状态为通话中
    g_is_in_call = true;
    ESP_LOGI(TAG, "设置电话状态为通话中");

    // 设置播放状态为真
    // brtc_set_playing_state(true);
    // ESP_LOGI(TAG, "设置播放状态为真");

    if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
        // 先发送+++退出命令模式
        memset(g_at_command_buffer, 0, 256);
        memset(g_at_response_buffer, 0, 1024);
        strcpy(g_at_command_buffer, "+++");

        esp_err_t at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer,
                                                  g_at_response_buffer, 1024);
        ESP_LOGI(TAG, "+++指令发送");
    ESP_LOGI(TAG, "+++指令响应: %s", g_at_response_buffer);

    // 等待一小段时间
    vTaskDelay(pdMS_TO_TICKS(500));

    // 发送ATD拨号指令
    memset(g_at_command_buffer, 0, 256);
    memset(g_at_response_buffer, 0, 1024);
    snprintf(g_at_command_buffer, 256, "ATD%s;", phone_number);

    at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer,
                                    g_at_response_buffer, 1024);
    if (at_ret == ESP_OK) {
      ESP_LOGI(TAG, "ATD拨号指令发送成功");
      ESP_LOGI(TAG, "ATD指令响应: %s", g_at_response_buffer);
      xSemaphoreGive(g_at_mutex);
      return ESP_OK;
    } else {
      ESP_LOGE(TAG, "ATD拨号指令发送失败 - 错误: %d", at_ret);
      xSemaphoreGive(g_at_mutex);
      return at_ret;
    }
  } else {
      ESP_LOGE(TAG, "无法获取AT Mutex");
      return ESP_FAIL;
  }
  } else {
    ESP_LOGE(TAG, "AT指令缓冲区、句柄未初始化");
    return ESP_ERR_INVALID_STATE;
  }
}

// ============================================
// 统一挂断清理函数
// ============================================

/**
 * @brief 统一挂断清理函数
 * @note 用于主动挂断、对方挂断（NO CARRIER）等所有挂断场景的统一处理
 *       确保GPIO和状态标志的一致性
 */
void hang_up_cleanup(void) {
  // 先关闭功放 PA，防止切换通道时产生脉冲拉低电源导致重启
  gpio_set_level(GPIO_NUM_48, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // 切换回 DAC 音频输出通道
  gpio_set_level(GPIO_NUM_47, 0); // 0 = DAC输出
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // 重新打开功放 PA，恢复系统声音
  gpio_set_level(GPIO_NUM_48, 1);
  ESP_LOGI(TAG, "已安全切回DAC通道并恢复PA供电");

  // 设置电话状态为非通话中
  g_is_in_call = false;
  ESP_LOGI(TAG, "设置电话状态为非通话中");
}

// ============================================
// AT指令挂断电话 - 核心挂断逻辑
// ============================================

/**
 * @brief 使用AT指令挂断电话
 * @return ESP_OK 成功，其他值表示失败
 * @note
 *   【核心挂断函数】此函数是按键挂断电话的最终执行函数
 *   执行流程：
 *   1. 执行统一挂断清理（GPIO47设为低电平，状态设为非通话中）
 *   2. 发送 ATH AT指令挂断电话
 *
 *   调用路径：
 *   - 按键40短按（通话中）→ at_hang_up_phone_call()
 *   - 其他模块主动挂断 → at_hang_up_phone_call()
 */
esp_err_t at_hang_up_phone_call(void) {
  ESP_LOGI(TAG, "开始挂断电话");

  // 使用AT指令挂断电话
  if (g_at_command_buffer != NULL && g_at_response_buffer != NULL &&
      g_at_handle != NULL) {
    esp_err_t at_ret;

    // --------------------------------------------------
    // 步骤1: 执行统一挂断清理
    // --------------------------------------------------
    hang_up_cleanup();

    // --------------------------------------------------
    // 步骤2: 发送ATH挂断指令
    // ATH = Hang up（挂断）
    // --------------------------------------------------
    if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
        memset(g_at_command_buffer, 0, 256);
        memset(g_at_response_buffer, 0, 1024);
        strcpy(g_at_command_buffer, "ATH");

        at_ret = at_send_custom_command(g_at_handle, g_at_command_buffer,
                                        g_at_response_buffer, 1024);
        xSemaphoreGive(g_at_mutex);
    if (at_ret == ESP_OK) {
      ESP_LOGI(TAG, "ATH挂断指令发送成功");
      ESP_LOGI(TAG, "ATH指令响应: %s", g_at_response_buffer);

      return ESP_OK;
    } else {
      return at_ret;
    }
  } else {
      ESP_LOGE(TAG, "无法获取AT Mutex");
      return ESP_FAIL;
  }
  } else {
    ESP_LOGE(TAG, "AT指令缓冲区、句柄未初始化");
    return ESP_ERR_INVALID_STATE;
  }
}

// ============================================
// 按键处理反初始化
// ============================================

/**
 * @brief 反初始化按键处理
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t button_handler_deinit(void) {
  ESP_LOGI(TAG, "反初始化按键处理");

  // 删除按键扫描任务
  if (button_scan_task_handle != NULL) {
    vTaskDelete(button_scan_task_handle);
    button_scan_task_handle = NULL;
  }

  // 删除按键事件处理任务
  if (button_event_task_handle != NULL) {
    vTaskDelete(button_event_task_handle);
    button_event_task_handle = NULL;
  }

  // 删除按键事件队列
  if (button_event_queue != NULL) {
    vQueueDelete(button_event_queue);
    button_event_queue = NULL;
  }

  return ESP_OK;
}
