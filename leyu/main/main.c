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

#include <inttypes.h> // 添加inttypes.h头文件以支持PRIu32宏
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "protocol_examples_utils.h"

#include "esp_gmf_oal_mem.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_thread.h"

#include "blufi_example.h"
#include "brtc_app.h"
#include "button_handler.h"

#include "call_manager.h" // 添加来电管理器头文件
#include "contact_manager.h"
#include "mqtt5_client.h"
#include "mqtt_example.h"
#include "network_manager.h"
#include "ota.h"
#include "system_monitor.h"         // 添加系统监控模块头文件
#include "uart_comm.h"              // 添加UART通信组件头文件
#include "uart_echo_example_main.h" // 添加UART通信模块头文件

#include "../components/modem_at/priv_include/modem_at_internal.h" // 添加内部头文件以使用AT指令相关函数
#include "at_3gpp_ts_27_007.h" // 添加AT指令相关头文件
#include "usb_rndis_4g_module.h"

#include "audio_processor.h" // 添加音频处理器头文件，以使用audio_prompt_play函数
#include "uart_command.h"

#include "battery_module.h"
#include "led_control.h"
#include "sip_service.h"
#include "spinlock.h"

extern esp_rtc_handle_t s_esp_sip;
extern bool is_service_status;
extern nvs_handle_t nvs_mqtt_handle;
extern void parse_service_status(const char *json_str);

// 定义AT上下文结构体，与usb_rndis_4g_module.c中的定义一致
typedef struct {
  void *cdc_port;        /*!< CDC port handle */
  at_handle_t at_handle; /*!< AT command parser handle */
} at_ctx_t;

// 声明外部AT上下文变量
extern at_ctx_t g_at_ctx;

// 定义使用PSRAM的全局变量，一个用于发送AT指令字符串，一个用于接收响应字符串
extern char *g_at_command_buffer;  // 用于发送AT指令字符串
extern char *g_at_response_buffer; // 用于接收AT指令响应字符串

// 固件版本号
const char *firmware_version = "102005";

/* This task is only for debug purpose. */
#define ENABLE_TASK_MONITOR (0)

static const char *TAG = "main";
static EventGroupHandle_t s_startup_event_group = NULL;

#define STARTUP_EVENT_BLUFI_READY_BIT BIT0
#define MODEM_POWER_STABILIZE_MS      10000

static const char *BOOT_CTRL_NVS_NAMESPACE = "boot_ctrl";
static const char *BOOT_CTRL_SKIP_POWERON_KEY = "skip_poweron";

// 按键绑定号码存储数组，使用PSRAM空间
static char (*g_phone_numbers)[64] = NULL;

// 函数声明
esp_err_t init_phone_numbers_from_flash(void);
const char *get_phone_number_by_key(int key_index);
static void wait_for_usb_start_conditions(TickType_t modem_power_on_tick);
static void on_blufi_ready(void);
static bool consume_skip_poweron_prompt_flag(void);

static bool consume_skip_poweron_prompt_flag(void) {
  nvs_handle_t nvs_handle = 0;
  uint8_t skip_poweron = 0;
  esp_err_t ret =
      nvs_open(BOOT_CTRL_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS读取skip_poweron失败: %s", esp_err_to_name(ret));
    return false;
  }

  ret = nvs_get_u8(nvs_handle, BOOT_CTRL_SKIP_POWERON_KEY, &skip_poweron);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvs_handle);
    return false;
  }

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "读取skip_poweron标志失败: %s", esp_err_to_name(ret));
    nvs_close(nvs_handle);
    return false;
  }

  ret = nvs_erase_key(nvs_handle, BOOT_CTRL_SKIP_POWERON_KEY);
  if (ret == ESP_OK) {
    ret = nvs_commit(nvs_handle);
  }

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "清除skip_poweron标志失败: %s", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "已消费skip_poweron标志，本次启动跳过poweron提示音");
  }

  nvs_close(nvs_handle);
  return skip_poweron == 1;
}

#if (ENABLE_TASK_MONITOR)
static void monitor_task(void *pv) {
  while (1) {
    esp_gmf_oal_sys_get_real_time_stats(1000, false);
    ESP_GMF_MEM_SHOW(TAG);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
#endif /* ENABLE_TASK_MONITOR */

// BLUFI初始化任务
static void on_blufi_ready(void) {
  if (s_startup_event_group != NULL) {
    xEventGroupSetBits(s_startup_event_group, STARTUP_EVENT_BLUFI_READY_BIT);
  }
}

static void blufi_task(void *pvParameters) {
  ESP_LOGI(TAG, "启动BLUFI初始化任务");
  blufi_init();
  vTaskDelete(NULL); // 任务完成后删除自身
}

static void wait_for_usb_start_conditions(TickType_t modem_power_on_tick) {
  if (s_startup_event_group != NULL) {
    EventBits_t bits = xEventGroupWaitBits(
        s_startup_event_group, STARTUP_EVENT_BLUFI_READY_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));
    if (bits & STARTUP_EVENT_BLUFI_READY_BIT) {
      ESP_LOGI(TAG, "BLUFI初始化完成，继续启动USB 4G模块");
    } else {
      ESP_LOGW(TAG, "等待BLUFI初始化超时，继续检查4G模块启动条件");
    }
  }

  TickType_t elapsed_ticks = xTaskGetTickCount() - modem_power_on_tick;
  TickType_t stable_ticks = pdMS_TO_TICKS(MODEM_POWER_STABILIZE_MS);
  if (elapsed_ticks < stable_ticks) {
    TickType_t remaining_ticks = stable_ticks - elapsed_ticks;
    ESP_LOGI(TAG, "等待4G模块上电稳定: %lu ms",
             (unsigned long)pdTICKS_TO_MS(remaining_ticks));
    vTaskDelay(remaining_ticks);
  } else {
    ESP_LOGI(TAG, "4G模块上电稳定窗口已满足");
  }
}

static esp_err_t spiffs_init(void) {
  esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                .partition_label = "spiffs_data",
                                .max_files = 5,
                                .format_if_mount_failed = false};

  // 使用上面定义的设置来初始化和挂载SPIFFS文件系统
  // 注意: esp_vfs_spiffs_register 是一个多合一的便利函数
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS注册失败，错误: %d", ret);
    // return ret;
  }
  return ESP_OK;
}

// MQTT初始化任务
static void mqtt_init_task(void *pvParameters) {
  // ESP32-S3有8MB PSRAM，内存充足，无需死等蓝牙资源释放即可启动MQTT
  // 这允许设备在未配网（蓝牙广播中）但4G已连上的情况下正常上线
  //  启动MQTT5客户端
  mqtt_app_init();

  // 任务完成后删除自身
  vTaskDelete(NULL);
}

// USB模块初始化任务包装函数
static void usb_module_task(void *pvParameters) {
  // 调用实际的USB模块初始化函数
  usb_module_app_init();
  // 任务完成后删除自身
  vTaskDelete(NULL);
}

#include "console.h"
#include "media_lib_adapter.h"

extern struct baidu_rtc_t s_bdrtc;
extern esp_err_t brtc_create(void);

// 重连状态标志，用于防止重复触发重连逻辑
static bool is_reconnecting = false;
// 重连失败计数
static int reconnect_fail_count = 0;
// 最大重连失败次数
#define MAX_RECONNECT_FAILS 3

void app_main() {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  media_lib_add_default_adapter();
  // console_init();

  // 初始化GPIO1、GPIO48和GPIO2为高电平，临时调试用，后面要根据扬声器控制开关以省电
  gpio_config_t io_conf = {.pin_bit_mask =
                               (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_5) |
                               (1ULL << GPIO_NUM_48) | (1ULL << GPIO_NUM_47) |
                               (1ULL << GPIO_NUM_2) | (1ULL << GPIO_NUM_11),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  // 设置GPIO1、GPIO48和GPIO2为高电平，临时调试用，后面要根据扬声器控制开关以省电
  gpio_set_level(GPIO_NUM_1, 1);
  gpio_set_level(GPIO_NUM_5, 0); // 先关闭LED电源/使能，防止开机时数据线浮空导致乱闪
  gpio_set_level(GPIO_NUM_48, 0); // 先关闭PA，防止初始化爆音
  gpio_set_level(GPIO_NUM_47, 0);
  gpio_set_level(GPIO_NUM_2, 1);
  gpio_set_level(GPIO_NUM_11, 1);
  ESP_LOGI(TAG, "初始化GPIO完成，已强制关闭功放PA");

  // 配置 4G 模块复位引脚 (GPIO_NUM_21)
  gpio_config_t rst_conf = {
      .pin_bit_mask = (1ULL << GPIO_NUM_21),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE, // 默认下拉
      .intr_type = GPIO_INTR_DISABLE
  };
  gpio_config(&rst_conf);

  // 执行 4G 模块硬件复位
  ESP_LOGI(TAG, "开始硬件复位 4G 模块...");
  gpio_set_level(GPIO_NUM_21, 1);       // 拉高复位引脚
  vTaskDelay(pdMS_TO_TICKS(500));       // 保持高电平 500ms
  gpio_set_level(GPIO_NUM_21, 0);       // 恢复低电平，模块开始正常 Boot
  ESP_LOGI(TAG, "4G 模块硬件复位完成");

  TickType_t modem_power_on_tick = xTaskGetTickCount();

  s_startup_event_group = xEventGroupCreate();
  if (s_startup_event_group == NULL) {
    ESP_LOGE(TAG, "启动事件组创建失败");
  }
  blufi_register_ready_callback(on_blufi_ready);

  // 已删除板级SPIFFS初始化配置，避免SD卡初始化失败问题
  ESP_LOGI(TAG, "已禁用板级SPIFFS设备初始化，避免与SD卡冲突");

  led_init();

  led_display(0);
  vTaskDelay(pdMS_TO_TICKS(10)); // 等待RMT将全黑数据发送完毕
  gpio_set_level(GPIO_NUM_5, 1); // 此时数据线已稳定，可以安全开启LED电源/使能

  TimerHandle_t periodic_timer = xTimerCreate(
      "11", pdMS_TO_TICKS(0.25 * 1000), pdTRUE, (void *)1, timer_callback);
  xTimerStart(periodic_timer, 0);

  // 初始化SPIFFS文件系统
  ret = spiffs_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS初始化失败，错误: %d", ret);
    // 不返回值，只是记录错误并继续执行
  }

  // 打印初始内存状态
  ESP_LOGI(TAG, "初始可用堆内存: %" PRIu32 " 字节",
           (uint32_t)esp_get_free_heap_size());

  bool skip_poweron_prompt = consume_skip_poweron_prompt_flag();

  audio_pipe_open();
  audio_prompt_config_t prompt_cfg = DEFAULT_AUDIO_PROMPT_CONFIG();
  audio_prompt_open(&prompt_cfg);
  
  // 音频DAC初始化完毕后，延时100ms等待底噪偏置电压稳定
  vTaskDelay(pdMS_TO_TICKS(100));
  // 安全开启功放PA，避免爆音
  gpio_set_level(GPIO_NUM_48, 1);
  ESP_LOGI(TAG, "DAC初始化完毕，已开启功放PA");
  // audio_prompt_play("file:///spiffs/wifi_wait.mp3");
  if (skip_poweron_prompt) {
    ESP_LOGI(TAG, "检测到RNDIS超时重启标志，跳过poweron提示音");
  } else {
    audio_prompt_play("file:///spiffs/poweron.mp3");
  }
  // vTaskDelay(pdMS_TO_TICKS(6000));

  battery_module_init(); // 初始化电池管理模块
  // 初始化系统监控模块
  ESP_LOGI(TAG, "初始化系统监控模块");
  esp_err_t monitor_ret = system_monitor_init();
  if (monitor_ret != ESP_OK) {
    ESP_LOGE(TAG, "系统监控模块初始化失败: %s", esp_err_to_name(monitor_ret));
  } else {
    // 启动系统监控任务
    monitor_ret = system_monitor_task_start();
    if (monitor_ret != ESP_OK) {
      ESP_LOGE(TAG, "启动系统监控任务失败: %s", esp_err_to_name(monitor_ret));
    }
  }

  // 初始化按键处理
  ESP_LOGI(TAG, "初始化按键处理");
  esp_err_t button_ret = button_handler_init();
  if (button_ret != ESP_OK) {
    ESP_LOGE(TAG, "按键处理初始化失败: %s", esp_err_to_name(button_ret));
  }

  uart_command_app_init();
  // SIP事件处理任务，加大栈空间防止偶发栈溢出
  xTaskCreatePinnedToCoreWithCaps(sip_event_handler_task,
                                  "sip_event_handler_task", 12 * 1024, NULL, 4,
                                  NULL, 1, MALLOC_CAP_SPIRAM);

  // 暂时不创建BLUFI任务和连接WiFi 创建并启动BLUFI初始化任务
  if (xTaskCreatePinnedToCoreWithCaps(blufi_task, "blufi_task", 6 * 1024, NULL,
                                      15, NULL, 1,
                                      MALLOC_CAP_SPIRAM) == pdPASS) {
    ESP_LOGI(TAG, "BLUFI任务创建成功");
  } else {
    ESP_LOGE(TAG, "创建BLUFI任务失败，内存不足");
  }

  wait_for_usb_start_conditions(modem_power_on_tick);

  // 创建并启动usb_rndis_4g_module初始化任务
  if (xTaskCreatePinnedToCoreWithCaps(usb_module_task, "usb_module_task",
                                      6 * 1024, NULL, 18, NULL, 1,
                                      MALLOC_CAP_SPIRAM) == pdPASS) {
    ESP_LOGI(TAG, "usb网卡初始化任务创建成功");
  } else {
    ESP_LOGE(TAG, "创建usb网卡初始化任务失败，内存不足");
  }

  // 先等待配网完成、连接WiFi或4G网络连接成功
  ESP_LOGI(TAG, "等待网络(WiFi/4G)连接...");
  esp_err_t wifi_ret = wait_wifi_connect();
  if (wifi_ret != ESP_OK) {
      ESP_LOGW(TAG, "网络连接超时，将进入离线模式或等待后续重连");
  } else {
      ESP_LOGI(TAG, "网络已连接");
  }

  // 等待4G模块初始化完成
  // vTaskDelay(pdMS_TO_TICKS(2 * 1000));

  // 初始化网络管理器
  ESP_LOGI(TAG, "初始化网络管理器");
  esp_err_t net_mgr_ret = network_manager_init();
  if (net_mgr_ret != ESP_OK) {
    ESP_LOGE(TAG, "网络管理器初始化失败: %s", esp_err_to_name(net_mgr_ret));
  }

  // 初始化联系人管理器，从Flash加载通讯录到PSRAM
  ESP_LOGI(TAG, "初始化联系人管理器，从Flash加载通讯录");
  esp_err_t contact_ret = contact_manager_load_from_flash();
  if (contact_ret != ESP_OK) {
    ESP_LOGE(TAG, "联系人管理器初始化失败: %s", esp_err_to_name(contact_ret));
  }

  // 初始化来电管理器
  ESP_LOGI(TAG, "初始化来电管理器");
  esp_err_t call_ret = call_manager_init();
  if (call_ret != ESP_OK) {
    ESP_LOGE(TAG, "来电管理器初始化失败: %s", esp_err_to_name(call_ret));
  }
  /* else {
      // 测试联系人管理器功能
      ESP_LOGI(TAG, "测试联系人管理器功能...");
      char phone[MAX_PHONE_LENGTH];

      // 测试查找存在的联系人
      contact_ret = contact_get_phone_by_name("张三", phone);
      if (contact_ret == ESP_OK) {
          ESP_LOGI(TAG, "测试成功：张三的电话号码是 %s", phone);
      } else {
          ESP_LOGE(TAG, "测试失败：无法找到张三的电话号码");
      }

      // 测试查找不存在的联系人
      contact_ret = contact_get_phone_by_name("不存在的人", phone);
      if (contact_ret == ESP_ERR_NOT_FOUND) {
          ESP_LOGI(TAG, "测试成功：正确处理不存在的联系人");
      } else {
          ESP_LOGE(TAG, "测试失败：未正确处理不存在的联系人");
      }

      // 测试拨号功能（仅演示，不实际拨打电话）
      ESP_LOGI(TAG, "测试拨号功能（仅演示）...");
      contact_ret = dial_phone_by_contact_name("李四");
      if (contact_ret == ESP_OK) {
          ESP_LOGI(TAG, "拨号功能测试成功：已向李四发送拨号指令");
      } else if (contact_ret == ESP_ERR_INVALID_STATE) {
          ESP_LOGI(TAG, "拨号功能测试预期结果：AT句柄未初始化，这是正常的");
      } else {
          ESP_LOGE(TAG, "拨号功能测试失败：错误代码 %d", contact_ret);
      }
  }*/
  /*
  // 在main函数中测试AT指令通讯
  ESP_LOGI(TAG, "在main函数中测试AT指令通讯...");
  vTaskDelay(pdMS_TO_TICKS(20000));  // 延迟20秒

  if (g_at_ctx.at_handle != NULL) {

      // 使用自定义AT指令函数测试
      ESP_LOGI(TAG, "使用自定义AT指令函数测试...");

      // 使用全局变量发送AT指令
      if (g_at_command_buffer != NULL && g_at_response_buffer != NULL) {
          // 测试AT+CREG?（2G/3G/4G通用，返回LAC、CI、网络注册状态）
          memset(g_at_command_buffer, 0, 256);
          memset(g_at_response_buffer, 0, 1024);
          strcpy(g_at_command_buffer, "AT+CREG?");

          esp_err_t at_ret = at_send_custom_command(g_at_ctx.at_handle,
  g_at_command_buffer, g_at_response_buffer, 1024); if (at_ret == ESP_OK) {
              ESP_LOGI(TAG, "AT+CREG?指令测试成功");
              ESP_LOGI(TAG, "AT+CREG?指令响应: %s", g_at_response_buffer);
          } else {
              ESP_LOGE(TAG, "AT+CREG?指令测试失败 - 错误: %d", at_ret);
          }

          // 测试AT+CEREG?（4G专用，返回TAC、4G CI）
          memset(g_at_command_buffer, 0, 256);
          memset(g_at_response_buffer, 0, 1024);
          strcpy(g_at_command_buffer, "AT+CEREG?");

          at_ret = at_send_custom_command(g_at_ctx.at_handle,
  g_at_command_buffer, g_at_response_buffer, 1024); if (at_ret == ESP_OK) {
              ESP_LOGI(TAG, "AT+CEREG?指令测试成功");
              ESP_LOGI(TAG, "AT+CEREG?指令响应: %s", g_at_response_buffer);
          } else {
              ESP_LOGE(TAG, "AT+CEREG?指令测试失败 - 错误: %d", at_ret);
          }

          //
  测试AT+MUESTATS="radio"（4G扩展，返回基站频点、物理小区ID、信号强度）
          memset(g_at_command_buffer, 0, 256);
          memset(g_at_response_buffer, 0, 1024);
          strcpy(g_at_command_buffer, "AT+MUESTATS=\"radio\"");

          at_ret = at_send_custom_command(g_at_ctx.at_handle,
  g_at_command_buffer, g_at_response_buffer, 1024); if (at_ret == ESP_OK) {
              ESP_LOGI(TAG, "AT+MUESTATS=\"radio\"指令测试成功");
              ESP_LOGI(TAG, "AT+MUESTATS=\"radio\"指令响应: %s",
  g_at_response_buffer); } else { ESP_LOGE(TAG,
  "AT+MUESTATS=\"radio\"指令测试失败 - 错误: %d", at_ret);
          }
      } else {
          ESP_LOGE(TAG, "AT指令缓冲区未初始化");
      }
  } else {
      ESP_LOGE(TAG, "AT句柄未初始化，无法进行AT指令测试");
  }
  */

  // 临时返回只测前面逻辑
  // return;
  // 因为没有wait for usb_module_app_init任务完成，所以这里需要等待一段时间
  // vTaskDelay(pdMS_TO_TICKS(35000));

  // 创建UART测试任务
  /*
  ESP_LOGI(TAG, "创建UART测试任务");
  //uart_app_init();  // 注释掉，避免与uart_comm模块冲突

  // 初始化UART通信模块
  ESP_LOGI(TAG, "初始化UART通信模块");
  esp_err_t uart_ret = uart_comm_init();
  if (uart_ret != ESP_OK) {
      ESP_LOGE(TAG, "UART通信模块初始化失败: %s", esp_err_to_name(uart_ret));
  } else {
      // 启动UART通信任务
      uart_ret = uart_comm_task_start();
      if (uart_ret != ESP_OK) {
          ESP_LOGE(TAG, "启动UART通信任务失败: %s", esp_err_to_name(uart_ret));
      }
  }
*/

  // ESP_ERROR_CHECK(nvs_flash_init());

  // ESP_ERROR_CHECK(esp_netif_init());
  // ESP_ERROR_CHECK(esp_event_loop_create_default());
  //  注释掉原来的立即连接WiFi，改为等待蓝牙配网完成后再连接
  //  ESP_ERROR_CHECK(example_connect());
  // ESP_ERROR_CHECK(wait_wifi_connect());
  // return ;//暂时调试蓝牙部分

  // 从Flash初始化按键绑定号码到PSRAM
  ESP_LOGI(TAG, "从Flash初始化按键绑定号码到PSRAM");
  esp_err_t phone_ret = init_phone_numbers_from_flash();
  if (phone_ret != ESP_OK) {
    ESP_LOGE(TAG, "按键绑定号码初始化失败: %s", esp_err_to_name(phone_ret));
  }

  // 等待有效的外网连接，避免提前启动导致超时重试
  ESP_LOGI(TAG, "等待有效的外网连接以启动云服务...");
  while (test_network_connectivity("[Active]") != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "外网连接已就绪，继续启动网络服务");

  // 启动ota任务
  ESP_LOGI(TAG, "启动OTA任务前可用内存: %" PRIu32 " 字节",
           (uint32_t)esp_get_free_heap_size());
  ota_app_init();
  ESP_LOGI(TAG, "启动OTA任务后可用内存: %" PRIu32 " 字节",
           (uint32_t)esp_get_free_heap_size());

  // init baidu rtc engine
  ESP_LOGI(TAG, "初始化百度RTC引擎前可用内存: %" PRIu32 " 字节",
           (uint32_t)esp_get_free_heap_size());
  brtc_init();
  ESP_LOGI(TAG, "初始化百度RTC引擎后可用内存: %" PRIu32 " 字节",
           (uint32_t)esp_get_free_heap_size());
  // 临时返回只测前面逻辑
  // return;

#if (ENABLE_TASK_MONITOR)
  esp_gmf_oal_thread_create(NULL, "monitor_task", monitor_task, NULL, 2596, 10,
                            true, 0);
#endif /* ENABLE_TASK_MONITOR */

  // 创建MQTT初始化任务，延迟启动MQTT客户端
  BaseType_t mqtt_ret =
      xTaskCreate(mqtt_init_task, "mqtt_init_task", 4 * 1024, NULL, 4, NULL);
  if (mqtt_ret != pdPASS) {
    ESP_LOGE(TAG, "创建MQTT任务失败，内存不足");
  } else {
    ESP_LOGI(TAG, "MQTT任务创建成功");
  }

  // vTaskDelay(pdMS_TO_TICKS(5 * 1000));
  uint16_t time_count = 0;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(500));
    // if (++time_count >= 100) {
    //   time_count = 0;
    //   if (is_service_status) {
    //     nvs_open("mqtt_data", NVS_READWRITE, &nvs_mqtt_handle);
    //     nvs_set_str(nvs_mqtt_handle, "SrvStat",
    //                 "{\"timeStamp\":\"0\",\"serial\":\"0\",\"imei\":\"0\",
    //             \"body\":{\"action\":\"off\"}}");
    //     nvs_commit(nvs_mqtt_handle);
    //     nvs_close(nvs_mqtt_handle);
    //   } else {
    //     nvs_open("mqtt_data", NVS_READWRITE, &nvs_mqtt_handle);
    //     nvs_set_str(nvs_mqtt_handle, "SrvStat",
    //                 "{\"timeStamp\":\"0\",\"serial\":\"0\",\"imei\":\"0\",
    //             \"body\":{\"action\":\"on\"}}");
    //     nvs_commit(nvs_mqtt_handle);
    //     nvs_close(nvs_mqtt_handle);
    //   }
    //   nvs_open("mqtt_data", NVS_READONLY, &nvs_mqtt_handle);
    //   esp_err_t ret = ESP_FAIL;
    //   char service_status[256] = {0};
    //   size_t service_status_len = sizeof(service_status);
    //   ret = nvs_get_str(nvs_mqtt_handle, "SrvStat", service_status,
    //                     &service_status_len);
    //   if (ret == ESP_OK) {
    //     ESP_LOGI(TAG, "从NVS读取服务状态: %s", service_status);
    //     parse_service_status(service_status);
    //   } else {
    //     ESP_LOGE(TAG, "从NVS读取服务状态失败，错误代码: %d", ret);
    //   }
    // }
    while (is_sip_flag) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (!is_sip_mode && !brtc_is_playing()) {
        is_sip_mode = true;
        esp_rtc_call(s_esp_sip, "10028");
      }
    }
    while (is_sip_event_calling) {
      audio_playback_play("file:///spiffs/dudu.mp3");
      for (int i = 0; i < 5 && is_sip_event_calling; i++) {
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));
      }
    }
    while (is_sip_event_incoming) {
      audio_playback_play("file:///spiffs/ring.mp3");
      for (int i = 0; i < 30 && is_sip_event_incoming; i++) {
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));
      }
    }

    if (s_bdrtc.is_connected == false && is_service_status == true &&
        !is_reconnecting && !s_bdrtc.is_destroying) {
      ESP_LOGI(TAG, "百度RTC引擎断开连接，尝试连接...");
      is_reconnecting = true;

      // 销毁旧的RTC引擎
      brtc_deinit();

      // 重新创建RTC连接
      esp_err_t ret = brtc_init();
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "百度RTC引擎重连失败，错误码: %d", ret);
        // 重连失败，增加失败计数
        reconnect_fail_count++;
        ESP_LOGE(TAG, "重连失败次数: %d/%d", reconnect_fail_count,
                 MAX_RECONNECT_FAILS);

        // 检查是否达到最大失败次数
        if (reconnect_fail_count >= MAX_RECONNECT_FAILS) {
          ESP_LOGE(TAG, "重连失败次数达到上限，系统将重启...");
          vTaskDelay(pdMS_TO_TICKS(2000));
          esp_restart();
        }

        // 重连失败，重置标志，允许下次尝试
        is_reconnecting = false;
        // 短暂延时后再尝试
        vTaskDelay(pdMS_TO_TICKS(2 * 1000));
      } else {
        // 重连成功，重置失败计数
        reconnect_fail_count = 0;

        // 重连后短暂延时，确保连接稳定
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));

        // 重连完成，重置标志
        is_reconnecting = false;
        ESP_LOGI(TAG, "百度RTC引擎重连完成");
      }
    } else if (is_reconnecting) {
      // 正在重连中，跳过检查
      ESP_LOGD(TAG, "百度RTC引擎正在重连中，跳过检查");
    } else if (s_bdrtc.is_destroying) {
      // 正在销毁中，跳过检查
      ESP_LOGD(TAG, "百度RTC引擎正在销毁中，跳过检查");
    }
  }

  // 可选：启动按键测试模式
  // button_test_init();
  // ESP_LOGI(TAG, "按键测试模式已启用，请在主循环中调用button_scan_test()");
}

/**
 * @brief 从Flash初始化按键绑定号码到PSRAM
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t init_phone_numbers_from_flash(void) {
  // 如果已经分配过内存，先释放
  if (g_phone_numbers != NULL) {
    free(g_phone_numbers);
    g_phone_numbers = NULL;
  }

  // 在PSRAM中分配存储3个号码的空间
  g_phone_numbers = (char(*)[64])heap_caps_malloc(3 * 64, MALLOC_CAP_SPIRAM);
  if (g_phone_numbers == NULL) {
    ESP_LOGE(TAG, "PSRAM分配失败，尝试使用内部RAM");
    g_phone_numbers = (char(*)[64])malloc(3 * 64);
    if (g_phone_numbers == NULL) {
      ESP_LOGE(TAG, "内存分配失败");
      return ESP_ERR_NO_MEM;
    }
  }

  // 初始化所有号码为空
  memset(g_phone_numbers, 0, 3 * 64);

  // 从Flash读取三个按键绑定号码
  const char *keys[] = {"key1", "key2", "key3"};
  const char *descriptions[] = {"子女1", "子女2", "SOS"};

  for (int i = 0; i < 3; i++) {
    esp_err_t ret = read_key_from_nvs(keys[i], g_phone_numbers[i], 64);
    if (ret == ESP_OK && strlen(g_phone_numbers[i]) > 0) {
      ESP_LOGI(TAG, "%s绑定号码: %s", descriptions[i], g_phone_numbers[i]);
    } else {
      ESP_LOGW(TAG, "%s绑定号码为空或读取失败", descriptions[i]);
    }
  }

  return ESP_OK;
}

/**
 * @brief 根据按键索引获取电话号码
 * @param key_index 按键索引 (0: 子女1, 1: 子女2, 2: SOS)
 * @return 电话号码字符串，如果不存在则返回NULL
 */
const char *get_phone_number_by_key(int key_index) {
  if (g_phone_numbers == NULL || key_index < 0 || key_index >= 3) {
    return NULL;
  }

  if (strlen(g_phone_numbers[key_index]) == 0) {
    return NULL;
  }

  return g_phone_numbers[key_index];
}

extern void play_dev_stop_aud_task(void *pvParameters);
extern nvs_handle_t nvs_mqtt_handle;
/**
 * @brief 根据服务状态控制设备状态（停机时，关闭sip服务、百度AI引擎）
 * @param action 服务状态（true：开启服务，false：关闭服务）
 */
void control_device_by_service_status(bool action) {
  if (action) {
    // 重启设备，启动sip服务、百度AI引擎
    ESP_LOGI(TAG, "重启设备，启动sip服务、百度AI引擎");
    audio_playback_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    audio_playback_play("file:///spiffs/dev_start_svr.mp3");
    vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    esp_restart();
  } else {
    // 关闭服务，关闭sip服务、百度AI引擎
    ESP_LOGI(TAG, "关闭服务，关闭sip服务、百度AI引擎");
    sip_service_stop(s_esp_sip);
    brtc_deinit();
    xTaskCreatePinnedToCoreWithCaps(play_dev_stop_aud_task,
                                    "play_dev_stop_aud_task", 4 * 1024, NULL, 4,
                                    NULL, 1, MALLOC_CAP_SPIRAM);
  }
}

/**
 * @brief 循环播放停机音频任务
 * @param 无
 */
void play_dev_stop_aud_task(void *pvParameters) {
  while (is_service_status == false) {
    esp_codec_dev_handle_t play_dev = get_play_dev_handle();
    esp_codec_dev_set_out_vol(play_dev, 80);
    audio_playback_play("file:///spiffs/dev_stop_svr.mp3");
    vTaskDelay(pdMS_TO_TICKS(5 * 1000));
  }
  
  vTaskDelete(NULL);
}
