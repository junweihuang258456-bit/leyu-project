/**
 * @file mqtt_example.c
 * @brief 专项市场接口规范MQTT接入实现
 *
 * 该文件实现了专项市场接口规范中的所有MQTT收发逻辑，包括：
 * 1. 设备指令完成反馈接口 (/device/specific/rui/send/response)
 * 2. 服务端指令完成反馈接口 (/device/specific/app/send/response/{imei})
 * 3. 语音拦截规则下发接口 (/device/specific/app/gsm/rules/{imei})
 * 4. 语音呼叫指令接口 (/device/specific/app/gsm/call/{imei})
 * 5. 短信发送指令接口 (/device/specific/app/sms/call/{imei})
 * 6. AT指令发送接口 (/device/specific/app/at/call/{imei})
 * 7. 语音来电接口 (/device/specific/rui/gsm/call)
 * 8. 短信来信接口 (/device/specific/rui/sms/call)
 *
 * @author Espressif Systems
 * @version 1.0
 * @date 2023
 */

#include "at_3gpp_ts_27_007.h"
#include "audio_processor.h"
#include "baidu_rtc_client.h"
#include "brtc_app.h"
#include "cJSON.h"
#include "call_manager.h"
#include "contact_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_g711_dec.h"
#include "esp_gmf_oal_mem.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "mqtt5_client.h"
#include "mqtt_msg_parser.h"
#include "mqtt_msg_builder.h"
#include "nvs.h"

/* Explicitly forward declare these to fix the implicit declaration compiler issue */
extern void parse_sip_info(const char *json_str);
extern void parse_service_status(const char *json_str);
extern void parse_server_command_response(const char *json_str);
extern void parse_voice_intercept_rules(const char *json_str);
extern void parse_voice_call_command(const char *json_str);
extern void parse_sms_send_command(const char *json_str);
extern void parse_at_command(const char *json_str);
extern void parse_key_bind_message(const char *json_str);
extern void parse_ota_upgrade_command(const char *json_str);
extern void parse_contact_sync_message(const char *json_str);

extern void send_voice_call_notification(const char *phone, const char *status);
extern void send_sms_notification(const char *phone, const char *content);
#include "nvs_flash.h"
#include "ota.h"
#include "protocol_examples_common.h"
#include "uart_comm.h"
#include "uart_command.h"
#include "usb_rndis_4g_module.h"
#include "network_manager.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

nvs_handle_t nvs_mqtt_handle;

// 声明外部函数，用于更新PSRAM中的号码数组
extern esp_err_t init_phone_numbers_from_flash(void);
extern const char *get_phone_number_by_key(int key_index);

// 声明外部固件版本号变量
extern const char *firmware_version;

// 定义AT上下文结构体，与usb_rndis_4g_module.c中的定义一致
typedef struct {
  void *cdc_port;        /*!< CDC port handle */
  at_handle_t at_handle; /*!< AT command parser handle */
} at_ctx_t;

// 声明外部AT上下文和缓冲区变量
extern at_ctx_t g_at_ctx;
extern char *g_at_command_buffer;
extern char *g_at_response_buffer;

static const char *TAG = "mqtt_example";
static bool mqtt_subscribed = false; // 添加订阅标志，避免重复订阅

/* WiFi事件组 */
static EventGroupHandle_t s_wifi_event_group;

/* WiFi事件标志 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* 最大重试次数 */
#define MAX_RETRY 5

/* 重试计数器 */
static int s_retry_num = 0;

/* 中断任务事件组句柄 */
static EventGroupHandle_t s_isr_event_group = NULL;

/* 设备IMEI号，实际应用中应从设备获取 */
char device_imei[25]; // IMEI最多15位，加上结束符21位

extern at_handle_t g_at_handle;

static int parse_cgsn_imei_strict(const char *at_response, char *imei_out,
                                  int max_len) {
  const char *prefix = "+CGSN: ";
  char *p = strstr(at_response, prefix);
  if (!p)
    return -1;

  p += strlen(prefix);

  // 只提取 0-9 的数字
  int idx = 0;
  while (*p != '\0' && idx < max_len - 1) {
    if (*p >= '0' && *p <= '9') {
      imei_out[idx++] = *p;
    } else {
      break; // 遇到非数字停止
    }
    p++;
  }
  imei_out[idx] = '\0';

  // IMEI 必须是 15 位
  if (idx != 15)
    return -1;

  return 0;
}

nvs_handle_t nvs_imei_handle;
// 获取4G模块的IMEI号
static void get_device_imei(void) {
  if (g_at_handle == NULL) {
    ESP_LOGE(TAG, "AT句柄未初始化，无法获取IMEI");
    return;
  }

  ESP_LOGI(TAG, "获取4G模块IMEI号...");

  esp_err_t ret =
      at_cmd_get_imei_number(g_at_handle, device_imei, sizeof(device_imei));
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "原始IMEI: %s", device_imei);
    // 使用严格的IMEI解析函数提取数字部分
    char temp_imei[16] = {0};
    if (parse_cgsn_imei_strict(device_imei, temp_imei, sizeof(temp_imei)) ==
        0) {
      strcpy(device_imei, temp_imei);
    }
    ESP_LOGI(TAG, "获取IMEI成功: %s", device_imei);

    ret = nvs_open("key_bind", NVS_READWRITE, &nvs_imei_handle);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
      goto exit;
    }
    // 写入IMEI到NVS
    ret = nvs_set_str(nvs_imei_handle, "imei", device_imei);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "写入IMEI到NVS失败: %s", esp_err_to_name(ret));
      goto exit;
    }
    ret = nvs_commit(nvs_imei_handle);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "提交NVS失败: %s", esp_err_to_name(ret));
      goto exit;
    }
    ESP_LOGI(TAG, "IMEI写入NVS成功");
  exit:
    // 关闭NVS句柄
    ESP_LOGI(TAG, "关闭NVS句柄");
    nvs_close(nvs_imei_handle);

  } else {
    ESP_LOGE(TAG, "获取IMEI失败: %s", esp_err_to_name(ret));
    // 如果获取失败，使用默认值
    strcpy(device_imei, "111111111111111");
    ESP_LOGI(TAG, "使用默认IMEI: %s", device_imei);
  }
}

/* BRTC服务器URL和CA证书 */
const char brtcserver[125] = "ws://rtc.exp.bcelive.com:8186/janus";

const char *ca_info = "-----BEGIN CERTIFICATE-----\r\n\
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P\
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\
gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\
BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\
TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\
DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\
hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\
06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\
PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\
YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\
CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\
-----END CERTIFICATE-----";

/* BRTC客户端全局变量 */
static void *s_mqtt_rtc_client = NULL;
static bool s_mqtt_sdk_initialized = false;
static char s_mqtt_rtc_serial[128] = {0};            // 保存RTC呼叫的MQTT流水号
static bool s_mqtt_rtc_connected = false;            // RTC连接状态
static TimerHandle_t s_mqtt_rtc_switch_timer = NULL; // RTC切换定时器
static TimerHandle_t s_heartbeat_timer = NULL;       // 心跳包定时器

/* RTC参数缓存（用于保存MQTT指令中的参数，确保内存生命周期） */
static char s_mqtt_rtc_appid[128] = {0};
static char s_mqtt_rtc_token[512] = {0};
static char s_mqtt_rtc_userid[128] = {0};
static char s_mqtt_rtc_roomname[128] = {0};

/* 声明外部BRTC函数 */
extern int brtc_sdk_init(void);
extern int brtc_sdk_deinit(void);
extern void brtc_sdk_enable_log(int enable);
extern void *brtc_create_client(void);
extern void brtc_destroy_client(void *rtc_client);
extern int brtc_login_room(void *rtc_client, const char *room_name,
                           const char *user_id, const char *display_name,
                           const char *token);
extern int brtc_logout_room(void *rtc_client);
extern void brtc_set_appid(void *rtc_client, const char *app_id);
extern void brtc_register_message_listener(void *rtc_client,
                                           IRtcMessageListener msgListener);
extern void brtc_set_audiocodec(void *rtc_client, const char *codec);
extern void brtc_set_usingvideo(void *rtc_client, int enable);
extern void brtc_set_receivingvideo(void *rtc_client, int enable);
extern void brtc_register_audio_frame_observer(
    void *rtc_client, void (*callback)(int64_t, const char *, int, int, int));
extern void brtc_set_cer(void *rtc_client, const char *cer);
extern void brtc_set_server_url(void *rtc_client, const char *server_url);
extern void brtc_set_auto_publish(void *rtc_client, int enable);
extern void brtc_set_auto_subscribe(void *rtc_client, int enable);
extern void brtc_send_audio(void *rtc_client, const void *data, int len);

/* 声明外部brtc_app函数 */
extern esp_err_t brtc_switch_to_ai_mode(void);

/* 声明外部音频函数 */
extern int audio_recorder_read_data(uint8_t *data, int len);

/* 函数前向声明 */
char *generate_timestamp(void);
char *generate_serial(void);

/* 辅助函数：以十六进制方式打印数据 */
static void print_hex_data(const char *tag, const char *prefix,
                           const void *data, size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  char hex_str[128];
  size_t pos = 0;

  for (size_t i = 0; i < len && i < 32; i++) {
    if (pos < sizeof(hex_str) - 3) {
      pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X ", bytes[i]);
    }
  }
  ESP_LOGE(tag, "%s (长度: %zu) %s", prefix, len, hex_str);
}

/* RTC切换定时器回调函数 */
static void rtc_switch_timer_callback(TimerHandle_t xTimer) {
  ESP_LOGI(TAG, "RTC通话30秒后，切换回AI对话模式");
  esp_err_t ret = brtc_switch_to_ai_mode();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "切换回AI对话模式失败");
  }

  // 删除定时器
  if (s_mqtt_rtc_switch_timer != NULL) {
    xTimerDelete(s_mqtt_rtc_switch_timer, 0);
    s_mqtt_rtc_switch_timer = NULL;
  }
}

/* 发送心跳包函数 */

/* 发送完整通讯录同步数据到服务器 */

/* 心跳包定时器回调函数 */
static void heartbeat_timer_callback(TimerHandle_t xTimer) {
  ESP_LOGI(TAG, "心跳包定时器触发，当前MQTT状态=%d", mqtt5_client_get_state());

  if (mqtt5_client_get_state() != MQTT_STATE_CONNECTED) {
    ESP_LOGW(TAG, "MQTT未连接，跳过心跳包发送");
    return;
  }

  // 发送心跳包，status为heartbeat
  send_heartbeat_packet("heartbeat");
}

/* BRTC消息监听器回调函数 */
static void mqtt_on_rtc_message_callback(RtcMessage *msg) {
  if (!msg) {
    ESP_LOGE(TAG, "收到空的RTC消息");
    return;
  }

  ESP_LOGI(TAG, "收到RTC消息，类型: %d", msg->msgType);

  switch (msg->msgType) {
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_OK:
    ESP_LOGI(TAG, "RTC房间登录成功2");
    s_mqtt_rtc_connected = true;
    // 设置RTC模式，audio_data_read_task会自动处理音频采集和发送
    brtc_set_rtc_mode(true, s_mqtt_rtc_client);
    // 注意：不在这里设置流状态，等待RTC_MESSAGE_STATE_STREAM_UP事件
    ESP_LOGI(TAG, "等待RTC流建立...");
    if (strlen(s_mqtt_rtc_serial) > 0) {
      send_device_command_response(s_mqtt_rtc_serial, "08", "200",
                                   "RTC房间登录成功", "9000");
      memset(s_mqtt_rtc_serial, 0, sizeof(s_mqtt_rtc_serial));
    }
    break;
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_TIMEOUT:
    ESP_LOGE(TAG, "RTC房间登录超时");
    s_mqtt_rtc_connected = false;
    if (strlen(s_mqtt_rtc_serial) > 0) {
      send_device_command_response(s_mqtt_rtc_serial, "08", "500",
                                   "RTC房间登录超时", "9000");
      memset(s_mqtt_rtc_serial, 0, sizeof(s_mqtt_rtc_serial));
    }
    break;
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_ERROR:
    ESP_LOGE(TAG, "RTC房间登录错误，错误代码: %" PRId64, msg->data.errorCode);
    s_mqtt_rtc_connected = false;
    if (msg->extra_info) {
      print_hex_data(TAG, "错误详情: ", msg->extra_info,
                     strlen(msg->extra_info));
    }
    if (strlen(s_mqtt_rtc_serial) > 0) {
      send_device_command_response(s_mqtt_rtc_serial, "08", "500",
                                   "RTC房间登录错误", "9000");
      memset(s_mqtt_rtc_serial, 0, sizeof(s_mqtt_rtc_serial));
    }
    break;
  case RTC_MESSAGE_ROOM_EVENT_CONNECTION_LOST:
    ESP_LOGW(TAG, "RTC连接丢失");
    s_mqtt_rtc_connected = false;
    // 切换回AI对话模式
    ESP_LOGI(TAG, "RTC连接丢失，切换回AI对话模式");
    esp_err_t ret = brtc_switch_to_ai_mode();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "切换回AI对话模式失败");
    }
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_COMING:
    ESP_LOGI(TAG, "RTC远程用户加入");
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_LEAVING:
    ESP_LOGI(TAG, "RTC远程用户离开");
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_RENDERING:
    ESP_LOGI(TAG, "RTC远程用户开始渲染");
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_GONE:
    ESP_LOGW(TAG, "RTC远程用户消失");
    break;
  case RTC_MESSAGE_ROOM_EVENT_SERVER_ERROR:
    ESP_LOGE(TAG, "RTC服务器错误");
    break;
  case RTC_MESSAGE_STATE_STREAM_UP:
    ESP_LOGI(TAG, "RTC流已建立，开始音频传输");
    brtc_set_rtc_stream_up(true);

    // 启动30秒定时器，之后切换回AI对话模式
    if (s_mqtt_rtc_switch_timer != NULL) {
      xTimerDelete(s_mqtt_rtc_switch_timer, 0);
      s_mqtt_rtc_switch_timer = NULL;
    }
    s_mqtt_rtc_switch_timer =
        xTimerCreate("rtc_switch", pdMS_TO_TICKS(30000), pdFALSE, NULL,
                     rtc_switch_timer_callback);
    if (s_mqtt_rtc_switch_timer != NULL) {
      xTimerStart(s_mqtt_rtc_switch_timer, 0);
      ESP_LOGI(TAG, "已启动30秒定时器，之后将切换回AI对话模式");
    } else {
      ESP_LOGE(TAG, "创建RTC切换定时器失败");
    }
    break;
  default:
    ESP_LOGW(TAG, "RTC未知消息类型: %d", msg->msgType);
    break;
  }
}

/* BRTC音频帧回调函数 */
static void mqtt_on_audio_frame(int64_t feedid, const char *audio, int len,
                                int samlplerate, int channels) {
  if (len <= 0) {
    return;
  }

  // 参考brtc_esp32_demo.c的做法，直接播放所有长度的音频数据
  // 不做严格的长度检查，因为实际收到的音频帧长度可能变化
  // 8kHz PCMU
  // 20ms的标准音频帧长度应该是160字节，但实际收到的可能在126-158字节之间

  ESP_LOGD(TAG, "收到音频帧: feedid=%lld, len=%d, samlplerate=%d, channels=%d",
           feedid, len, samlplerate, channels);

  // 分析数据格式：检查前12字节是否为RTP头
  if (len >= 12) {
    const uint8_t *data = (const uint8_t *)audio;
    // RTP头第一个字节：V(2) P(1) X(1) CC(4)
    uint8_t first_byte = data[0];
    uint8_t version = (first_byte >> 6) & 0x03;
    uint8_t csrc_count = first_byte & 0x0F;

    // RTP头第二个字节：M(1) PT(7)
    uint8_t second_byte = data[1];
    uint8_t payload_type = second_byte & 0x7F;

    // RTP头第3-4字节：序列号
    uint16_t sequence = (data[2] << 8) | data[3];

    ESP_LOGD(
        TAG,
        "数据格式分析: version=%d, csrc_count=%d, payload_type=%d, sequence=%d",
        version, csrc_count, payload_type, sequence);

    // 如果检测到RTP头（version=2），则跳过RTP头
    if (version == 2) {
      int rtp_header_len = 12 + csrc_count * 4;
      if (len > rtp_header_len) {
        int audio_data_len = len - rtp_header_len;
        ESP_LOGD(TAG, "检测到RTP头，跳过%d字节，播放%d字节音频数据",
                 rtp_header_len, audio_data_len);
        audio_feeder_feed_data((uint8_t *)audio + rtp_header_len,
                               audio_data_len);
        return;
      }
    }
  }

  // 直接播放收到的音频数据
  // 注意：这里假设收到的数据已经是PCM格式，或者audio_feeder_feed_data会自动处理解码
  audio_feeder_feed_data((uint8_t *)audio, len);
}

/* 函数声明 */
esp_err_t write_key_to_nvs(const char *key, const char *value);
esp_err_t read_key_from_nvs(const char *key, char *value_buffer,
                            size_t buffer_size);

/* BRTC音频帧回调函数声明 */
static void mqtt_on_audio_frame(int64_t feedid, const char *audio, int len,
                                int samlplerate, int channels);

/* 生成时间戳函数 */
char *generate_timestamp(void) {
  time_t now = 0;
  struct tm timeinfo = {0};
  char *timestamp = malloc(32);

  if (!timestamp) {
    ESP_LOGE(TAG, "分配时间戳内存失败");
    return NULL;
  }

  // 获取当前时间
  time(&now);
  localtime_r(&now, &timeinfo);

  // 格式化为yyyyMMddHHmmss
  strftime(timestamp, 32, "%Y%m%d%H%M%S", &timeinfo);

  return timestamp;
}

/* 生成流水号函数 */
char *generate_serial(void) {
  char *serial = malloc(32);
  if (!serial) {
    ESP_LOGE(TAG, "分配流水号内存失败");
    return NULL;
  }

  // 使用时间戳和随机数生成唯一流水号
  uint32_t random = esp_random();
  snprintf(serial, 32, "xsz%llu%lu", esp_timer_get_time() / 1000, random);

  return serial;
}

/* 生成自定义userid函数 */
char *generate_custom_userid(void) {
  char *userid = malloc(64);
  if (!userid) {
    ESP_LOGE(TAG, "分配userid内存失败");
    return NULL;
  }

  // 使用设备IMEI、时间戳和随机数生成唯一userid
  uint32_t random = esp_random();
  uint64_t time_us = esp_timer_get_time() / 1000;
  snprintf(userid, 64, "esp_%s_%" PRIu64 "_%" PRIu32, device_imei, time_us,
           random);

  return userid;
}

/**
 * @brief MQTT事件回调函数
 *
 * @param event MQTT事件
 * @param user_data 用户数据
 */
static void mqtt_event_callback(mqtt_event_t *event, void *user_data) {
  switch (event->state) {
  case MQTT_STATE_CONNECTED:
    ESP_LOGI(TAG, "MQTT已连接");
    break;

  case MQTT_STATE_DISCONNECTED:
    ESP_LOGE(TAG, "MQTT已断开连接，错误代码: %d", event->error_code);
    break;

  case MQTT_STATE_DATA_RECEIVED:
    ESP_LOGI(TAG, "MQTT接收到数据");
    if (event->message.topic && event->message.data) {
      printf("主题=%s\r\n", event->message.topic);
      printf("数据=%s\r\n", event->message.data);

      // 处理设备绑定主题消息
      char sn_buffer[128] = {0};
      esp_err_t sn_ret = read_sn_from_nvs(sn_buffer, sizeof(sn_buffer));

      if (sn_ret == ESP_OK && strlen(sn_buffer) > 0) {
        char expected_topic[256];
        snprintf(expected_topic, sizeof(expected_topic), "key_bind/%s",
                 sn_buffer);

        if (strcmp(event->message.topic, expected_topic) == 0) {
          ESP_LOGI(TAG, "收到设备绑定消息，开始解析...");
          parse_key_bind_message(event->message.data);
        }
      }
    }

    // 处理百度RTC呼叫主题消息
    // if (event->message.topic && event->message.data) {
    //   char rtc_topic[256];
    //   snprintf(rtc_topic, sizeof(rtc_topic),
    //   "/device/specific/app/call/rtc/%s",
    //            device_imei);

    //   if (strcmp(event->message.topic, rtc_topic) == 0) {
    //     ESP_LOGI(TAG, "收到百度RTC呼叫消息，开始解析...");
    //     parsebrtc_call_message(event->message.data);
    //   }
    // }

    // 处理通讯录同步主题消息
    if (event->message.topic && event->message.data) {
      char contact_topic[256];
      snprintf(contact_topic, sizeof(contact_topic),
               "/device/specific/app/contact/sync/%s", device_imei);

      if (strcmp(event->message.topic, contact_topic) == 0) {
        ESP_LOGI(TAG, "收到通讯录同步消息，开始解析...");
        parse_contact_sync_message(event->message.data);
      }
    }
    break;

  case MQTT_STATE_ERROR:
    ESP_LOGE(TAG, "MQTT发生错误，错误代码: %d", event->error_code);
    break;

  case MQTT_STATE_IDLE:
    // 空闲状态，无需处理
    break;

  default:
    ESP_LOGI(TAG, "收到MQTT事件，状态=%d", event->state);
    break;
  }
}

/**
 * @brief 设备指令完成反馈接口实现
 *
 * @param ori_serial 原始请求流水号
 * @param busi 业务类型
 * @param code 状态码
 * @param msg 错误信息
 * @param cmd_res 指令回执
 */

/**
 * @brief 语音来电接口实现
 *
 * @param phone 来电号码
 * @param call_time 来电时间
 */

/**
 * @brief 短信来信接口实现
 *
 * @param phone 发送号码
 * @param content 短信内容
 */

extern void control_device_by_service_status(bool action);
bool is_service_status = true;
/**
 * @brief 解析服务状态接口消息
 *
 * @param json_str JSON字符串
 */

#include "sip_service.h"
esp_rtc_handle_t s_esp_sip;
/**
 * @brief 解析SIP信息接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 解析服务端指令完成反馈接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 测试OTA升级功能
 *
 * 发送一个模拟的OTA升级指令到本地MQTT客户端
 *
static void test_ota_upgrade(void)
{
    // 创建模拟的OTA升级指令JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "创建测试OTA升级JSON失败");
        return;
    }

    // 创建body对象
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        ESP_LOGE(TAG, "创建测试OTA升级body JSON对象失败");
        cJSON_Delete(root);
        return;
    }

    // 添加字段
    cJSON_AddStringToObject(root, "timeStamp", "20240807221527");
    cJSON_AddStringToObject(root, "serial", "AAA20240802163734");
    cJSON_AddStringToObject(root, "imei", device_imei);

    cJSON_AddStringToObject(body, "version", "123460");
    cJSON_AddStringToObject(body, "url",
"http://192.168.2.222:8080/hello_world.bin");

    cJSON_AddItemToObject(root, "body", body);

    // 转换为JSON字符串
    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "转换测试OTA升级JSON字符串失败");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "测试OTA升级功能，模拟消息: %s", json_str);

    // 直接调用解析函数测试
    parse_ota_upgrade_command(json_str);

    // 清理资源
    free(json_str);
    cJSON_Delete(root);
} */

/**
 * @brief 解析语音拦截规则下发接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 解析语音呼叫指令接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 解析短信发送指令接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 解析AT指令发送接口消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 解析设备绑定消息
 *
 * @param json_str JSON字符串
 */

/**
 * @brief 保存key到NVS
 *
 * @param key 键名
 * @param value 值
 * @return esp_err_t 保存结果
 */
esp_err_t write_key_to_nvs(const char *key, const char *value) {
  if (key == NULL || value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // 初始化NVS
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS分区已满或版本不匹配，擦除并重试
    ESP_LOGW(TAG, "NVS分区需要擦除，错误代码: %s", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS初始化失败: %s", esp_err_to_name(err));
    return err;
  }

  // 打开NVS命名空间
  nvs_handle_t nvs_handle;
  err = nvs_open("key_bind", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS命名空间打开失败: %s", esp_err_to_name(err));
    return err;
  }

  // 写入key值
  err = nvs_set_str(nvs_handle, key, value);
  if (err == ESP_OK) {
    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Key '%s' 写入NVS成功: %s", key, value);
    } else {
      ESP_LOGE(TAG, "NVS提交失败: %s", esp_err_to_name(err));
    }
  } else {
    ESP_LOGE(TAG, "Key '%s' 写入NVS失败: %s", key, esp_err_to_name(err));
  }

  // 关闭NVS
  nvs_close(nvs_handle);

  return err;
}

/**
 * @brief 从NVS读取key值
 *
 * @param key 键名
 * @param value_buffer 值缓冲区
 * @param buffer_size 缓冲区大小
 * @return esp_err_t 操作结果
 */
esp_err_t read_key_from_nvs(const char *key, char *value_buffer,
                            size_t buffer_size) {
  if (!key || !value_buffer || buffer_size == 0) {
    ESP_LOGE(TAG, "参数错误");
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs_handle;
  esp_err_t ret = nvs_open("key_bind", NVS_READONLY, &nvs_handle);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS打开失败，错误代码: %d", ret);
    return ret;
  }

  size_t required_size = buffer_size;
  ret = nvs_get_str(nvs_handle, key, value_buffer, &required_size);

  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "键 %s 不存在", key);
    nvs_close(nvs_handle);
    return ret;
  } else if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS读取失败，错误代码: %d", ret);
    nvs_close(nvs_handle);
    return ret;
  }

  ESP_LOGI(TAG, "从NVS读取键 %s: %s", key, value_buffer);
  nvs_close(nvs_handle);
  return ESP_OK;
}

/**
 * @brief 解析OTA升级指令消息
 *
 * @param json_str JSON字符串
 */


/**
 * @brief MQTT事件处理任务
 *
 * @param pvParameters 任务参数
 */
static void mqtt_event_task(void *pvParameters) {
  QueueHandle_t event_queue = (QueueHandle_t)pvParameters;
  mqtt_event_t *event;

  ESP_LOGI(TAG, "MQTT事件处理任务已启动");

  while (1) {
    /* 从事件队列中获取事件 */
    if (xQueueReceive(event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (event) {
        network_status_t net_status = get_network_status();
        ESP_LOGI(TAG, "[%s USE] 收到MQTT事件，状态=%d", net_status.is_wifi_active ? "WIFI" : (net_status.is_4g_active ? "4g" : "UNKNOWN"), event->state);

        /* 根据事件状态进行相应的处理 */
        switch (event->state) {
        case MQTT_STATE_CONNECTED:
          ESP_LOGI(TAG, "处理连接成功事件");
          /* 连接成功后等待一段时间再订阅主题，避免立即订阅导致断开 */
          vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒确保连接稳定

          /* 连接成功后订阅所有需要的主题，但只订阅一次 */
          if (!mqtt_subscribed) {
            mqtt_subscribed = true;
            ESP_LOGI(TAG, "首次连接，开始订阅主题");
            char topic[128];

            // 订阅 SIP 网址、账号、密码
            int ret0 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/sip/info/%s", device_imei);
            if (ret0 > 0 && ret0 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造SIP用户名主题");
            }

            // 订阅服务端指令完成反馈主题
            int ret1 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/send/response/%s", device_imei);
            if (ret1 > 0 && ret1 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造服务端指令完成反馈主题");
            }

            // 订阅语音拦截规则下发主题
            int ret2 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/gsm/rules/%s", device_imei);
            if (ret2 > 0 && ret2 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造语音拦截规则下发主题");
            }

            // 订阅语音呼叫指令主题
            int ret3 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/gsm/call/%s", device_imei);
            if (ret3 > 0 && ret3 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造语音呼叫指令主题");
            }

            // 订阅短信发送指令主题
            int ret4 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/sms/call/%s", device_imei);
            if (ret4 > 0 && ret4 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造短信发送指令主题");
            }

            // 订阅AT指令主题
            int ret5 = snprintf(topic, sizeof(topic),
                                "/device/specific/app/at/call/%s", device_imei);
            if (ret5 > 0 && ret5 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造AT指令主题");
            }

            // 订阅OTA升级主题
            int ret6 = snprintf(topic, sizeof(topic),
                                "/device/specific/app/version/upgrade/%s",
                                device_imei);
            if (ret6 > 0 && ret6 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造OTA升级主题");
            }

            // 订阅设备绑定主题
            snprintf(topic, sizeof(topic), "key_bind/%s", device_imei);
            mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            ESP_LOGI(TAG, "订阅设备绑定主题: %s", topic);
            // char sn_buffer[128] = {0};
            // esp_err_t sn_ret = read_sn_from_nvs(sn_buffer,
            // sizeof(sn_buffer)); if (sn_ret == ESP_OK && strlen(sn_buffer) >
            // 0) {
            //   // 确保不会溢出，预留足够的空间给前缀和终止符
            //   size_t sn_len = strlen(sn_buffer);
            //   size_t prefix_len = strlen("key_bind/");
            //   size_t total_len =
            //       sn_len + prefix_len + 1; // +1 for null terminator

            //   if (total_len <= sizeof(topic)) {
            //     int ret =
            //         snprintf(topic, sizeof(topic), "key_bind/%s", sn_buffer);
            //     if (ret > 0 && ret < sizeof(topic)) {
            //       mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            //       ESP_LOGI(TAG, "订阅设备绑定主题: %s", topic);
            //     } else {
            //       ESP_LOGE(TAG, "主题字符串格式化失败，返回值: %d", ret);
            //     }
            //   } else {
            //     ESP_LOGW(TAG,
            //              "SN码过长(%d字节)，无法构造主题，最大允许长度:
            //              %d字节", sn_len, (int)(sizeof(topic) - prefix_len -
            //              1));
            //   }
            // } else {
            //   ESP_LOGW(TAG, "无法读取SN码，跳过设备绑定主题订阅");
            // }

            // 订阅百度RTC呼叫主题
            // int ret7 =
            //     snprintf(topic, sizeof(topic),
            //              "/device/specific/app/call/rtc/%s", device_imei);
            // if (ret7 > 0 && ret7 < sizeof(topic)) {
            //   mqtt5_client_subscribe(topic, 0); // 降低QoS到0
            //   ESP_LOGI(TAG, "订阅百度RTC呼叫主题: %s", topic);
            // } else {
            //   ESP_LOGE(TAG, "主题字符串过长，无法构造百度RTC呼叫主题");
            // }

            // 订阅通讯录同步主题
            int ret8 =
                snprintf(topic, sizeof(topic),
                         "/device/specific/app/contact/sync/%s", device_imei);
            if (ret8 > 0 && ret8 < sizeof(topic)) {
              mqtt5_client_subscribe(topic, 0); // 降低QoS到0
              ESP_LOGI(TAG, "订阅通讯录同步主题: %s", topic);
            } else {
              ESP_LOGE(TAG, "主题字符串过长，无法构造通讯录同步主题");
            }
          } else {
            ESP_LOGI(TAG, "已经订阅过主题，跳过订阅操作");
          }

          // 确保订阅指令已经发送后，再发送开机状态报文，避免服务器的快速回复丢包
          ESP_LOGI(TAG, "订阅完成或已跳过，现在发送开机状态并启动心跳");
          send_heartbeat_packet("on");

          if (s_heartbeat_timer == NULL) {
            s_heartbeat_timer = xTimerCreate("heartbeat_timer", pdMS_TO_TICKS(60000), pdTRUE, (void *)0, heartbeat_timer_callback);
            if (s_heartbeat_timer == NULL) {
              ESP_LOGE(TAG, "创建心跳包定时器失败");
            } else {
              if (xTimerStart(s_heartbeat_timer, 0) != pdPASS) {
                ESP_LOGE(TAG, "启动心跳包定时器失败");
                xTimerDelete(s_heartbeat_timer, 0);
                s_heartbeat_timer = NULL;
              } else {
                ESP_LOGI(TAG, "心跳包定时器启动成功，每60秒发送一次");
              }
            }
          }
          break;

        case MQTT_STATE_DATA_RECEIVED:
          ESP_LOGI(TAG, "处理数据接收事件");
          if (event->message.topic && event->message.data) {
            ESP_LOGI(TAG, "收到消息：主题=%s, 数据=%s", event->message.topic, event->message.data);

            /* 根据主题进行相应的处理 */
            if (strstr(event->message.topic, "/device/specific/app/sip/info/")) {
              parse_sip_info(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/service/status/")) {
              parse_service_status(event->message.data);
            } else if (strstr(event->message.topic, "key_bind/")) {
              parse_key_bind_message(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/send/response/")) {
              parse_server_command_response(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/gsm/rules/")) {
              parse_voice_intercept_rules(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/gsm/call/")) {
              parse_voice_call_command(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/sms/call/")) {
              parse_sms_send_command(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/at/call/")) {
              parse_at_command(event->message.data);
            } else if (strstr(event->message.topic, "/device/specific/app/version/upgrade/")) {
              parse_ota_upgrade_command(event->message.data);
            } else {
              ESP_LOGW(TAG, "未知主题: %s", event->message.topic);
            }

            esp_err_t ret = nvs_open("mqtt_data", NVS_READWRITE, &nvs_mqtt_handle);
            if (ret != ESP_OK) {
              ESP_LOGE(TAG, "NVS打开失败，错误代码: %d", ret);
            } else {
              if (strstr(event->message.topic, "/device/specific/app/sip/info/")) {
                ret = nvs_set_str(nvs_mqtt_handle, "SipInfo", event->message.data);
                if (ret == ESP_OK) ESP_LOGI(TAG, "SIP信息已存储到NVS");
                else ESP_LOGE(TAG, "存储SIP信息失败，错误代码: %d", ret);
              } else if (strstr(event->message.topic, "/device/specific/app/gsm/rules/")) {
                ret = nvs_set_str(nvs_mqtt_handle, "Rules", event->message.data);
                if (ret == ESP_OK) ESP_LOGI(TAG, "GSM规则已存储到NVS");
                else ESP_LOGE(TAG, "存储GSM规则失败，错误代码: %d", ret);
              } else if (strstr(event->message.topic, "/device/specific/app/service/status/")) {
                ret = nvs_set_str(nvs_mqtt_handle, "SrvStat", event->message.data);
                if (ret == ESP_OK) ESP_LOGI(TAG, "服务状态已存储到NVS");
                else ESP_LOGE(TAG, "存储服务状态失败，错误代码: %d", ret);
              }
              ret = nvs_commit(nvs_mqtt_handle);
              if (ret != ESP_OK) {
                ESP_LOGE(TAG, "NVS提交失败，错误代码: %d", ret);
              }
              nvs_close(nvs_mqtt_handle);
            }
          } else {
            ESP_LOGE(TAG, "消息主题或数据为空");
          }
          break;

        case MQTT_STATE_DISCONNECTED:
          ESP_LOGI(TAG, "处理断开连接事件");
          mqtt_subscribed = false; // 断开连接时重置订阅标志
          /* 可以在这里实现重连逻辑 */
          break;

        case MQTT_STATE_ERROR:
          ESP_LOGE(TAG, "处理错误事件，错误代码：%d", event->error_code);
          /* 可以在这里实现错误处理逻辑 */
          break;

        default:
          break;
        }

        /* 释放事件资源 */
        if (event->message.topic) {
          free(event->message.topic);
        }
        if (event->message.data) {
          free(event->message.data);
        }
        free(event);
      }
    }
  }
}

void mqtt_flash_load(void) {
  nvs_open("mqtt_data", NVS_READONLY, &nvs_mqtt_handle);
  esp_err_t ret = ESP_FAIL;
  char sip_info[256] = {0};
  size_t sip_info_len = sizeof(sip_info);
  ret = nvs_get_str(nvs_mqtt_handle, "SipInfo", sip_info, &sip_info_len);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "从NVS读取SIP信息: %s", sip_info);
    parse_sip_info(sip_info);
  } else {
    ESP_LOGE(TAG, "从NVS读取SIP信息失败，错误代码: %d", ret);
  }
  char rules[256] = {0};
  size_t rules_len = sizeof(rules);
  ret = nvs_get_str(nvs_mqtt_handle, "Rules", rules, &rules_len);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "从NVS读取GSM规则: %s", rules);
    parse_voice_intercept_rules(rules);
  } else {
    ESP_LOGE(TAG, "从NVS读取GSM规则失败，错误代码: %d", ret);
  }
  char srv_stat[256] = {0};
  size_t srv_stat_len = sizeof(srv_stat);
  ret = nvs_get_str(nvs_mqtt_handle, "SrvStat", srv_stat, &srv_stat_len);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "从NVS读取服务状态: %s", srv_stat);
    parse_service_status(srv_stat);
  } else {
    ESP_LOGE(TAG, "从NVS读取服务状态失败，错误代码: %d", ret);
  }
  nvs_close(nvs_mqtt_handle);
}

/**
 * @brief 模拟语音来电任务
 *
 * @param pvParameters 任务参数
 */
static void simulate_voice_call_task(void *pvParameters) {
  ESP_LOGI(TAG, "模拟语音来电任务已启动");

  while (1) {
    // 等待MQTT连接成功
    if (mqtt5_client_get_state() == MQTT_STATE_CONNECTED) {
      // 每30秒模拟一次语音来电
      vTaskDelay(pdMS_TO_TICKS(30000));

      // 生成随机电话号码
      char phone[16];
      snprintf(phone, sizeof(phone), "189%08lu", esp_random() % 100000000);

      // 生成当前时间戳
      char *call_time = generate_timestamp();
      if (!call_time) {
        ESP_LOGE(TAG, "生成来电时间失败");
        continue;
      }

      // 添加随机延迟，避免多个任务同时发布消息
      vTaskDelay(pdMS_TO_TICKS(esp_random() % 2000)); // 0-2秒随机延迟

      // 发送语音来电通知
      send_voice_call_notification(phone, call_time);

      free(call_time);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

/**
 * @brief 模拟短信来信任务
 *
 * @param pvParameters 任务参数
 */
static void simulate_sms_receive_task(void *pvParameters) {
  ESP_LOGI(TAG, "模拟短信来信任务已启动");

  while (1) {
    // 等待MQTT连接成功
    if (mqtt5_client_get_state() == MQTT_STATE_CONNECTED) {
      // 每45秒模拟一次短信来信
      vTaskDelay(pdMS_TO_TICKS(45000));

      // 生成随机电话号码
      char phone[16];
      snprintf(phone, sizeof(phone), "138%08lu", esp_random() % 100000000);

      // 模拟短信内容
      const char *contents[] = {"验证码：123456", "您的快递已签收",
                                "会议将于10分钟后开始", "您的账户余额不足",
                                "感谢您的支持"};
      int content_index =
          esp_random() % (sizeof(contents) / sizeof(contents[0]));

      // 添加随机延迟，避免多个任务同时发布消息
      vTaskDelay(pdMS_TO_TICKS(esp_random() % 2000)); // 0-2秒随机延迟

      // 发送短信来信通知
      send_sms_notification(phone, contents[content_index]);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

/**
 * @brief MQTT连接监控任务
 *
 * @param pvParameters 任务参数
 */
static void mqtt_connection_monitor_task(void *pvParameters) {
  ESP_LOGI(TAG, "MQTT连接监控任务已启动");

  mqtt_state_t last_state = MQTT_STATE_DISCONNECTED;
  int disconnect_count = 0;
  const int max_disconnect_count = 5;
  int reconnect_delay = 2000;            // 初始重连延迟2秒
  const int max_reconnect_delay = 30000; // 最大重连延迟30秒

  while (1) {
    mqtt_state_t current_state = mqtt5_client_get_state();

    // 检测状态变化
    if (current_state != last_state) {
      ESP_LOGI(TAG, "MQTT状态变化: %d -> %d", last_state, current_state);
      last_state = current_state;

      if (current_state == MQTT_STATE_CONNECTED) {
        disconnect_count = 0;   // 连接成功，重置断开计数
        reconnect_delay = 2000; // 重置重连延迟
        ESP_LOGI(TAG, "MQTT连接稳定，断开计数已重置");
      }
    }

    // 如果连接断开，立即尝试重连
    if (current_state == MQTT_STATE_DISCONNECTED) {
      disconnect_count++;
      ESP_LOGW(TAG, "MQTT连接断开，计数: %d/%d", disconnect_count,
               max_disconnect_count);

      // 使用指数退避算法进行重连
      ESP_LOGI(TAG, "等待 %d 毫秒后尝试重连...", reconnect_delay);
      vTaskDelay(pdMS_TO_TICKS(reconnect_delay));

      // 尝试重新启动客户端
      ESP_LOGI(TAG, "尝试重新连接MQTT服务器...");
      esp_err_t ret = mqtt5_client_start();

      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT客户端重新启动成功");
        // 等待连接结果
        vTaskDelay(pdMS_TO_TICKS(5000));

        // 检查是否连接成功
        if (mqtt5_client_get_state() == MQTT_STATE_CONNECTED) {
          ESP_LOGI(TAG, "MQTT重连成功");
          disconnect_count = 0;   // 重置计数
          reconnect_delay = 2000; // 重置重连延迟
        } else {
          ESP_LOGW(TAG, "MQTT重连未成功，增加重连延迟");
          // 增加重连延迟，使用指数退避
          reconnect_delay = reconnect_delay * 2;
          if (reconnect_delay > max_reconnect_delay) {
            reconnect_delay = max_reconnect_delay;
          }
        }
      } else {
        ESP_LOGE(TAG, "重新启动MQTT客户端失败: %s", esp_err_to_name(ret));
        // 增加重连延迟
        reconnect_delay = reconnect_delay * 2;
        if (reconnect_delay > max_reconnect_delay) {
          reconnect_delay = max_reconnect_delay;
        }
      }

      // 如果断开次数过多，尝试重新初始化MQTT客户端
      if (disconnect_count >= max_disconnect_count) {
        ESP_LOGW(TAG, "MQTT频繁断开，尝试完全重新初始化客户端");

        // 停止当前客户端
        mqtt5_client_stop();
        vTaskDelay(pdMS_TO_TICKS(3000));

        // 重新启动客户端
        ret = mqtt5_client_start();
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "重新初始化MQTT客户端失败");
        } else {
          ESP_LOGI(TAG, "MQTT客户端重新初始化成功");
          disconnect_count = 0;   // 重置计数
          reconnect_delay = 2000; // 重置重连延迟
        }
      }
    }

    // 每5秒检查一次连接状态
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void mqtt_app_init(void) {
  ESP_LOGI(TAG, "[APP] 启动专项市场接口规范MQTT接入应用程序..");
  /* 初始化NVS */
  // ESP_ERROR_CHECK(nvs_flash_init());

  /* 初始化WiFi */
  // wifi_init_sta();

  /* 获取设备IMEI号 */
  get_device_imei();

  /* 配置MQTT客户端 */
  char client_id[64];
  snprintf(client_id, sizeof(client_id), "esp32_%s", device_imei);

  mqtt_config_t mqtt_config = {
      .broker_uri = "mqtt://mqtt-emqx.ofish.cn:1883",
      .username = "ruikh",
      .password = "ufg3AeW^4c",
      .client_id = client_id, // 使用IMEI作为客户端ID，确保唯一性
      .will_topic = "/topic/will",
      .will_message = "客户端意外断开连接",
      .will_qos = 1,
      .will_retain = false,
      .keepalive = 20, // 调整心跳间隔为20秒，防止超时断开
      .disable_auto_reconnect = false,
      .network_timeout = 30000, // 增加网络超时时间到30秒
      .clean_session = true};

  /* 初始化MQTT客户端 */
  esp_err_t ret = mqtt5_client_init(&mqtt_config, mqtt_event_callback, NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "初始化MQTT客户端失败");
    return;
  }

  /* 设置用户属性 */
  // mqtt5_client_set_user_property("device_type", "7258");
  // mqtt5_client_set_user_property("firmware_version", "1.0.0");
  mqtt5_client_set_user_property("device_imei", device_imei);

  /* 启动MQTT客户端 */
  ret = mqtt5_client_start();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "启动MQTT客户端失败");
    return;
  }

  /* 获取事件队列 */
  QueueHandle_t event_queue = mqtt5_client_get_event_queue();

  /* 检查队列是否有效 */
  if (event_queue == NULL) {
    ESP_LOGE(TAG, "事件队列为NULL，无法创建任务");
    return;
  }

  ESP_LOGI(TAG, "事件队列检查通过");

  /* 创建MQTT事件处理任务 */
  BaseType_t ret1 =
      xTaskCreatePinnedToCoreWithCaps(mqtt_event_task, "mqtt_event_task", 6 * 1024,
                              event_queue, 5, NULL, 1, MALLOC_CAP_SPIRAM); 
  if (ret1 != pdPASS) {
    ESP_LOGE(TAG, "创建MQTT事件处理任务失败");
  }



  /* 创建模拟语音来电任务 */
  // xTaskCreate(simulate_voice_call_task, "sim_voice_call_task", 3072, NULL,
  // 4, NULL);  // 从4096减小到3072

  /* 创建模拟短信来信任务 */
  // xTaskCreate(simulate_sms_receive_task, "sim_sms_receive_task", 3072,
  // NULL, 4, NULL);  // 从4096减小到3072

  /* 创建MQTT连接监控任务 */
  // xTaskCreatePinnedToCore(mqtt_connection_monitor_task, "mqtt_conn_monitor",
  //                         3 * 1024, NULL, 1, NULL, 1); //
  //                         增加到3124字节，使用PSRAM

  ESP_LOGI(TAG, "专项市场接口规范MQTT接入应用程序已启动");

  // 等待一段时间，确保MQTT连接稳定
  // vTaskDelay(pdMS_TO_TICKS(10000));

  // 测试OTA升级功能
  // test_ota_upgrade();

  // 测试ota
  // ota_start("http://192.168.2.222:8080/hello_world.bin");
  //  主循环，等待退出信号
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 处理来电管理器事件
    call_manager_process_events();

    // 打印内存使用情况
    // ESP_LOGI(TAG, "可用堆内存: %" PRIu32 " 字节",
    // esp_get_free_heap_size());

    // 检查MQTT连接状态
    // mqtt_state_t state = mqtt5_client_get_state();
    // ESP_LOGI(TAG, "MQTT连接状态: %d", state);
  }

  // 清理资源（实际上永远不会执行到这里）
  if (s_isr_event_group) {
    vEventGroupDelete(s_isr_event_group);
    s_isr_event_group = NULL;
  }

  mqtt5_client_destroy();
}

/**
 * @brief 示例：在任意任务中发送AT指令
 *
 * 该函数展示了如何在任意任务中调用全局AT指令发送功能
 * 可以根据需要在其他任务中调用此函数
 *
 * @param command 要发送的AT指令字符串
 * @param timeout_ms 等待响应的超时时间(毫秒)，0表示不等待响应
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t send_at_command_from_any_task(const char *command,
                                        uint32_t timeout_ms) {
  if (command == NULL) {
    ESP_LOGE(TAG, "AT指令参数为NULL");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "从任务发送AT指令: %s, 超时: %" PRIu32 "ms", command,
           timeout_ms);

  // 调用全局AT指令发送函数
  esp_err_t ret = uart_send_global_at_command(command, timeout_ms);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "AT指令已成功添加到发送队列");
  } else {
    ESP_LOGE(TAG, "AT指令发送失败: %s", esp_err_to_name(ret));
  }

  return ret;
}

/**
 * @brief 解析百度RTC呼叫指令接口消息
 *
 * @param json_str JSON字符串
 */
static void parsebrtc_call_message(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析百度RTC呼叫指令JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "百度RTC呼叫指令JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *appid = cJSON_GetObjectItem(body, "appid");
  cJSON *token = cJSON_GetObjectItem(body, "token");
  cJSON *userid = cJSON_GetObjectItem(body, "userid");
  cJSON *roomName = cJSON_GetObjectItem(body, "roomName");

  if (!cJSON_IsString(appid) || !cJSON_IsString(token) ||
      !cJSON_IsString(userid) || !cJSON_IsString(roomName)) {
    ESP_LOGE(TAG, "百度RTC呼叫指令body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到百度RTC呼叫指令:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "appid: %s", appid->valuestring);
  ESP_LOGI(TAG, "userid: %s", userid->valuestring);
  ESP_LOGI(TAG, "roomName: %s", roomName->valuestring);

  // 在销毁引擎之前，先保存所有需要的参数到静态变量
  ESP_LOGI(TAG, "保存RTC参数到静态变量");
  memset(s_mqtt_rtc_appid, 0, sizeof(s_mqtt_rtc_appid));
  memset(s_mqtt_rtc_token, 0, sizeof(s_mqtt_rtc_token));
  memset(s_mqtt_rtc_userid, 0, sizeof(s_mqtt_rtc_userid));
  memset(s_mqtt_rtc_roomname, 0, sizeof(s_mqtt_rtc_roomname));
  memset(s_mqtt_rtc_serial, 0, sizeof(s_mqtt_rtc_serial));
  strlcpy(s_mqtt_rtc_appid, appid->valuestring, sizeof(s_mqtt_rtc_appid));
  strlcpy(s_mqtt_rtc_token, token->valuestring, sizeof(s_mqtt_rtc_token));
  strlcpy(s_mqtt_rtc_userid, userid->valuestring, sizeof(s_mqtt_rtc_userid));
  strlcpy(s_mqtt_rtc_roomname, roomName->valuestring,
          sizeof(s_mqtt_rtc_roomname));
  strlcpy(s_mqtt_rtc_serial, serial->valuestring, sizeof(s_mqtt_rtc_serial));

  // 0. 停止AI引擎并退出AI房间
  ESP_LOGI(TAG, "停止AI引擎并退出AI房间");
  byte_rtc_engine_destroy();
  vTaskDelay(pdMS_TO_TICKS(1000));

  // 1. 初始化BRTC SDK（byte_rtc_engine_destroy已经完全释放了SDK）
  ESP_LOGI(TAG, "初始化BRTC SDK");
  brtc_sdk_enable_log(1);
  int ret = brtc_sdk_init();
  if (ret < 0 && ret != 1) { // ret=1表示已经初始化成功
    ESP_LOGE(TAG, "BRTC SDK初始化失败，错误代码: %d", ret);
    send_device_command_response(serial->valuestring, "08", "500",
                                 "BRTC SDK初始化失败", "9000");
    cJSON_Delete(root);
    return;
  }
  ESP_LOGI(TAG, "BRTC SDK初始化成功");
  s_mqtt_sdk_initialized = true;

  // 3. 销毁旧的BRTC客户端（如果存在）
  if (s_mqtt_rtc_client) {
    ESP_LOGI(TAG, "销毁旧的BRTC客户端");
    brtc_logout_room(s_mqtt_rtc_client);
    brtc_destroy_client(s_mqtt_rtc_client);
    s_mqtt_rtc_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // 4. 创建BRTC客户端
  ESP_LOGI(TAG, "创建BRTC客户端");
  s_mqtt_rtc_client = brtc_create_client();
  if (!s_mqtt_rtc_client) {
    ESP_LOGE(TAG, "创建BRTC客户端失败");
    send_device_command_response(serial->valuestring, "08", "500",
                                 "创建BRTC客户端失败", "9000");
    cJSON_Delete(root);
    return;
  }

  // 5. 注册消息监听器（必须在设置参数之前）
  ESP_LOGI(TAG, "注册RTC消息监听器");
  brtc_register_message_listener(s_mqtt_rtc_client,
                                 mqtt_on_rtc_message_callback);

  // 6. 注册音频帧观察者（必须在设置参数之前）
  ESP_LOGI(TAG, "注册音频帧观察者");
  brtc_register_audio_frame_observer(s_mqtt_rtc_client, mqtt_on_audio_frame);

  // 7. 设置BRTC参数
  ESP_LOGI(TAG, "设置BRTC参数");
  brtc_set_cer(s_mqtt_rtc_client, ca_info);
  brtc_set_server_url(s_mqtt_rtc_client, brtcserver);
  brtc_set_appid(s_mqtt_rtc_client, s_mqtt_rtc_appid);

  // 9. 设置自动发布和自动订阅
  ESP_LOGI(TAG, "设置自动发布和自动订阅");
  brtc_set_auto_publish(s_mqtt_rtc_client, 1);
  brtc_set_auto_subscribe(s_mqtt_rtc_client, 1);

  // 10. 设置语音通话参数（禁用视频，仅保留音频）
  ESP_LOGI(TAG, "设置语音通话参数");
  brtc_set_audiocodec(s_mqtt_rtc_client, "pcmu"); // 设置音频编码为PCMU
  brtc_set_usingvideo(s_mqtt_rtc_client, 0);      // 禁用视频发送
  brtc_set_receivingvideo(s_mqtt_rtc_client, 0);  // 禁用视频接收

  // 10. 登录房间
  ESP_LOGI(TAG, "使用token直接登录房间: %s, userid: %s", s_mqtt_rtc_roomname,
           s_mqtt_rtc_userid);
  ret = brtc_login_room(s_mqtt_rtc_client, s_mqtt_rtc_roomname,
                        s_mqtt_rtc_userid, s_mqtt_rtc_userid, s_mqtt_rtc_token);
  if (ret != 1) {
    ESP_LOGE(TAG, "登录房间失败，错误代码: %d", ret);
    brtc_destroy_client(s_mqtt_rtc_client);
    s_mqtt_rtc_client = NULL;
    send_device_command_response(serial->valuestring, "08", "500",
                                 "登录房间失败", "9000");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "RTC房间登录请求已发送，等待服务器确认");

  cJSON_Delete(root);
}

/**
 * @brief 解析通讯录同步消息
 *
 * @param json_str JSON字符串
 */
