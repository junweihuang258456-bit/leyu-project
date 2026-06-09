/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
 * This is a demo for bluetooth config wifi connection to ap. You can config
 * ESP32 to connect a softap or config ESP32 as a softap to be connected by
 * other device. APP can be downloaded from github android source code:
 * https://github.com/EspressifApp/EspBlufi iOS source code:
 * https://github.com/EspressifApp/EspBlufiForiOS
 ****************************************************************************/

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "blufi_example.h"
#include "esp_blufi_api.h"

#include "esp_blufi.h"

// 添加JSON支持
#include "cJSON.h"

// 添加串口指令处理模块
#include "uart_command.h"
// 添加音频处理模块
#include "audio_processor.h"
// 添加4G模块引用
#include "usb_rndis_4g_module.h"

// 定义TAG用于日志输出
static const char *TAG = "blufi_example";

// 串口指令处理示例函数声明
void uart_command_example_init(void);

// 函数声明
static esp_err_t wifi_list_to_json(uint16_t ap_count,
                                   esp_blufi_ap_record_t *ap_list,
                                   char **json_str, size_t *json_len);
static esp_err_t create_wifi_success_json(char **json_str, size_t *json_len);
static esp_err_t create_wifi_fail_json(uint8_t type, char **json_str,
                                       size_t *json_len);

// 直接发送蓝牙通知的函数声明
static esp_err_t blufi_direct_send_json(const char *json_str, size_t json_len);

// WiFi配置检查和音频播放任务相关函数声明
bool check_wifi_config_saved(void);
static esp_err_t start_wifi_wait_audio_task(void);
static void wifi_wait_audio_task(void *pvParameters);
#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY                                  \
  10 // 为了向app报告连接失败，两次就报告
#define EXAMPLE_INVALID_REASON 255
#define EXAMPLE_INVALID_RSSI -128

// 直接设置WiFi认证模式阈值为WPA2_PSK，这是最常用的安全模式
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

static void example_event_callback(esp_blufi_cb_event_t event,
                                   esp_blufi_cb_param_t *param);
static blufi_ready_callback_t s_blufi_ready_callback = NULL;

#define WIFI_LIST_NUM 10

extern esp_blufi_callbacks_t example_callbacks;

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a
 * request */
EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t example_wifi_retry = 0;

/* store the station info for send back to phone */
bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static bool ble_is_initialized = false;

bool is_ble_initialized(void) {
    return ble_is_initialized;
}

static portMUX_TYPE s_ble_mux = portMUX_INITIALIZER_UNLOCKED;

static void safe_release_bluetooth(void) {
    bool should_release = false;
    portENTER_CRITICAL(&s_ble_mux);
    if (ble_is_initialized) {
        ble_is_initialized = false;
        should_release = true;
    }
    portEXIT_CRITICAL(&s_ble_mux);

    if (should_release) {
        BLUFI_INFO("Release Bluetooth resources safely...");
        esp_blufi_host_deinit();
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
        esp_blufi_controller_deinit();
#endif

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#elif CONFIG_IDF_TARGET_ESP32
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
        BLUFI_INFO("Bluetooth resources released successfully");
    }
}

static void release_bt_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // 延迟3秒，确保配网成功的JSON数据已经发送给APP
    safe_release_bluetooth();

    vTaskDelete(NULL);
}
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;

// 等待配网音频播放任务句柄
static TaskHandle_t wifi_wait_audio_task_handle = NULL;
volatile bool g_stop_wifi_wait_audio = false;

static void example_record_wifi_conn_info(int rssi, uint8_t reason) {
  memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
  if (gl_sta_is_connecting) {
    gl_sta_conn_info.sta_max_conn_retry_set = true;
    gl_sta_conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
  } else {
    gl_sta_conn_info.sta_conn_rssi_set = true;
    gl_sta_conn_info.sta_conn_rssi = rssi;
    gl_sta_conn_info.sta_conn_end_reason_set = true;
    gl_sta_conn_info.sta_conn_end_reason = reason;
  }
}

void example_wifi_connect(void) {
  example_wifi_retry = 0;
  gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
  example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
}

static bool example_wifi_reconnect(void) {
  bool ret;
  ESP_LOGI(TAG, "WiFi重试次数: %d", example_wifi_retry);
  if (example_wifi_retry++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
    BLUFI_INFO("BLUFI WiFi starts reconnection\n");
    // gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(10 * 1000));
    esp_wifi_connect();
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    ret = true;
  } else {
    ret = false;
  }
  return ret;
}

static int softap_get_current_connection_number(void) {
  esp_err_t ret;
  ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
  if (ret == ESP_OK) {
    return gl_sta_list.num;
  }

  return 0;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
  wifi_mode_t mode;

  switch (event_id) {
  case IP_EVENT_STA_GOT_IP: {
    esp_blufi_extra_info_t info;

    // 设置事件组标志位，表示已连接到WiFi并获取到IP
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

    // 停止等待配网音频播放任务
    if (wifi_wait_audio_task_handle != NULL) {
      g_stop_wifi_wait_audio = true;
      BLUFI_INFO("已请求停止等待配网音频播放任务\n");
    }

    esp_wifi_get_mode(&mode);

    // 初始化WiFi连接信息结构体
    memset(&info, 0, sizeof(esp_blufi_extra_info_t));
    // 复制已连接AP的BSSID到信息结构体
    memcpy(info.sta_bssid, gl_sta_bssid, 6);
    info.sta_bssid_set = true;
    // 设置SSID和SSID长度
    info.sta_ssid = gl_sta_ssid;
    info.sta_ssid_len = gl_sta_ssid_len;
    // 标记已获取到IP
    gl_sta_got_ip = true;
    if (ble_is_connected == true) {
      // 配网成功后，发送WiFi连接状态报告给app端，报告连接成功状态
      // 包含WiFi模式、连接状态、当前连接数和连接详细信息
      // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
      // esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS,
      // softap_get_current_connection_number(), &info);

      // 发送配网成功的JSON数据给APP
      char *json_str = NULL;
      size_t json_len = 0;
      esp_err_t ret = create_wifi_success_json(&json_str, &json_len);

      if (ret == ESP_OK && json_str != NULL) {
        // 分配缓冲区，包括JSON数据和0x0A终止符
        uint8_t *success_data = (uint8_t *)malloc(json_len + 1);
        if (success_data) {
          // 复制JSON数据到缓冲区
          memcpy(success_data, json_str, json_len);
          // 添加0x0A终止符
          success_data[json_len] = 0x0A;

          // 发送配网成功的JSON数据给app端
          esp_blufi_send_custom_data(success_data, json_len + 1);

          // 释放缓冲区
          free(success_data);
        } else {
          BLUFI_ERROR("Failed to allocate memory for WiFi success data\n");
        }

        // 释放JSON字符串
        free(json_str);
        
        // 蓝牙配网成功后，启动任务释放蓝牙资源
        xTaskCreatePinnedToCoreWithCaps(release_bt_task, "release_bt_task", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
      } else {
        BLUFI_ERROR("Failed to create WiFi success JSON\n");
      }
    } else {
      BLUFI_INFO("BLUFI BLE is not connected yet\n");
    }
    break;
  }
  default:
    break;
  }
  return;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  wifi_event_sta_connected_t *event;
  wifi_event_sta_disconnected_t *disconnected_event;
  wifi_mode_t mode;

  ESP_LOGI(TAG, "WiFi事件触发: %d", event_id);

  switch (event_id) {
  case WIFI_EVENT_STA_START:
    example_wifi_connect();
    break;
  case WIFI_EVENT_STA_CONNECTED:
    example_wifi_retry = 0;
    gl_sta_connected = true;
    gl_sta_is_connecting = false;
    event = (wifi_event_sta_connected_t *)event_data;
    memcpy(gl_sta_bssid, event->bssid, 6);
    memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
    gl_sta_ssid_len = event->ssid_len;
    break;
  case WIFI_EVENT_STA_DISCONNECTED: {
    ESP_LOGI(TAG, "WiFi断开连接事件触发");
    disconnected_event = (wifi_event_sta_disconnected_t *)event_data;
    uint8_t reason = disconnected_event->reason;
    ESP_LOGI(TAG, "断开原因代码: %d", reason);
    
    // 1. 判断是否为密码错误/认证失败
    bool is_pwd_error = false;
    if (reason == WIFI_REASON_AUTH_EXPIRE || 
        reason == WIFI_REASON_AUTH_LEAVE || 
        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || 
        reason == WIFI_REASON_AUTH_FAIL || 
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        is_pwd_error = true;
        ESP_LOGE(TAG, "检测到WiFi密码错误或认证失败！");
        // 强制终止重连，避免死等
        example_wifi_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    }

    /* Only handle reconnection during connecting */
    if (example_wifi_reconnect() == false && gl_sta_connected == false) {
      gl_sta_is_connecting = false;
      example_record_wifi_conn_info(disconnected_event->rssi, reason);
      
      if (is_pwd_error) {
          // 播放密码错误音频
          audio_prompt_play("file:///spiffs/wifierror.mp3");
          ESP_LOGW(TAG, "因密码错误，即将清除无效WiFi配置并拉起蓝牙...");
          // 清除NVS中错误的WiFi配置
          wifi_config_t empty_config = {0};
          esp_wifi_set_config(WIFI_IF_STA, &empty_config);
          
          // 如果蓝牙未开启，说明是开机自动连失败的，此时必须拉起蓝牙配网，否则设备死锁
          if (!ble_is_initialized) {
              ESP_LOGI(TAG, "正在启动蓝牙配网服务...");
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
              esp_err_t ret_ble = esp_blufi_controller_init();
              if (ret_ble) {
                  ESP_LOGE(TAG, "蓝牙控制器初始化失败: %s", esp_err_to_name(ret_ble));
              }
#endif
              esp_err_t host_ret = esp_blufi_host_and_cb_init(&example_callbacks);
              if (host_ret == ESP_OK) {
                  ble_is_initialized = true;
                  ESP_LOGI(TAG, "蓝牙配网服务拉起成功，等待手机连接！");
                  
                  // 启动等待配网音频播放任务
                  start_wifi_wait_audio_task();
              }
          }
      } else {
          // 播放普通WiFi连接失败音频
          audio_prompt_play("file:///spiffs/wifi_fail.mp3");
      }

      // 如果BLE已连接，发送配网失败类型的JSON数据给APP
      if (ble_is_connected == true) {
        // 根据错误类型反馈给APP
        uint8_t fail_type = is_pwd_error ? 1 : 0; // 1: 密码错误, 0: 普通失败
        
        BLUFI_INFO("发送连接失败JSON给APP (type=%d)\n", fail_type);
        
        // 创建并发送配网失败JSON数据
        char *json_str = NULL;
        size_t json_len = 0;
        esp_err_t ret = create_wifi_fail_json(fail_type, &json_str, &json_len);

        if (ret == ESP_OK && json_str != NULL) {
          // 分配缓冲区，包括JSON数据和0x0A终止符
          uint8_t *fail_data = (uint8_t *)malloc(json_len + 1);
          if (fail_data) {
            // 复制JSON数据到缓冲区
            memcpy(fail_data, json_str, json_len);
            // 添加0x0A终止符
            fail_data[json_len] = 0x0A;

            // 发送配网失败类型的JSON数据给app端
            esp_blufi_send_custom_data(fail_data, json_len + 1);

            // 释放缓冲区
            free(fail_data);
          } else {
            BLUFI_ERROR("Failed to allocate memory for WiFi fail data\n");
          }

          // 释放JSON字符串
          free(json_str);
        } else {
          BLUFI_ERROR("Failed to create WiFi fail JSON\n");
        }
      }
    }
    /* This is a workaround as ESP32 WiFi libs don't currently
       auto-reassociate. */
    gl_sta_connected = false;
    gl_sta_got_ip = false;
    memset(gl_sta_ssid, 0, 32);
    memset(gl_sta_bssid, 0, 6);
    gl_sta_ssid_len = 0;
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    break;
  }
  case WIFI_EVENT_AP_START:
    esp_wifi_get_mode(&mode);

    /* TODO: get config or information of softap, then set to report extra_info
     */
    if (ble_is_connected == true) {
      if (gl_sta_connected) {
        esp_blufi_extra_info_t info;
        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, gl_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid;
        info.sta_ssid_len = gl_sta_ssid_len;
        // 配网成功后，发送WiFi连接状态报告给app端，报告已连接状态
        // 根据是否获取到IP，报告成功连接或已连接但未获取IP的状态
        // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
        // esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ?
        // ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP,
        // softap_get_current_connection_number(), &info);
      } else if (gl_sta_is_connecting) {
        // 发送WiFi连接状态报告给app端，报告正在连接中状态
        // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
        // esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING,
        // softap_get_current_connection_number(), &gl_sta_conn_info);
      } else {
        // 发送WiFi连接状态报告给app端，报告连接失败状态
        // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
        // esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
        // softap_get_current_connection_number(), &gl_sta_conn_info);

        // 修改：所有连接失败都设置为密码错误(type=1)
        uint8_t fail_type = 1; // 密码错误

        // 打印日志，确认代码被执行
        BLUFI_INFO("WiFi connection failed in WIFI_EVENT_AP_START, sending "
                   "password error JSON (type=1)\n");

        // 播放WiFi连接失败音频
        audio_prompt_play("file:///spiffs/wifi_fail.mp3"); // 播放wifi连接失败

        // 创建并发送配网失败JSON数据
        char *json_str = NULL;
        size_t json_len = 0;
        esp_err_t ret = create_wifi_fail_json(fail_type, &json_str, &json_len);

        if (ret == ESP_OK && json_str != NULL) {
          // 分配缓冲区，包括JSON数据和0x0A终止符
          uint8_t *fail_data = (uint8_t *)malloc(json_len + 1);
          if (fail_data) {
            // 复制JSON数据到缓冲区
            memcpy(fail_data, json_str, json_len);
            // 添加0x0A终止符
            fail_data[json_len] = 0x0A;

            // 发送配网失败类型的JSON数据给app端
            esp_blufi_send_custom_data(fail_data, json_len + 1);

            // 释放缓冲区
            free(fail_data);
          } else {
            BLUFI_ERROR("Failed to allocate memory for WiFi fail data\n");
          }

          // 释放JSON字符串
          free(json_str);
        } else {
          BLUFI_ERROR("Failed to create WiFi fail JSON\n");
        }
      }
    } else {
      BLUFI_INFO("BLUFI BLE is not connected yet\n");
    }
    break;
  case WIFI_EVENT_SCAN_DONE: {
    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount == 0) {
      BLUFI_INFO("Nothing AP found");
      break;
    }
    wifi_ap_record_t *ap_list =
        (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
    if (!ap_list) {
      BLUFI_INFO("Nothing AP found");
      esp_wifi_clear_ap_list();
      break;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
    esp_blufi_ap_record_t *blufi_ap_list = (esp_blufi_ap_record_t *)malloc(
        apCount * sizeof(esp_blufi_ap_record_t));
    if (!blufi_ap_list) {
      if (ap_list) {
        free(ap_list);
      }
      BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
      break;
    }
    for (int i = 0; i < apCount; ++i) {
      blufi_ap_list[i].rssi = ap_list[i].rssi;
      memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
    }

    if (ble_is_connected == true) {
      // 将WiFi列表转换为JSON格式
      char *json_str = NULL;
      size_t json_len = 0;
      esp_err_t ret =
          wifi_list_to_json(apCount, blufi_ap_list, &json_str, &json_len);

      if (ret == ESP_OK && json_str != NULL) {
        // 直接发送JSON数据，绕过BLUFI协议封装
        // 使用简单的数据发送机制，避免加密和协议封装
        BLUFI_INFO("准备发送WiFi列表JSON数据，长度: %d", json_len);

        // 直接发送JSON字符串，不添加0x0A终止符，避免BLUFI协议处理
        // 使用esp_blufi_send_custom_data但关闭加密和校验
        esp_blufi_send_custom_data((uint8_t *)json_str, json_len);

        BLUFI_INFO("WiFi列表JSON数据发送完成");

        // 释放JSON字符串
        free(json_str);
      } else {
        BLUFI_ERROR(
            "Failed to convert WiFi list to JSON, using original format\n");
        // 如果JSON转换失败，使用原始格式
        esp_blufi_send_wifi_list(apCount, blufi_ap_list);
      }
    } else {
      BLUFI_INFO("BLUFI BLE is not connected yet\n");
    }

    esp_wifi_scan_stop();
    free(ap_list);
    free(blufi_ap_list);
    break;
  }
  case WIFI_EVENT_AP_STACONNECTED: {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    BLUFI_INFO("station " MACSTR " join, AID=%d", MAC2STR(event->mac),
               event->aid);
    break;
  }
  case WIFI_EVENT_AP_STADISCONNECTED: {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    BLUFI_INFO("station " MACSTR " leave, AID=%d, reason=%d",
               MAC2STR(event->mac), event->aid, event->reason);
    break;
  }

  default:
    break;
  }
  return;
}

static void initialise_wifi(void) {
  // 检查网络接口是否已经初始化
  esp_err_t netif_err = esp_netif_init();
  if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(netif_err);
  }

  wifi_event_group = xEventGroupCreate();

  // 检查事件循环是否已经存在
  esp_err_t loop_err = esp_event_loop_create_default();
  if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_err);
  } else if (loop_err == ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "默认事件循环已存在，跳过创建");
  }

  // 创建WiFi STA接口（如果不存在）
  esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta_netif) {
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
  }

  // 创建WiFi AP接口（如果不存在）
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap_netif) {
    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
  }

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &ip_event_handler, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // 设置为AP+STA双模式，支持同时连接WiFi和发射WiFi
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // 初始化AP配置，使用与app_wifi_main()相同的配置
  //   memset(&ap_config, 0, sizeof(ap_config));
  //   strcpy((char *)ap_config.ap.ssid,
  //          "ESP-USB-4G"); // 与CONFIG_ESP_WIFI_AP_SSID保持一致
  //   ap_config.ap.ssid_len = strlen("ESP-USB-4G");
  //   ap_config.ap.channel = 1;
  //   ap_config.ap.authmode = WIFI_AUTH_OPEN; // 开放网络，与空密码配置保持一致
  //   ap_config.ap.max_connection = 4;
  //   ap_config.ap.beacon_interval = 100;
  //   ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi初始化完成，AP+STA双模式已启用");
  if (s_blufi_ready_callback != NULL) {
    s_blufi_ready_callback();
  }
}

void blufi_register_ready_callback(blufi_ready_callback_t callback) {
  s_blufi_ready_callback = callback;
}

esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

static void example_event_callback(esp_blufi_cb_event_t event,
                                   esp_blufi_cb_param_t *param) {
  /* actually, should post to blufi_task handle the procedure,
   * now, as a example, we do it more simply */
  switch (event) {
  case ESP_BLUFI_EVENT_INIT_FINISH:
    BLUFI_INFO("BLUFI init finish\n");

    esp_blufi_adv_start();
    break;
  case ESP_BLUFI_EVENT_DEINIT_FINISH:
    BLUFI_INFO("BLUFI deinit finish\n");
    break;
  case ESP_BLUFI_EVENT_BLE_CONNECT:
    BLUFI_INFO("BLUFI ble connect\n");
    ble_is_connected = true;
    esp_blufi_adv_stop();
    blufi_security_init();
    break;
  case ESP_BLUFI_EVENT_BLE_DISCONNECT:
    BLUFI_INFO("BLUFI ble disconnect\n");
    ble_is_connected = false;
    blufi_security_deinit();
    esp_blufi_adv_start();
    break;
  case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
    BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
    ESP_ERROR_CHECK(esp_wifi_set_mode(param->wifi_mode.op_mode));
    break;
  case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
    BLUFI_INFO("BLUFI request wifi connect to AP\n");
    /* there is no wifi callback when the device has already connected to this
    wifi so disconnect wifi before connection.
    */
    esp_wifi_disconnect();
    example_wifi_connect();
    break;
  case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
    BLUFI_INFO("BLUFI request wifi disconnect from AP\n");
    esp_wifi_disconnect();
    break;
  case ESP_BLUFI_EVENT_REPORT_ERROR:
    BLUFI_ERROR("BLUFI report error, error code %d\n",
                param->report_error.state);
    // 配网过程中发生错误时，发送错误信息给app端，报告具体的错误状态
    esp_blufi_send_error_info(param->report_error.state);
    break;
  case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
    wifi_mode_t mode;
    esp_blufi_extra_info_t info;

    esp_wifi_get_mode(&mode);

    if (gl_sta_connected) {
      memset(&info, 0, sizeof(esp_blufi_extra_info_t));
      memcpy(info.sta_bssid, gl_sta_bssid, 6);
      info.sta_bssid_set = true;
      info.sta_ssid = gl_sta_ssid;
      info.sta_ssid_len = gl_sta_ssid_len;
      // 配网过程中，当APP请求WiFi状态时，发送WiFi连接状态报告给app端，报告已连接状态
      // 根据是否获取到IP，报告成功连接或已连接但未获取IP的状态
      // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
      // esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ?
      // ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP,
      // softap_get_current_connection_number(), &info);
    } else if (gl_sta_is_connecting) {
      // 配网过程中，当APP请求WiFi状态时，发送WiFi连接状态报告给app端，报告正在连接中状态
      // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
      // esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING,
      // softap_get_current_connection_number(), &gl_sta_conn_info);
    } else {
      // 配网过程中，当APP请求WiFi状态时，发送WiFi连接状态报告给app端，报告连接失败状态
      // 注释：取消发送type=0x3d的非标准自定义数据，仅保留JSON格式数据
      // esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
      // softap_get_current_connection_number(), &gl_sta_conn_info);

      // 修改：所有连接失败都设置为密码错误(type=1)
      uint8_t fail_type = 1; // 密码错误

      // 打印日志，确认代码被执行
      BLUFI_INFO("WiFi connection failed in ESP_BLUFI_EVENT_GET_WIFI_STATUS, "
                 "sending password error JSON (type=1)\n");

      // 创建并发送配网失败JSON数据
      char *json_str = NULL;
      size_t json_len = 0;
      esp_err_t ret = create_wifi_fail_json(fail_type, &json_str, &json_len);

      if (ret == ESP_OK && json_str != NULL) {
        // 分配缓冲区，包括JSON数据和0x0A终止符
        uint8_t *fail_data = (uint8_t *)malloc(json_len + 1);
        if (fail_data) {
          // 复制JSON数据到缓冲区
          memcpy(fail_data, json_str, json_len);
          // 添加0x0A终止符
          fail_data[json_len] = 0x0A;

          // 发送配网失败类型的JSON数据给app端
          esp_blufi_send_custom_data(fail_data, json_len + 1);

          // 释放缓冲区
          free(fail_data);
        } else {
          BLUFI_ERROR("Failed to allocate memory for WiFi fail data\n");
        }

        // 释放JSON字符串
        free(json_str);
      } else {
        BLUFI_ERROR("Failed to create WiFi fail JSON\n");
      }
    }
    BLUFI_INFO("BLUFI get wifi status from AP\n");

    break;
  }
  case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
    BLUFI_INFO("blufi close a gatt connection");
    // 注释掉主动断开蓝牙连接的代码，以便在发送密码错误JSON后保持连接，允许用户在APP端修改密码
    // esp_blufi_disconnect();
    // 停止等待配网音频播放任务
    if (wifi_wait_audio_task_handle != NULL) {
      g_stop_wifi_wait_audio = true;
      BLUFI_INFO("已请求停止等待配网音频播放任务\n");
    }
    break;
  case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
    /* TODO */
    break;
  case ESP_BLUFI_EVENT_RECV_STA_BSSID:
    memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
    sta_config.sta.bssid_set = 1;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    BLUFI_INFO("Recv STA BSSID %s\n", sta_config.sta.ssid);
    break;
  case ESP_BLUFI_EVENT_RECV_STA_SSID:
    if (param->sta_ssid.ssid_len >=
        sizeof(sta_config.sta.ssid) / sizeof(sta_config.sta.ssid[0])) {
      // 配网过程中，当接收到无效SSID长度时，发送错误信息给app端，报告数据格式错误
      esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
      BLUFI_INFO("Invalid STA SSID\n");
      break;
    }
    strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid,
            param->sta_ssid.ssid_len);
    sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    BLUFI_INFO("Recv STA SSID %s\n", sta_config.sta.ssid);
    break;
  case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
    if (param->sta_passwd.passwd_len >=
        sizeof(sta_config.sta.password) / sizeof(sta_config.sta.password[0])) {
      // 配网过程中，当接收到无效密码长度时，发送错误信息给app端，报告数据格式错误
      esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
      BLUFI_INFO("Invalid STA PASSWORD\n");
      break;
    }
    strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd,
            param->sta_passwd.passwd_len);
    sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
    sta_config.sta.threshold.authmode = EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    BLUFI_INFO("Recv STA PASSWORD %s\n", sta_config.sta.password);
    // 停止等待配网音频播放任务
    if (wifi_wait_audio_task_handle != NULL) {
      g_stop_wifi_wait_audio = true;
      BLUFI_INFO("已请求停止等待配网音频播放任务\n");
    }
    break;
  case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
    if (param->softap_ssid.ssid_len >=
        sizeof(ap_config.ap.ssid) / sizeof(ap_config.ap.ssid[0])) {
      // 配网过程中，当接收到无效SoftAP
      // SSID长度时，发送错误信息给app端，报告数据格式错误
      esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
      BLUFI_INFO("Invalid SOFTAP SSID\n");
      break;
    }
    strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid,
            param->softap_ssid.ssid_len);
    ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
    ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid,
               ap_config.ap.ssid_len);
    break;
  case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
    if (param->softap_passwd.passwd_len >=
        sizeof(ap_config.ap.password) / sizeof(ap_config.ap.password[0])) {
      // 配网过程中，当接收到无效SoftAP密码长度时，发送错误信息给app端，报告数据格式错误
      esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
      BLUFI_INFO("Invalid SOFTAP PASSWD\n");
      break;
    }
    strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd,
            param->softap_passwd.passwd_len);
    ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    BLUFI_INFO("Recv SOFTAP PASSWORD %s len = %d\n", ap_config.ap.password,
               param->softap_passwd.passwd_len);
    break;
  case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
    if (param->softap_max_conn_num.max_conn_num > 4) {
      return;
    }
    ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
    break;
  case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
    if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
      return;
    }
    ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    BLUFI_INFO("Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
    break;
  case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
    if (param->softap_channel.channel > 13) {
      return;
    }
    ap_config.ap.channel = param->softap_channel.channel;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    BLUFI_INFO("Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
    break;
  case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
    wifi_scan_config_t scanConf = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false};
    esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
    if (ret != ESP_OK) {
      // 配网过程中，当WiFi扫描失败时，发送错误信息给app端，报告扫描失败
      esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
    }
    break;
  }
  case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
    BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
    ESP_LOG_BUFFER_HEX("Custom Data", param->custom_data.data,
                       param->custom_data.data_len);
    break;
  case ESP_BLUFI_EVENT_RECV_USERNAME:
    /* Not handle currently */
    break;
  case ESP_BLUFI_EVENT_RECV_CA_CERT:
    /* Not handle currently */
    break;
  case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
    /* Not handle currently */
    break;
  case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
    /* Not handle currently */
    break;
  case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
    /* Not handle currently */
    break;
    ;
  case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
    /* Not handle currently */
    break;
  default:
    break;
  }
}

#include "brtc_app.h"
bool wifi_configured = false;
void blufi_init(void) {
  esp_err_t ret;

  initialise_wifi();

  BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());

  // 检查WiFi配置状态
  wifi_configured = check_wifi_config_saved();

  if (wifi_configured) {
    // WiFi已配置，不初始化蓝牙功能
    BLUFI_INFO("WiFi已配置，不初始化蓝牙功能\n");
    audio_pipe_sip_mode(false);
  } else {
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret) {
      BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__,
                  esp_err_to_name(ret));
      return;
    }
#endif

    ret = esp_blufi_host_and_cb_init(&example_callbacks);
    if (ret) {
      BLUFI_ERROR("%s initialise failed: %s\n", __func__, esp_err_to_name(ret));
      return;
    }
    ble_is_initialized = true;
    
    // WiFi未配置，启动等待配网音频播放任务
    BLUFI_INFO("WiFi未配置，启动等待配网音频播放任务\n");
    start_wifi_wait_audio_task();
  }
  
  esp_err_t connect_err = wait_wifi_connect();
  if (connect_err != ESP_OK) {
      BLUFI_ERROR("等待网络连接失败或超时 (错误码: %d)，系统将继续运行并尝试依靠4G网络或后续重连\n", connect_err);
  } else {
      BLUFI_INFO("网络连接等待完成\n");
  }
}

/**
 * @brief 将WiFi列表转换为JSON格式字符串，用于配网过程中发送给APP
 *
 * @param ap_count WiFi数量
 * @param ap_list WiFi列表
 * @param json_str 输出的JSON字符串
 * @param json_len JSON字符串长度
 * @return esp_err_t
 */
static esp_err_t wifi_list_to_json(uint16_t ap_count,
                                   esp_blufi_ap_record_t *ap_list,
                                   char **json_str, size_t *json_len) {
  cJSON *root = NULL;
  cJSON *wifis = NULL;
  cJSON *wifi_item = NULL;
  char *ssid_str = NULL;

  if (ap_count == 0 || ap_list == NULL) {
    BLUFI_ERROR("Invalid parameters for wifi_list_to_json\n");
    return ESP_ERR_INVALID_ARG;
  }

  // 创建JSON根对象，用于构建WiFi列表数据结构
  root = cJSON_CreateObject();
  if (root == NULL) {
    BLUFI_ERROR("Failed to create JSON root object\n");
    return ESP_ERR_NO_MEM;
  }

  // 创建WiFi数组，用于存储所有扫描到的WiFi信息
  wifis = cJSON_CreateArray();
  if (wifis == NULL) {
    BLUFI_ERROR("Failed to create JSON array\n");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }

  // 添加每个WiFi到数组
  for (int i = 0; i < ap_count; i++) {
    wifi_item = cJSON_CreateObject();
    if (wifi_item == NULL) {
      BLUFI_ERROR("Failed to create WiFi item object\n");
      cJSON_Delete(wifis);
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }

    // 添加SSID
    ssid_str = malloc(strlen((char *)ap_list[i].ssid) + 1);
    if (ssid_str == NULL) {
      BLUFI_ERROR("Failed to allocate memory for SSID\n");
      cJSON_Delete(wifi_item);
      cJSON_Delete(wifis);
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    strcpy(ssid_str, (char *)ap_list[i].ssid);
    cJSON_AddStringToObject(wifi_item, "ssid", ssid_str);
    free(ssid_str);

    // 添加RSSI（信号强度）
    cJSON_AddNumberToObject(wifi_item, "rssi", ap_list[i].rssi);

    // 添加isPw（是否需要密码），由于esp_blufi_ap_record_t没有authmode字段，暂时设置为3(加密)
    cJSON_AddNumberToObject(wifi_item, "isPw", 3);

    // 将WiFi项添加到数组
    cJSON_AddItemToArray(wifis, wifi_item);
  }

  // 将WiFi数组添加到根对象
  cJSON_AddItemToObject(root, "wifis", wifis);

  // 将JSON对象转换为字符串，用于发送给APP
  *json_str = cJSON_PrintUnformatted(root);
  if (*json_str == NULL) {
    BLUFI_ERROR("Failed to print JSON to string\n");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }

  *json_len = strlen(*json_str);

  // 释放JSON对象内存
  cJSON_Delete(root);

  return ESP_OK;
}

/**
 * @brief 创建配网成功类型的JSON数据
 *
 * @param json_str 输出参数，返回创建的JSON字符串
 * @param json_len 输出参数，返回JSON字符串的长度
 * @return esp_err_t ESP_OK成功，其他值失败
 */
char device_imei_bl[25];
static esp_err_t create_wifi_success_json(char **json_str, size_t *json_len) {
  if (json_str == NULL || json_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // 获取设备MAC地址
  uint8_t mac[6];
  esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (ret != ESP_OK) {
    BLUFI_ERROR("Failed to get MAC address\n");
    return ret;
  }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    BLUFI_ERROR("Failed to create JSON object\n");
    return ESP_ERR_NO_MEM;
  }

  // 添加type字段，配网成功为0
  cJSON *type_item = cJSON_CreateNumber(0);
  if (type_item == NULL) {
    BLUFI_ERROR("Failed to create type item\n");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(root, "type", type_item);

  // 添加mac字段，格式化为12位大写十六进制字符串
  char mac_str[13] = {0};
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  cJSON *mac_item = cJSON_CreateString(mac_str);
  if (mac_item == NULL) {
    BLUFI_ERROR("Failed to create mac item\n");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(root, "mac", mac_item);

  // 尝试从NVS读取SN码,使用device_imei作为SN码字段
  nvs_handle_t nvs_imei_handle_local = 0;
  bool imei_read_success = false;
  device_imei_bl[0] = '\0';

  ret = nvs_open("key_bind", NVS_READONLY, &nvs_imei_handle_local);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
  } else {
    // 从NVS读取IMEI码
    size_t length = sizeof(device_imei_bl);
    ret = nvs_get_str(nvs_imei_handle_local, "imei", device_imei_bl, &length);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "从NVS读取IMEI失败: %s", esp_err_to_name(ret));
      device_imei_bl[0] = '\0';
    } else {
      ESP_LOGI(TAG, "从NVS读取到IMEI码: %s", device_imei_bl);
      imei_read_success = true;
    }
    ESP_LOGI(TAG, "关闭NVS句柄");
    nvs_close(nvs_imei_handle_local);
  }

  // 如果SN码存在，则添加到JSON中
  if (imei_read_success && strlen(device_imei_bl) > 0) {
    BLUFI_INFO("从NVS读取到SN码: %s", device_imei_bl);
    cJSON *sn_item = cJSON_CreateString(device_imei_bl);
    if (sn_item == NULL) {
      BLUFI_ERROR("Failed to create sn item\n");
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "sn", sn_item);
  } else {
    BLUFI_INFO("NVS中未找到SN码，使用原始JSON格式");
  }

  // 转换为字符串
  char *str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (str == NULL) {
    BLUFI_ERROR("Failed to print JSON\n");
    return ESP_ERR_NO_MEM;
  }

  *json_str = str;
  *json_len = strlen(str);

  return ESP_OK;
}

/**
 * @brief 创建配网失败类型的JSON格式字符串，用于配网失败时发送给APP
 *
 * @param type 失败类型：1-密码错误，2-添加设备失败，99-其他异常
 * @param json_str 输出的JSON字符串
 * @param json_len JSON字符串长度
 * @return esp_err_t
 */
static esp_err_t create_wifi_fail_json(uint8_t type, char **json_str,
                                       size_t *json_len) {
  cJSON *root = NULL;

  // 获取设备MAC地址
  uint8_t mac[6];
  esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (ret != ESP_OK) {
    BLUFI_ERROR("Failed to get MAC address\n");
    return ret;
  }

  // 创建JSON根对象
  root = cJSON_CreateObject();
  if (root == NULL) {
    BLUFI_ERROR("Failed to create JSON root object for wifi fail\n");
    return ESP_ERR_NO_MEM;
  }

  // 添加失败类型
  cJSON_AddNumberToObject(root, "type", type);

  // 添加mac字段，格式化为12位大写十六进制字符串
  char mac_str[13] = {0};
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  cJSON_AddStringToObject(root, "mac", mac_str);

  // 将JSON对象转换为字符串
  *json_str = cJSON_PrintUnformatted(root);
  if (*json_str == NULL) {
    BLUFI_ERROR("Failed to print wifi fail JSON to string\n");
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }

  *json_len = strlen(*json_str);

  // 释放JSON对象内存
  cJSON_Delete(root);

  return ESP_OK;
}

extern bool is_sip_mode;
static void wifi_wait_audio_task(void *pvParameters) {
  // 每隔10秒检查一次WiFi是否已配置
  g_stop_wifi_wait_audio = false; // 启动时重置标志
  while (check_wifi_config_saved() == false && !g_stop_wifi_wait_audio) {
    ESP_LOGI(TAG, "WiFi未配置，播放等待配网音频");
    audio_playback_play("file:///spiffs/wifi_wait.mp3");
    
    // 将10秒延时分解为多次小延时，以便及时响应停止标志
    for (int i = 0; i < 100; i++) {
        if (check_wifi_config_saved() == true || g_stop_wifi_wait_audio) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  ESP_LOGI(TAG, "退出等待配网音频任务，准备释放音频资源");
  // 【关键修复死锁的地方】：在强行销毁管线前，必须先停止当前的音频播放任务
  audio_playback_stop(); 
  vTaskDelay(pdMS_TO_TICKS(100)); // 给播放器一点点缓冲时间来释放 I2S 或混音器资源
  
  audio_pipe_sip_mode(false);
  // 任务完成前将句柄设置为NULL
  wifi_wait_audio_task_handle = NULL;

  // 任务完成后删除自身
  vTaskDelete(NULL);
}

/**
 * @brief 启动等待配网音频播放任务
 *
 * @return esp_err_t
 */
static esp_err_t start_wifi_wait_audio_task(void) {
  BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(wifi_wait_audio_task, "wifi_wait_audio", 4096,
                               NULL, 5, &wifi_wait_audio_task_handle, 1, MALLOC_CAP_SPIRAM);
  if (ret != pdPASS) {
    BLUFI_ERROR("创建等待配网音频任务失败\n");
    return ESP_FAIL;
  }

  return ESP_OK;
}

/**
 * @brief 检查Flash中是否保存了WiFi配置信息
 *
 * @return true WiFi配置已保存
 * @return false WiFi配置未保存
 */
bool check_wifi_config_saved(void) {
  wifi_config_t conf;
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &conf);
  if (err != ESP_OK) {
    return false;
  }
  if (strlen((char *)conf.sta.ssid) > 0) {
    return true;
  } else {
    static bool printed = false;
    if (!printed) {
      BLUFI_INFO("Flash中未保存WiFi配置\n");
      printed = true;
    }
    return false;
  }
}

/**
 * @brief 等待WiFi连接成功或4G网络连接成功(带90秒窗口期)
 * 使用非阻塞方式，不会影响其他任务(如蓝牙)的执行
 *
 * @return esp_err_t
 *         - ESP_OK: WiFi或4G网络已连接成功
 */
esp_err_t wait_wifi_connect(void) {
  // 检查事件组是否已初始化，防止blufi任务未加载时引发异常
  if (wifi_event_group == NULL) {
    BLUFI_INFO("WiFi事件组未初始化，仅检查4G网络连接状态\n");

    // 如果WiFi事件组未初始化，只检查4G网络连接状态
    while (internet_connected == 0) {
      BLUFI_INFO("等待4G网络连接...\n");
      vTaskDelay(2000 / portTICK_PERIOD_MS); // 每2秒检查一次
    }

    BLUFI_INFO("4G网络连接成功\n");
    return ESP_OK;
  }

  int wait_seconds = 0;
  // 循环检查WiFi连接状态和4G网络连接状态
  while (1) {
    // 检查CONNECTED_BIT是否被设置
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);

    // 检查WiFi连接状态 (WiFi连接成功可以立即放行)
    if (bits & CONNECTED_BIT) {
      BLUFI_INFO("WiFi连接成功\n");
      return ESP_OK;
    }

    // 4G连接成功的情况
    if (internet_connected == 1) {
      // 如果已经配网（老设备），4G就绪即可立即放行
      if (check_wifi_config_saved()) {
        BLUFI_INFO("设备已配网且4G网络连接成功\n");
        return ESP_OK;
      }
    }
    
    // 如果达到了90秒的配网窗口期
    if (wait_seconds >= 90) {
        if (internet_connected == 1) {
             BLUFI_INFO("90秒配网窗口期结束，4G已就绪，直接使用4G网络！\n");
             // 停止配网提示音，设置标志位让任务安全退出
             if (wifi_wait_audio_task_handle != NULL) {
                 g_stop_wifi_wait_audio = true;
             }
             return ESP_OK;
        } else if (wait_seconds == 90) {
             // 只有在恰好第90秒时触发一次硬件复位
             BLUFI_INFO("90秒配网窗口期结束，但4G尚未就绪，执行4G模块硬件复位并继续等待网络...\n");
             extern void reset_4g_module_hardware(void);
             reset_4g_module_hardware();
        } else if (wait_seconds % 10 == 0) {
             BLUFI_INFO("继续等待网络...\n");
        }
    }

    // 如果未连接，等待一段时间再检查
    // 使用vTaskDelay让出CPU，允许其他任务(如蓝牙)执行
    vTaskDelay(2000 / portTICK_PERIOD_MS); // 每2秒检查一次
    wait_seconds += 2;
  }
}
