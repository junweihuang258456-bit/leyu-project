/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usb_rndis_4g_module.h"
#include "app_wifi.h"
#include "audio_processor.h"
#include "brtc_app.h"
#include "at_3gpp_ts_27_007.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_rndis.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include <stdio.h>
#include "call_manager.h"
#include "network_manager.h"

#include "driver/gpio.h"   // GPIO驱动头文件
#include "esp_heap_caps.h" // 添加PSRAM内存分配头文件

static const char *TAG = "RNDIS_4G_MODULE";

// 声明外部函数 - 用于挂断电话时清理状态
extern void button_handler_set_call_state(bool in_call);
extern bool button_handler_is_in_call(void);
extern void hang_up_cleanup(void);

// 前向声明AT响应处理回调函数
static bool at_handle_line(at_handle_t at_handle, const char *line);

/**
 * @brief 处理非请求AT响应（如 NO CARRIER）
 * @param at_handle AT解析器句柄
 * @param response 响应字符串
 * @return true 如果响应被处理
 * @note 当对方挂断电话时，会收到 NO CARRIER 响应
 */
static bool at_handle_unsolicited_response(at_handle_t at_handle, const char *response)
{
    ESP_LOGI(TAG, "收到非请求响应: %s", response);
    
    // 处理 NO CARRIER - 对方挂断电话
    if (strstr(response, "NO CARRIER")) {
        ESP_LOGI(TAG, "收到 NO CARRIER - 对方已挂断电话");
        
        // 执行统一挂断清理（无论当前状态如何，都确保GPIO47为低电平）
        // 注意：如果对方主动挂断，状态可能仍显示通话中
        // 如果主动挂断，状态可能已被清除，但为确保GPIO状态一致，仍执行清理
        hang_up_cleanup();
        
        // 通知呼叫管理器电话已挂断（停止铃声，重置状态）
        call_manager_handle_call_ended();
        return true;
    }
    
    // 处理 RING - 来电通知
    if (strstr(response, "RING")) {
        ESP_LOGI(TAG, "收到来电通知 (RING): %s", response);
        // 发送 +CLCC 指令获取来电详情
        // modem_at_send_command(at_handle, "AT+CLCC", 1000, at_handle_line, NULL);
        return true;
    }
    
    // 处理 +CLCC 指令响应（来电详情）
    if (strstr(response, "+CLCC:")) {
        ESP_LOGI(TAG, "收到来电详情: %s", response);
        
        // 解析 +CLCC 响应，提取方向和号码
        // 格式: +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>,[<number>],<type>[,<alpha>][,<priority>][,<CLI validity>]
        // dir: 0=呼出(MO), 1=呼入(MT)
        int dir = -1;
        char number[20] = {0};
        if (sscanf(response, "+CLCC: %*d,%d,%*d,%*d,%*d,\"%[^\"]\",", &dir, number) >= 1) {
            ESP_LOGI(TAG, "电话方向: %s, 号码: %s", dir == 0 ? "呼出" : "呼入", number);
            if (dir == 0) {
                // 呼出电话，直接设置为通话中
                button_handler_set_call_state(true);
                ESP_LOGI(TAG, "呼出电话，电话状态已设置为: 通话中");
            } else {
                // 呼入电话，调用呼叫管理器处理来电
                call_manager_handle_incoming_call(number);
            }
        }
        return true;
    }
    
    ESP_LOGI(TAG, "未处理的非请求响应: %s", response);
    return false;
}

// AT 响应处理回调函数（用于有对应命令的响应）
static bool at_handle_line(at_handle_t at_handle, const char *line)
{
    // 处理 +CLCC 指令响应（来电详情）- 这也可能在命令响应中收到
    if (strstr(line, "+CLCC:")) {
        ESP_LOGI(TAG, "AT 响应 - 来电详情: %s", line);
        
        // 解析 +CLCC 响应，提取方向和号码
        // dir: 0=呼出(MO), 1=呼入(MT)
        int dir = -1;
        char number[20] = {0};
        if (sscanf(line, "+CLCC: %*d,%d,%*d,%*d,%*d,\"%[^\"]\",", &dir, number) >= 1) {
            ESP_LOGI(TAG, "电话方向: %s, 号码: %s", dir == 0 ? "呼出" : "呼入", number);
            if (dir == 0) {
                // 呼出电话，直接设置为通话中
                button_handler_set_call_state(true);
                ESP_LOGI(TAG, "呼出电话，电话状态已设置为: 通话中");
            } else {
                // 呼入电话，调用呼叫管理器处理来电
                call_manager_handle_incoming_call(number);
            }
        }
        return true;
    }
    
    // 处理其他响应
    ESP_LOGI(TAG, "AT 响应: %s", line);
    return true;
}

// 定义使用PSRAM的全局变量，一个用于发送AT指令字符串，一个用于接收响应字符串
char *g_at_command_buffer = NULL;  // 用于发送AT指令字符串
char *g_at_response_buffer = NULL; // 用于接收AT指令响应字符串
SemaphoreHandle_t g_at_mutex = NULL; // AT指令全局互斥锁

// 定义全局AT句柄
at_handle_t g_at_handle = NULL;

// 互联网连接状态标志，初始化为0（未连接）
uint8_t internet_connected = 0;

static EventGroupHandle_t g_event_group;
static iot_eth_driver_t *rndis_eth_driver = NULL;
static esp_netif_t *s_eth_netif = NULL; // 保存4G模块的网络接口
static modem_wifi_config_t s_modem_wifi_config = MODEM_WIFI_DEFAULT_CONFIG();
static esp_ping_handle_t s_ping = NULL;
static TimerHandle_t s_ping_timer = NULL;
static void start_ping_timer(void);
static void stop_ping_timer(void);
static void at_deinit_context(void);

#define EVENT_GOT_IP_BIT (BIT0)
#define EVENT_LINK_UP_BIT (BIT1)
#define EVENT_HW_RESET_BIT (BIT2)

volatile bool g_4g_hardware_reset_requested = false;

#define RNDIS_READY_TIMEOUT_MS 8000
#define RNDIS_COLD_BOOT_READY_TIMEOUT_MS 12000
#define RNDIS_POLL_INTERVAL_MS 250
#define RNDIS_SETTLE_DELAY_MS  500
#define RNDIS_BOOT_STRATEGY_MAGIC 0x524E4453UL
#define BOOT_CTRL_NVS_NAMESPACE "boot_ctrl"
#define BOOT_CTRL_SKIP_POWERON_KEY "skip_poweron"

static RTC_DATA_ATTR uint32_t s_rndis_boot_strategy_magic = 0;
static RTC_DATA_ATTR uint8_t s_rndis_hot_restart_pending = 0;
static bool s_use_cold_boot_extended_wait = false;

static bool is_cold_boot_reset_reason(esp_reset_reason_t reset_reason) {
  switch (reset_reason) {
  case ESP_RST_POWERON:
  case ESP_RST_BROWNOUT:
  case ESP_RST_UNKNOWN:
    return true;
  default:
    return false;
  }
}

static const char *reset_reason_to_str(esp_reset_reason_t reset_reason) {
  switch (reset_reason) {
  case ESP_RST_UNKNOWN:
    return "ESP_RST_UNKNOWN";
  case ESP_RST_POWERON:
    return "ESP_RST_POWERON";
  case ESP_RST_EXT:
    return "ESP_RST_EXT";
  case ESP_RST_SW:
    return "ESP_RST_SW";
  case ESP_RST_PANIC:
    return "ESP_RST_PANIC";
  case ESP_RST_INT_WDT:
    return "ESP_RST_INT_WDT";
  case ESP_RST_TASK_WDT:
    return "ESP_RST_TASK_WDT";
  case ESP_RST_WDT:
    return "ESP_RST_WDT";
  case ESP_RST_DEEPSLEEP:
    return "ESP_RST_DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "ESP_RST_BROWNOUT";
  case ESP_RST_SDIO:
    return "ESP_RST_SDIO";
  case ESP_RST_USB:
    return "ESP_RST_USB";
  case ESP_RST_JTAG:
    return "ESP_RST_JTAG";
  default:
    return "ESP_RST_OTHER";
  }
}

static void init_rndis_boot_strategy(void) {
  esp_reset_reason_t reset_reason = esp_reset_reason();

  if (s_rndis_boot_strategy_magic != RNDIS_BOOT_STRATEGY_MAGIC) {
    s_rndis_boot_strategy_magic = RNDIS_BOOT_STRATEGY_MAGIC;
    s_rndis_hot_restart_pending = 0;
  }

  if (reset_reason == ESP_RST_SW && s_rndis_hot_restart_pending != 0) {
    s_use_cold_boot_extended_wait = false;
    ESP_LOGI(TAG,
             "启动策略: reset_reason=%s，检测到上次RNDIS超时后的热重启，本轮直接走快速路径",
             reset_reason_to_str(reset_reason));
    return;
  }

  if (reset_reason != ESP_RST_SW) {
    s_rndis_hot_restart_pending = 0;
  }

  s_use_cold_boot_extended_wait = is_cold_boot_reset_reason(reset_reason);
  ESP_LOGI(TAG, "启动策略: reset_reason=%s，cold_boot_extended_wait=%s",
           reset_reason_to_str(reset_reason),
           s_use_cold_boot_extended_wait ? "true" : "false");
}

static TickType_t get_rndis_ready_timeout_ticks(bool first_at_init_attempt) {
  uint32_t timeout_ms = RNDIS_READY_TIMEOUT_MS;

  if (first_at_init_attempt && s_use_cold_boot_extended_wait) {
    timeout_ms = RNDIS_COLD_BOOT_READY_TIMEOUT_MS;
    ESP_LOGI(TAG, "冷启动首次RNDIS等待延长到 %" PRIu32 " ms", timeout_ms);
    s_use_cold_boot_extended_wait = false;
  }

  return pdMS_TO_TICKS(timeout_ms);
}

static void mark_rndis_hot_restart_pending(void) {
  s_rndis_boot_strategy_magic = RNDIS_BOOT_STRATEGY_MAGIC;
  s_rndis_hot_restart_pending = 1;
}

static void clear_rndis_hot_restart_pending(void) {
  if (s_rndis_hot_restart_pending != 0) {
    ESP_LOGI(TAG, "RNDIS热重启接力结束，后续恢复为正常启动策略");
  }
  s_rndis_hot_restart_pending = 0;
}

static void set_skip_poweron_prompt_flag(void) {
  nvs_handle_t nvs_handle = 0;
  esp_err_t ret =
      nvs_open(BOOT_CTRL_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS写入skip_poweron失败: %s", esp_err_to_name(ret));
    return;
  }

  ret = nvs_set_u8(nvs_handle, BOOT_CTRL_SKIP_POWERON_KEY, 1);
  if (ret == ESP_OK) {
    ret = nvs_commit(nvs_handle);
  }

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "写入skip_poweron标志失败: %s", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "已写入skip_poweron标志，下一次启动跳过poweron提示音");
  }

  nvs_close(nvs_handle);
}

static esp_err_t wait_for_usb_got_ip(TickType_t timeout_ticks) {
  if (g_event_group == NULL) {
    ESP_LOGE(TAG, "等待IP失败：事件组未初始化");
    return ESP_ERR_INVALID_STATE;
  }

  EventBits_t bits =
      xEventGroupWaitBits(g_event_group, EVENT_GOT_IP_BIT | EVENT_HW_RESET_BIT, pdFALSE, pdFALSE,
                          timeout_ticks);
  if (bits & EVENT_HW_RESET_BIT) {
    xEventGroupClearBits(g_event_group, EVENT_HW_RESET_BIT);
    return ESP_FAIL;
  }
  if (bits & EVENT_GOT_IP_BIT) {
    return ESP_OK;
  }

  return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_rndis_link_up(TickType_t timeout_ticks) {
  if (g_event_group == NULL) {
    ESP_LOGE(TAG, "等待RNDIS链路失败：事件组未初始化");
    return ESP_ERR_INVALID_STATE;
  }

  EventBits_t bits =
      xEventGroupWaitBits(g_event_group, EVENT_LINK_UP_BIT, pdFALSE, pdFALSE,
                          timeout_ticks);
  if (bits & EVENT_LINK_UP_BIT) {
    return ESP_OK;
  }

  return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_rndis_ready(TickType_t timeout_ticks) {
  if (rndis_eth_driver == NULL) {
    ESP_LOGE(TAG, "等待RNDIS就绪失败：驱动未初始化");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t link_ret = wait_for_rndis_link_up(timeout_ticks);
  if (link_ret != ESP_OK) {
    return link_ret;
  }

  TickType_t start_ticks = xTaskGetTickCount();
  bool cdc_seen_once = false;

  while ((xTaskGetTickCount() - start_ticks) < timeout_ticks) {
    usbh_cdc_handle_t cdc_hdl = usb_rndis_get_cdc_handle(rndis_eth_driver);
    if (cdc_hdl != NULL) {
      if (cdc_seen_once) {
        vTaskDelay(pdMS_TO_TICKS(RNDIS_SETTLE_DELAY_MS));
        ESP_LOGI(TAG, "RNDIS链路和CDC句柄已稳定");
        return ESP_OK;
      }
      cdc_seen_once = true;
    } else {
      cdc_seen_once = false;
    }

    vTaskDelay(pdMS_TO_TICKS(RNDIS_POLL_INTERVAL_MS));
  }

  ESP_LOGW(TAG, "RNDIS链路已连接，但CDC句柄未稳定");
  return ESP_ERR_TIMEOUT;
}

// 4G 模块硬件复位函数
void reset_4g_module_hardware(void) {
    ESP_LOGI(TAG, "执行 4G 模块异常恢复硬件复位...");
    // 移除这里的 g_4g_hardware_reset_requested = true;
    // 避免 FSM 自己调用了复位后，又把标志置为 true，导致自己无限循环复位。
    if (g_event_group != NULL) {
        xEventGroupSetBits(g_event_group, EVENT_HW_RESET_BIT);
    }
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(GPIO_NUM_21, 0);
    // 复位后给予模块一点缓冲时间
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void safe_restart_device(void) {
  ESP_LOGW(TAG, "准备重启设备，先停止音频并关闭功放");

  esp_codec_dev_handle_t play_dev = get_play_dev_handle();
  if (play_dev != NULL) {
    esp_err_t vol_ret = esp_codec_dev_set_out_vol(play_dev, 0);
    if (vol_ret != ESP_OK) {
      ESP_LOGW(TAG, "设置播放音量为0返回: %s", esp_err_to_name(vol_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(120));
  }

  esp_err_t playback_ret = audio_playback_stop();
  if (playback_ret != ESP_OK) {
    ESP_LOGW(TAG, "停止音频播放返回: %s", esp_err_to_name(playback_ret));
  }

  esp_err_t prompt_ret = audio_prompt_close();
  if (prompt_ret != ESP_OK) {
    ESP_LOGW(TAG, "关闭提示音模块返回: %s", esp_err_to_name(prompt_ret));
  }

  esp_err_t mixer_ret = audio_processor_mixer_close();
  if (mixer_ret != ESP_OK) {
    ESP_LOGW(TAG, "关闭音频mixer返回: %s", esp_err_to_name(mixer_ret));
  }

  gpio_config_t pa_gpio_cfg = {
      .pin_bit_mask = (1ULL << GPIO_NUM_46) | (1ULL << GPIO_NUM_47) |
                      (1ULL << GPIO_NUM_48),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pa_gpio_cfg);

  // 板级配置中的真正PA控制脚是GPIO46，先关PA再重启可明显降低爆音。
  gpio_set_level(GPIO_NUM_46, 0);
  gpio_set_level(GPIO_NUM_47, 0);
  gpio_set_level(GPIO_NUM_48, 0);
  vTaskDelay(pdMS_TO_TICKS(800));

  reset_4g_module_hardware();

  esp_restart();
}

static void restart_due_to_rndis_timeout(void) {
  mark_rndis_hot_restart_pending();
  set_skip_poweron_prompt_flag();
  safe_restart_device();
}

static esp_err_t recover_rndis_link(iot_eth_handle_t eth_handle) {
  ESP_LOGI(TAG, "开始恢复4G模块和RNDIS链路状态...");

  reset_4g_module_hardware();

#if CONFIG_EXAMPLE_ENABLE_AT_CMD
  // 先清理AT上下文，避免后续继续使用失效的句柄。
  at_deinit_context();
#endif

  internet_connected = 0;
  if (g_event_group != NULL) {
    xEventGroupClearBits(g_event_group, EVENT_LINK_UP_BIT);
    xEventGroupClearBits(g_event_group, EVENT_GOT_IP_BIT);
  }

  iot_eth_stop(eth_handle);
  vTaskDelay(pdMS_TO_TICKS(1000));
  iot_eth_start(eth_handle);

  esp_err_t wait_ret = wait_for_rndis_link_up(pdMS_TO_TICKS(RNDIS_READY_TIMEOUT_MS));
  if (wait_ret == ESP_OK) {
    ESP_LOGI(TAG, "RNDIS链路恢复完成");
  } else {
    ESP_LOGW(TAG, "等待RNDIS链路恢复超时: %s", esp_err_to_name(wait_ret));
  }

  return wait_ret;
}

#define AT_PROBE_RETRY_DELAY_MS   1000
#define AT_READY_MAX_RETRIES      15
#define SIM_READY_MAX_RETRIES     20
#define NETWORK_READY_MAX_RETRIES 30
#define AT_PORT_OPEN_TIMEOUT_MS   10000
#define AT_INIT_RETRY_DELAY_MS    2000

static esp_err_t wait_for_at_probe_ready(uint32_t retries, uint32_t delay_ms);
static esp_err_t wait_for_sim_ready(uint32_t retries, uint32_t delay_ms);
static esp_err_t wait_for_network_registered(uint32_t retries, uint32_t delay_ms,
                                             esp_modem_at_cereg_t *out_status);

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
  uint8_t ttl;
  uint16_t seqno;
  uint32_t elapsed_time, recv_len;
  ip_addr_t target_addr;
  esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
  esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr,
                       sizeof(target_addr));
  esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time,
                       sizeof(elapsed_time));
  ESP_LOGI(TAG,
           "%" PRIu32 " bytes from %s icmp_seq=%u ttl=%u time=%" PRIu32 " ms\n",
           recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, elapsed_time);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
  uint16_t seqno;
  ip_addr_t target_addr;
  esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr,
                       sizeof(target_addr));
  ESP_LOGW(TAG, "From %s icmp_seq=%u timeout\n", ipaddr_ntoa(&target_addr),
           seqno);
  // Users can add logic to handle ping timeout
  // Add Wait or Reset logic
}

static esp_ping_handle_t ping_create() {
  ip_addr_t target_addr;
  memset(&target_addr, 0, sizeof(target_addr));
  char *ping_addr_s = NULL;
  ping_addr_s = "127.0.0.1";
  esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
  ipaddr_aton(ping_addr_s, &target_addr);
  ping_config.target_addr = target_addr;
  ping_config.timeout_ms = 2000;
  ping_config.task_stack_size = 2396; // Ping任务栈大小（4096字节）
  ping_config.count = 1;

  /* set callback functions */
  esp_ping_callbacks_t cbs = {
      .on_ping_success = on_ping_success,
      .on_ping_timeout = on_ping_timeout,
      .on_ping_end = NULL,
      .cb_args = NULL,
  };
  esp_ping_handle_t ping;
  esp_ping_new_session(&ping_config, &cbs, &ping);
  return ping;
}

// Periodic ping using FreeRTOS software timer. Ping starts only when IP is
// available.
static void ping_timer_cb(TimerHandle_t xTimer) {
  // Send one ping per timer tick while network is connected
  if (s_ping) {
    esp_ping_start(s_ping);
    ESP_LOGI(TAG, "Network is connected, starting ping...");
  }
}

static void start_ping_timer(void) {
  if (s_ping == NULL) {
    s_ping = ping_create();
    ESP_RETURN_VOID_ON_FALSE(s_ping != NULL, TAG, "Failed to create ping");
  }
  if (s_ping_timer == NULL) {
    s_ping_timer = xTimerCreate("ping_periodic", pdMS_TO_TICKS(5000), pdTRUE,
                                NULL, ping_timer_cb);
    ESP_RETURN_VOID_ON_FALSE(s_ping_timer != NULL, TAG,
                             "Failed to create FreeRTOS timer");
  }
  if (xTimerIsTimerActive(s_ping_timer) == pdFALSE) {
    BaseType_t ok = xTimerStart(s_ping_timer, 0);
    ESP_RETURN_VOID_ON_FALSE(ok == pdPASS, TAG,
                             "Failed to start FreeRTOS timer");
    ESP_LOGI(TAG, "Ping timer started");
  }
}

static void stop_ping_timer(void) {
  if (s_ping_timer && xTimerIsTimerActive(s_ping_timer) == pdTRUE) {
    BaseType_t ok = xTimerStop(s_ping_timer, 0);
    ESP_RETURN_VOID_ON_FALSE(ok == pdPASS, TAG,
                             "Failed to stop FreeRTOS timer");
    ESP_LOGI(TAG, "Ping timer stopped");
  }
  if (s_ping) {
    // Stop ongoing ping session if any
    esp_ping_stop(s_ping);
  }
}

static esp_err_t wait_for_at_probe_ready(uint32_t retries, uint32_t delay_ms);
static esp_err_t wait_for_sim_ready(uint32_t retries, uint32_t delay_ms);
static esp_err_t wait_for_network_registered(uint32_t retries, uint32_t delay_ms,
                                             esp_modem_at_cereg_t *out_status);

#if CONFIG_EXAMPLE_ENABLE_AT_CMD
typedef enum {
    STATE_POWER_ON,
    STATE_AT_SYNC,
    STATE_CHECK_SIM,
    STATE_CHECK_NETWORK,
    STATE_START_DATACALL,
    STATE_CONNECTED,
    STATE_ERROR_NO_SIM,
    STATE_ERROR_NET_FAIL
} ml307a_state_t;

typedef struct {
  usbh_cdc_port_handle_t cdc_port; /*!< CDC port handle */
  at_handle_t at_handle;           /*!< AT command parser handle */
} at_ctx_t;

at_ctx_t g_at_ctx = {0};

// 调制解调器信息结构体
typedef struct {
  char manufacturer_id[64];
  char module_id[64];
  char revision_id[64];
  char pdp_context[128];
} modem_info_t;

static modem_info_t *modem_info = NULL;

static esp_err_t _at_send_cmd(const char *command, size_t length,
                              void *usr_data) {
  at_ctx_t *at_ctx = (at_ctx_t *)usr_data;

  // 检查CDC端口是否有效
  if (at_ctx->cdc_port == NULL) {
    ESP_LOGE(TAG, "CDC port is NULL, cannot send command: %s", command);
    return ESP_FAIL;
  }

  return usbh_cdc_write_bytes(at_ctx->cdc_port, (const uint8_t *)command,
                              length, pdMS_TO_TICKS(500));
}

static void _at_port_closed_cb(usbh_cdc_port_handle_t cdc_port_handle,
                               void *arg) {
  at_ctx_t *at_ctx = (at_ctx_t *)arg;
  ESP_LOGI(TAG, "AT port closed");
  at_ctx->cdc_port = NULL;

  // 保存AT句柄指针，用于后续清除全局变量
  at_handle_t at_handle_to_clear = at_ctx->at_handle;

  if (at_ctx->at_handle) {
    // 先停止AT解析器，确保不会处理更多数据
    modem_at_stop(at_ctx->at_handle);
    // 延迟销毁AT解析器，确保所有回调都完成
    modem_at_parser_destroy(at_ctx->at_handle);
    at_ctx->at_handle = NULL;
  }

  // 清除全局AT句柄
  if (g_at_handle == at_handle_to_clear) {
    g_at_handle = NULL;
  }
}

static void _at_recv_data_cb(usbh_cdc_port_handle_t cdc_port_handle,
                             void *arg) {
  at_ctx_t *at_ctx = (at_ctx_t *)arg;

  // 检查AT句柄是否有效
  if (at_ctx->at_handle == NULL) {
    ESP_LOGW(TAG, "AT handle is NULL, ignoring received data");
    return;
  }

  size_t length = 0;
  usbh_cdc_get_rx_buffer_size(cdc_port_handle, &length);
  if (length == 0) {
    return;
  }

  char *buffer;
  size_t buffer_remain;
  modem_at_get_response_buffer(at_ctx->at_handle, &buffer, &buffer_remain);
  if (buffer_remain < length) {
    length = buffer_remain;
    ESP_LOGE(TAG, "data size is too big, truncated to %d", length);
  }
  usbh_cdc_read_bytes(cdc_port_handle, (uint8_t *)buffer, &length, 0);
  // Parse the AT command response
  modem_at_write_response_done(at_ctx->at_handle, length);
}

static void at_deinit_context(void) {
  usbh_cdc_port_handle_t cdc_port = g_at_ctx.cdc_port;
  at_handle_t at_handle = g_at_ctx.at_handle;

  g_at_ctx.cdc_port = NULL;
  g_at_ctx.at_handle = NULL;
  if (g_at_handle == at_handle) {
    g_at_handle = NULL;
  }

  if (cdc_port != NULL) {
    esp_err_t close_ret = usbh_cdc_port_close(cdc_port);
    if (close_ret != ESP_OK && close_ret != ESP_ERR_INVALID_STATE &&
        close_ret != ESP_ERR_INVALID_ARG) {
      ESP_LOGW(TAG, "Failed to close CDC port cleanly: %s",
               esp_err_to_name(close_ret));
    }
  }

  if (at_handle != NULL) {
    modem_at_stop(at_handle);
    esp_err_t destroy_ret = modem_at_parser_destroy(at_handle);
    if (destroy_ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to destroy AT parser cleanly: %s",
               esp_err_to_name(destroy_ret));
    }
  }
}

static esp_err_t at_init() {
  ESP_LOGI(TAG, "AT init");
  if (g_at_ctx.cdc_port != NULL || g_at_ctx.at_handle != NULL) {
    ESP_LOGW(TAG, "AT context still active, cleaning up before re-init");
    at_deinit_context();
  }

  // 初始化全局AT上下文
  memset(&g_at_ctx, 0, sizeof(at_ctx_t));

  // 先创建AT命令解析器，确保在打开CDC端口之前完成
  modem_at_config_t at_config = {.send_buffer_length = 256,
                                 .recv_buffer_length = 256,
                                 .io = {
                                     .send_cmd = _at_send_cmd,
                                     .usr_data = &g_at_ctx,
                                 }};
  g_at_ctx.at_handle = modem_at_parser_create(&at_config);
  if (g_at_ctx.at_handle == NULL) {
    ESP_LOGE(TAG, "Failed to create AT parser");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "AT parser created successfully");

  // Open a CDC port for AT command
  usbh_cdc_handle_t cdc_hdl = usb_rndis_get_cdc_handle(rndis_eth_driver);
  if (cdc_hdl == NULL) {
    ESP_LOGE(TAG, "Failed to get CDC handle");
    modem_at_parser_destroy(g_at_ctx.at_handle);
    g_at_ctx.at_handle = NULL;
    return ESP_FAIL;
  }

  usbh_cdc_port_config_t cdc_port_config = {
      .itf_num = CONFIG_EXAMPLE_AT_INTERFACE_NUM,
      .in_transfer_buffer_size = 512,
      .out_transfer_buffer_size = 512,
      .cbs =
          {
              .notif_cb = NULL,
              .recv_data = _at_recv_data_cb,
              .closed = _at_port_closed_cb,
              .user_data = &g_at_ctx,
          },
  };
  esp_err_t ret = usbh_cdc_port_open(cdc_hdl, &cdc_port_config,
                                     pdMS_TO_TICKS(AT_PORT_OPEN_TIMEOUT_MS),
                                     &g_at_ctx.cdc_port);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open CDC port: %s", esp_err_to_name(ret));
    at_deinit_context();
    return ret;
  }
  ESP_LOGI(TAG, "CDC port opened successfully");

  // 设置全局AT句柄
  g_at_handle = g_at_ctx.at_handle;

  // 注册非请求响应处理回调
  modem_at_register_unsolicited_handler(g_at_ctx.at_handle, at_handle_unsolicited_response);
  ESP_LOGI(TAG, "已注册非请求响应处理回调");

  // 启动AT解析器
  ret = modem_at_start(g_at_ctx.at_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start AT parser: %s", esp_err_to_name(ret));
    at_deinit_context();
    return ret;
  }
  ESP_LOGI(TAG, "AT parser started successfully");

  return ESP_OK;
}

// 获取模块信息
static esp_err_t wait_for_at_probe_ready(uint32_t retries, uint32_t delay_ms)
{
  for (uint32_t attempt = 0; attempt < retries; attempt++) {
    if (g_at_ctx.at_handle == NULL) {
      ESP_LOGW(TAG, "AT handle is NULL while probing modem readiness");
      return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = at_send_command_response_ok(g_at_ctx.at_handle, "AT");
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "AT probe succeeded on attempt %" PRIu32 "/%" PRIu32,
               attempt + 1, retries);
      return ESP_OK;
    }

    ESP_LOGW(TAG, "AT probe failed on attempt %" PRIu32 "/%" PRIu32 ": %s",
             attempt + 1, retries, esp_err_to_name(ret));
    if (attempt + 1 < retries) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_sim_ready(uint32_t retries, uint32_t delay_ms)
{
  esp_err_t last_ret = ESP_FAIL;

  for (uint32_t attempt = 0; attempt < retries; attempt++) {
    esp_modem_pin_state_t current_pin_state = PIN_UNKNOWN;
    last_ret = at_cmd_read_pin(g_at_ctx.at_handle, &current_pin_state);
    if (last_ret == ESP_OK && current_pin_state == PIN_READY) {
      ESP_LOGI(TAG, "SIM is ready on attempt %" PRIu32 "/%" PRIu32,
               attempt + 1, retries);
      return ESP_OK;
    }

    if (last_ret == ESP_OK) {
      ESP_LOGW(TAG, "SIM not ready yet on attempt %" PRIu32 "/%" PRIu32
                    ", pin_state=%d",
               attempt + 1, retries, current_pin_state);
    } else {
      ESP_LOGW(TAG, "Failed to read SIM state on attempt %" PRIu32 "/%" PRIu32 ": %s",
               attempt + 1, retries, esp_err_to_name(last_ret));
    }

    if (attempt + 1 < retries) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  return last_ret == ESP_OK ? ESP_ERR_TIMEOUT : last_ret;
}

static esp_err_t wait_for_network_registered(uint32_t retries, uint32_t delay_ms,
                                             esp_modem_at_cereg_t *out_status)
{
  esp_err_t last_ret = ESP_FAIL;
  esp_modem_at_cereg_t reg_status = {0};

  for (uint32_t attempt = 0; attempt < retries; attempt++) {
    last_ret = at_cmd_get_network_reg_status(g_at_ctx.at_handle, &reg_status);
    if (last_ret == ESP_OK && (reg_status.stat == 1 || reg_status.stat == 5)) {
      if (out_status) {
        *out_status = reg_status;
      }
      ESP_LOGI(TAG, "Network registered on attempt %" PRIu32 "/%" PRIu32
                    ", stat=%d",
               attempt + 1, retries, reg_status.stat);
      return ESP_OK;
    }

    if (last_ret == ESP_OK) {
      ESP_LOGW(TAG, "Network not registered yet on attempt %" PRIu32
                    "/%" PRIu32 ", stat=%d",
               attempt + 1, retries, reg_status.stat);
    } else {
      ESP_LOGW(TAG, "Failed to read network status on attempt %" PRIu32
                    "/%" PRIu32 ": %s",
               attempt + 1, retries, esp_err_to_name(last_ret));
    }

    if (attempt + 1 < retries) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  if (out_status) {
    *out_status = reg_status;
  }
  return last_ret == ESP_OK ? ESP_ERR_TIMEOUT : last_ret;
}

static void get_modem_info(void) {
  if (g_at_ctx.at_handle == NULL) {
    ESP_LOGE(TAG, "AT句柄未初始化，无法获取模块信息");
    return;
  }

  if (modem_info == NULL) {
    ESP_LOGE(TAG, "modem_info结构体未初始化，无法获取模块信息");
    return;
  }

  ESP_LOGI(TAG, "获取4G模块信息...");

  // 获取制造商ID
  if (at_cmd_get_manufacturer_id(
          g_at_ctx.at_handle, modem_info->manufacturer_id,
          sizeof(modem_info->manufacturer_id)) == ESP_OK) {
    ESP_LOGI(TAG, "获取制造商ID成功: %s", modem_info->manufacturer_id);
  } else {
    ESP_LOGE(TAG, "获取制造商ID失败");
  }

  // 获取模块ID
  if (at_cmd_get_module_id(g_at_ctx.at_handle, modem_info->module_id,
                           sizeof(modem_info->module_id)) == ESP_OK) {
    ESP_LOGI(TAG, "获取模块ID成功: %s", modem_info->module_id);
  } else {
    ESP_LOGE(TAG, "获取模块ID失败");
  }

  // 获取版本ID
  if (at_cmd_get_revision_id(g_at_ctx.at_handle, modem_info->revision_id,
                             sizeof(modem_info->revision_id)) == ESP_OK) {
    ESP_LOGI(TAG, "获取修订版本ID成功: %s", modem_info->revision_id);
  } else {
    ESP_LOGE(TAG, "获取修订版本ID失败");
  }

  // 获取PDP上下文
  if (at_cmd_get_pdp_context(g_at_ctx.at_handle, modem_info->pdp_context,
                             sizeof(modem_info->pdp_context)) == ESP_OK) {
    ESP_LOGI(TAG, "PDP上下文: %s", modem_info->pdp_context);
  } else {
    ESP_LOGE(TAG, "获取PDP上下文失败");
  }

  ESP_LOGI(TAG, "模块信息获取完成");
}

// 响应处理函数，用于检查AT命令的响应是否为OK
static bool dialup_response_ok(at_handle_t at_handle, const char *line)
{
    esp_err_t *ret = (esp_err_t *)modem_at_get_handle_line_ctx(at_handle);
    if (strstr(line, "OK")) {
        *ret = ESP_OK;
        return true;
    } else if (strstr(line, "ERROR")) {
        ESP_LOGE(TAG, "AT command \"%s\" ERROR", modem_at_get_current_cmd(at_handle));
        *ret = ESP_FAIL;
        return true;
    }
    return false;
}

// 执行4G拨号上下文配置和数据拨号
static esp_err_t execute_datacall(void) {
  esp_err_t ret;
  esp_modem_at_csq_t signal_quality;

  ESP_LOGI(TAG, "Starting data call sequence...");

  ESP_LOGI(TAG, "Checking signal quality...");
  ret = at_cmd_get_signal_quality(g_at_ctx.at_handle, &signal_quality);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Signal quality: RSSI=%d, BER=%d", signal_quality.rssi,
             signal_quality.ber);
  }

  ESP_LOGI(TAG, "Setting PDP context...");
  esp_modem_at_pdp_t pdp_ctx = {
      .cid = 1,
      .type = "IP",
      .apn = "CMNET",
  };
  ret = at_cmd_set_pdp_context(g_at_ctx.at_handle, &pdp_ctx);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set PDP context, continuing to dial");
  }

  ESP_LOGI(TAG, "AT/SIM/network are ready, starting dialup...");
  esp_err_t dial_ret = ESP_ERR_TIMEOUT;
  ret = modem_at_send_command(g_at_ctx.at_handle, "AT+MDIALUP=1,1", 10000,
                              dialup_response_ok, &dial_ret);
  if (ret != ESP_OK || dial_ret != ESP_OK) {
    ESP_LOGE(TAG, "Dial command failed, ret=%d, response=%d", ret, dial_ret);

    char *error_response = NULL;
    size_t response_length = 0;
    modem_at_get_response_buffer(g_at_ctx.at_handle, &error_response,
                                 &response_length);
    if (error_response != NULL && response_length > 0) {
      ESP_LOGE(TAG, "Dialup error response: %.*s", (int)response_length,
               error_response);
    }

    esp_modem_at_cereg_t current_network_status;
    if (at_cmd_get_network_reg_status(g_at_ctx.at_handle,
                                      &current_network_status) == ESP_OK) {
      ESP_LOGI(TAG, "Current network reg stat=%d", current_network_status.stat);
    }

    esp_modem_at_csq_t current_signal_quality;
    if (at_cmd_get_signal_quality(g_at_ctx.at_handle,
                                  &current_signal_quality) == ESP_OK) {
      ESP_LOGI(TAG, "Current signal quality: RSSI=%d, BER=%d",
               current_signal_quality.rssi, current_signal_quality.ber);
    }

    return ret != ESP_OK ? ret : dial_ret;
  }

  ESP_LOGI(TAG, "Dialup succeeded, reading modem info...");
  get_modem_info();

  ESP_LOGI(TAG, "Verifying network status after dialup...");
  esp_modem_at_cereg_t network_status_after_dialup;
  ret = at_cmd_get_network_reg_status(g_at_ctx.at_handle,
                                      &network_status_after_dialup);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Post-dial network stat=%d", network_status_after_dialup.stat);
  } else {
    ESP_LOGW(TAG, "Failed to read post-dial network status");
  }

  ESP_LOGI(TAG, "4G dialup flow finished");
  return ESP_OK;
}

#endif

static void iot_event_handle(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
  if (event_base == IOT_ETH_EVENT) {
    switch (event_id) {
    case IOT_ETH_EVENT_START:
      ESP_LOGI(TAG, "IOT_ETH_EVENT_START");
      if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, EVENT_LINK_UP_BIT | EVENT_GOT_IP_BIT);
      }
      break;
    case IOT_ETH_EVENT_STOP:
      ESP_LOGI(TAG, "IOT_ETH_EVENT_STOP");
      if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, EVENT_LINK_UP_BIT | EVENT_GOT_IP_BIT);
      }
      break;
    case IOT_ETH_EVENT_CONNECTED:
      ESP_LOGI(TAG, "IOT_ETH_EVENT_CONNECTED");
      if (g_event_group != NULL) {
        xEventGroupSetBits(g_event_group, EVENT_LINK_UP_BIT);
      }
      break;
    case IOT_ETH_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "IOT_ETH_EVENT_DISCONNECTED");
      internet_connected = 0;
      if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, EVENT_LINK_UP_BIT | EVENT_GOT_IP_BIT);
      }
      stop_ping_timer();
      break;
    default:
      ESP_LOGI(TAG, "IOT_ETH_EVENT_UNKNOWN");
      break;
    }
  } else if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_ETH_GOT_IP) {
      ip_event_got_ip_t *got_ip_event = (ip_event_got_ip_t *)event_data;
      internet_connected = 1;
      if (got_ip_event != NULL) {
        ESP_LOGI(TAG, "USB RNDIS GOT_IP: " IPSTR,
                 IP2STR(&got_ip_event->ip_info.ip));
      } else {
        ESP_LOGI(TAG, "USB RNDIS GOT_IP");
      }
      if (g_event_group != NULL) {
        xEventGroupSetBits(g_event_group, EVENT_GOT_IP_BIT);
      }
#if CONFIG_EXAMPLE_ENABLE_AT_CMD
      // 如果启用了AT命令，等待AT拨号完成后再开始ping
      // ping将在AT拨号成功后手动启动
#else
      // 如果没有启用AT命令，直接开始ping
      // start_ping_timer();
#endif
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
      internet_connected = 0;
      ESP_LOGW(TAG, "USB RNDIS LOST_IP");
      if (g_event_group != NULL) {
        xEventGroupClearBits(g_event_group, EVENT_GOT_IP_BIT);
      }
    }
  }
}

void usb_module_app_init(void) {
  init_rndis_boot_strategy();

  /* Initialize default TCP/IP stack */
  ESP_ERROR_CHECK(esp_netif_init());

  /* Check if default event loop is already created, if not create it */
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(err);
  } else if (err == ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "默认事件循环已存在，跳过创建");
  }

  g_event_group = xEventGroupCreate();
  ESP_RETURN_ON_FALSE(g_event_group != NULL, , TAG,
                      "Failed to create event group");
  esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, iot_event_handle,
                             NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, iot_event_handle,
                             NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, iot_event_handle,
                             NULL);

  // install usbh cdc driver
  usbh_cdc_driver_config_t config = {
      .task_stack_size = 8 * 1024, // USB CDC驱动任务栈大小（4096字节）
      .task_priority = 20,
      .task_coreid = 0,
      .skip_init_usb_host_driver = false,
  };
  ESP_ERROR_CHECK(usbh_cdc_driver_install(&config));

  static const usb_device_match_id_t dev_match_id[] = {
      {
          .match_flags =
              USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT,
          .idVendor = USB_DEVICE_VENDOR_ANY,
          .idProduct = USB_DEVICE_PRODUCT_ANY,
      },
      {0}, // Null-terminated
  };
  iot_usbh_rndis_config_t rndis_cfg = {
      .match_id_list = dev_match_id,
  };
  esp_err_t ret = iot_eth_new_usb_rndis(&rndis_cfg, &rndis_eth_driver);
  ESP_RETURN_VOID_ON_ERROR(ret, TAG, "Failed to create USB RNDIS driver");

  iot_eth_config_t eth_cfg = {
      .driver = rndis_eth_driver,
      .stack_input = NULL,
  };
  iot_eth_handle_t eth_handle = NULL;
  ESP_RETURN_VOID_ON_ERROR(iot_eth_install(&eth_cfg, &eth_handle), TAG,
                           "Failed to install eth driver");

  // 创建自定义网络接口配置，使用独立键值避免与WiFi冲突
  esp_netif_inherent_config_t eth_base_cfg = {
      .flags = ESP_NETIF_DHCP_CLIENT,
      .get_ip_event = IP_EVENT_ETH_GOT_IP,
      .lost_ip_event = IP_EVENT_ETH_LOST_IP,
      .if_key = "USB_DEF", // 使用独立键值，避免与WiFi接口冲突
      .if_desc = "usb_rndis",
      .route_prio = 50 // 设置比WiFi低的优先级，WiFi优先
  };

  esp_netif_config_t netif_cfg = {.base = &eth_base_cfg,
                                  .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
                                  .driver = NULL};
  esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
  s_eth_netif = eth_netif; // 保存网络接口指针
  ESP_RETURN_VOID_ON_FALSE(eth_netif != NULL, TAG, "Failed to create netif");
  iot_eth_netif_glue_handle_t glue = iot_eth_new_netif_glue(eth_handle);
  ESP_RETURN_VOID_ON_FALSE(glue != NULL, TAG, "Failed to create netif glue");
  esp_netif_attach(eth_netif, glue);

  // 初始化全局互斥锁
  if (g_at_mutex == NULL) {
      g_at_mutex = xSemaphoreCreateMutex();
      if (g_at_mutex == NULL) {
          ESP_LOGE(TAG, "创建AT互斥锁失败");
          return;
      }
  }

  // 分配PSRAM内存用于AT指令字符串
  g_at_command_buffer = (char *)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
  if (g_at_command_buffer == NULL) {
    ESP_LOGE(TAG, "分配AT指令发送缓冲区失败");
    return;
  }
  memset(g_at_command_buffer, 0, 256);

  // 分配PSRAM内存用于AT指令响应
  g_at_response_buffer = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
  if (g_at_response_buffer == NULL) {
    ESP_LOGE(TAG, "分配AT指令响应缓冲区失败");
    heap_caps_free(g_at_command_buffer);
    g_at_command_buffer = NULL;
    return;
  }
  memset(g_at_response_buffer, 0, 1024);

  // 分配PSRAM内存用于modem_info结构体
  modem_info =
      (modem_info_t *)heap_caps_malloc(sizeof(modem_info_t), MALLOC_CAP_SPIRAM);
  if (modem_info == NULL) {
    ESP_LOGE(TAG, "分配modem_info结构体内存失败");
    heap_caps_free(g_at_command_buffer);
    g_at_command_buffer = NULL;
    heap_caps_free(g_at_response_buffer);
    g_at_response_buffer = NULL;
    return;
  }
  memset(modem_info, 0, sizeof(modem_info_t));

  ESP_LOGI(TAG,
           "AT指令缓冲区分配成功 - 发送缓冲区:256字节, 响应缓冲区:1024字节, "
           "modem_info结构体:%" PRIu32 "字节",
           (uint32_t)sizeof(modem_info_t));

  internet_connected = 0;
  xEventGroupClearBits(g_event_group, EVENT_LINK_UP_BIT | EVENT_GOT_IP_BIT);

  // 启动以太网驱动
  iot_eth_start(eth_handle);

#if CONFIG_EXAMPLE_ENABLE_AT_CMD
  while (1) {
      ml307a_state_t  fsm_state = STATE_POWER_ON;
      while (fsm_state != STATE_CONNECTED && fsm_state != STATE_ERROR_NO_SIM) {
          if (g_4g_hardware_reset_requested) {
              ESP_LOGW(TAG, "检测到硬件复位请求，重置 FSM 状态机！");
              g_4g_hardware_reset_requested = false;
              at_deinit_context();
              vTaskDelay(pdMS_TO_TICKS(1000));
              recover_rndis_link(eth_handle);
              fsm_state = STATE_POWER_ON;
          }

          switch (fsm_state) {
              case STATE_POWER_ON: {
                  ESP_LOGI(TAG, "[FSM] STATE_POWER_ON - 等待RNDIS就绪");
                  TickType_t rndis_wait_ticks = get_rndis_ready_timeout_ticks(true);
                  esp_err_t rndis_ready_ret = wait_for_rndis_ready(rndis_wait_ticks);
                  if (rndis_ready_ret == ESP_OK) {
                      fsm_state = STATE_AT_SYNC;
                  } else {
                      ESP_LOGW(TAG, "RNDIS就绪超时，尝试恢复链路...");
                      if (recover_rndis_link(eth_handle) == ESP_ERR_TIMEOUT) {
                          ESP_LOGE(TAG, "RNDIS链路恢复超时，执行热重启");
                          restart_due_to_rndis_timeout();
                      }
                  }
                  break;
              }
              case STATE_AT_SYNC: {
                  ESP_LOGI(TAG, "[FSM] STATE_AT_SYNC - 初始化AT及同步波特率");
                  if (g_at_ctx.cdc_port == NULL || g_at_ctx.at_handle == NULL) {
                      esp_err_t at_ret = at_init();
                      if (at_ret != ESP_OK) {
                          ESP_LOGW(TAG, "AT初始化失败: %s", esp_err_to_name(at_ret));
                          at_deinit_context();
                          vTaskDelay(pdMS_TO_TICKS(1000));
                          recover_rndis_link(eth_handle);
                          fsm_state = STATE_POWER_ON;
                          break;
                      }
                  }

                  if (wait_for_at_probe_ready(AT_READY_MAX_RETRIES, AT_PROBE_RETRY_DELAY_MS) == ESP_OK) {
                      if (at_cmd_set_echo(g_at_ctx.at_handle, true) != ESP_OK) {
                          ESP_LOGW(TAG, "Failed to enable modem echo, continuing");
                      }
                      fsm_state = STATE_CHECK_SIM;
                  } else {
                      ESP_LOGW(TAG, "AT指令无响应");
                      at_deinit_context();
                      vTaskDelay(pdMS_TO_TICKS(1000));
                      recover_rndis_link(eth_handle);
                      fsm_state = STATE_POWER_ON;
                  }
                  break;
              }
              case STATE_CHECK_SIM: {
                  ESP_LOGI(TAG, "[FSM] STATE_CHECK_SIM - 检查SIM卡状态");
                  esp_err_t sim_ret = wait_for_sim_ready(SIM_READY_MAX_RETRIES, AT_PROBE_RETRY_DELAY_MS);
                  if (sim_ret == ESP_OK) {
                      fsm_state = STATE_CHECK_NETWORK;
                  } else {
                      ESP_LOGE(TAG, "未检测到SIM卡或SIM卡异常");
                      fsm_state = STATE_ERROR_NO_SIM;
                  }
                  break;
              }
              case STATE_CHECK_NETWORK: {
                  ESP_LOGI(TAG, "[FSM] STATE_CHECK_NETWORK - 检查网络注册状态");
                  esp_modem_at_cereg_t network_status;
                  esp_err_t net_ret = wait_for_network_registered(NETWORK_READY_MAX_RETRIES, AT_PROBE_RETRY_DELAY_MS, &network_status);
                  
                  if (net_ret == ESP_OK && (network_status.stat == 1 || network_status.stat == 5)) {
                      fsm_state = STATE_START_DATACALL;
                  } else {
                      ESP_LOGW(TAG, "驻网失败或超时 (stat=%d)", network_status.stat);
                      fsm_state = STATE_ERROR_NET_FAIL;
                  }
                  break;
              }
              case STATE_START_DATACALL: {
                  ESP_LOGI(TAG, "[FSM] STATE_START_DATACALL - 开始数据拨号");
                  if (g_event_group != NULL) {
                      xEventGroupClearBits(g_event_group, EVENT_GOT_IP_BIT);
                  }
                  internet_connected = 0;

                  if (execute_datacall() == ESP_OK) {
                      ESP_LOGI(TAG, "4G数据拨号指令发送成功，等待获取IP地址...");
                      if (wait_for_usb_got_ip(pdMS_TO_TICKS(30000)) == ESP_OK) {
                          ESP_LOGI(TAG, "4G网络已成功获取IP地址");
                          clear_rndis_hot_restart_pending();
                          fsm_state = STATE_CONNECTED;
                      } else {
                          ESP_LOGE(TAG, "获取IP地址超时");
                          fsm_state = STATE_ERROR_NET_FAIL;
                      }
                  } else {
                      ESP_LOGE(TAG, "数据拨号失败");
                      fsm_state = STATE_ERROR_NET_FAIL;
                  }
                  break;
              }
              case STATE_ERROR_NO_SIM:
              case STATE_CONNECTED:
                  // 这些状态在循环条件外处理或在下方专门处理
                  break;
              case STATE_ERROR_NET_FAIL:
                  ESP_LOGW(TAG, "4G网络连接或拨号失败，等待10秒后重试...");
                  vTaskDelay(pdMS_TO_TICKS(10000));
                  fsm_state = STATE_CHECK_NETWORK; // 退回到检查网络状态，以便恢复连接
                  break;
          }
      }

      if (fsm_state == STATE_ERROR_NO_SIM) {
          ESP_LOGE(TAG, "4G初始化终止：未检测到SIM卡。如已插入请尝试重启设备。");
          audio_prompt_play("file:///spiffs/sim_error.mp3"); // 播放无SIM卡提示音
          at_deinit_context();
          internet_connected = 0;
          break; // 退出外层循环，结束任务
      } else if (fsm_state == STATE_CONNECTED) {
          ESP_LOGI(TAG, "4G初始化成功完成！");
          internet_connected = 1;

          esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
          if (wifi_netif) {
              ESP_LOGI(TAG, "测试WiFi外网连通性...");
              if (test_network_connectivity_with_netif("[WiFi]", wifi_netif) != ESP_OK) {
                  ESP_LOGW(TAG, "WiFi无外网，切换到4G路由...");
                  switch_to_4g_network();
              } else {
                  ESP_LOGI(TAG, "WiFi外网正常，保持WiFi路由。");
              }
          } else {
              ESP_LOGI(TAG, "WiFi未连接，切换到4G路由...");
              switch_to_4g_network();
          }

          // ===== 新增监控模式：检测到掉线自动重启RNDIS =====
          ESP_LOGI(TAG, "进入4G链路后台监控模式");
          while (1) {
              vTaskDelay(pdMS_TO_TICKS(1000));
              
              if (g_4g_hardware_reset_requested) {
                  ESP_LOGW(TAG, "监测到硬件复位请求，准备重启链路...");
                  g_4g_hardware_reset_requested = false;
                  break;
              }

              // 当RNDIS底层掉线时，event handler会将其设为0
              if (internet_connected == 0) {
                  vTaskDelay(pdMS_TO_TICKS(3000)); // 防抖
                  if (internet_connected == 0) {
                      ESP_LOGE(TAG, "检测到4G USB链路意外掉线！触发自动重连机制...");
                      break;
                  }
              }
          }

          // 走到这里说明脱离了监控循环（掉线或请求复位），进行重连恢复
          at_deinit_context();
          vTaskDelay(pdMS_TO_TICKS(1000));
          recover_rndis_link(eth_handle);
      }
  }
#endif
}

// 获取4G模块的网络接口
esp_netif_t *get_usb_netif(void) { return s_eth_netif; }

// 释放AT指令缓冲区内存
void free_at_buffers(void) {
  if (g_at_command_buffer != NULL) {
    heap_caps_free(g_at_command_buffer);
    g_at_command_buffer = NULL;
    ESP_LOGI(TAG, "释放AT指令发送缓冲区");
  }

  if (g_at_response_buffer != NULL) {
    heap_caps_free(g_at_response_buffer);
    g_at_response_buffer = NULL;
    ESP_LOGI(TAG, "释放AT指令响应缓冲区");
  }

  if (g_at_mutex != NULL) {
    vSemaphoreDelete(g_at_mutex);
    g_at_mutex = NULL;
    ESP_LOGI(TAG, "释放AT互斥锁");
  }

  if (modem_info != NULL) {
    heap_caps_free(modem_info);
    modem_info = NULL;
    ESP_LOGI(TAG, "释放modem_info结构体内存");
  }
}

esp_err_t usb_rndis_4g_module_deinit(void) {
  ESP_LOGI(TAG, "开始反初始化4G模块...");

  // 释放AT指令缓冲区
  free_at_buffers();

  // 停止ping定时器
  stop_ping_timer();

  // 清理事件组
  if (g_event_group != NULL) {
    vEventGroupDelete(g_event_group);
    g_event_group = NULL;
  }

  // 清理ping句柄
  if (s_ping != NULL) {
    esp_ping_delete_session(s_ping);
    s_ping = NULL;
  }

  // 清理ping定时器
  if (s_ping_timer != NULL) {
    xTimerDelete(s_ping_timer, 0);
    s_ping_timer = NULL;
  }

  ESP_LOGI(TAG, "4G模块反初始化完成");
  return ESP_OK;
}
