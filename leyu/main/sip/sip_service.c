#include "sip_service.h"
#include "audio_processor.h"
#include "esp_codec_dev.h"
#include "esp_gmf_pool.h"
#include "esp_log.h"
#include "esp_rtc.h"
#include "freertos/idf_additions.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"
#include "stdbool.h"
#include <stdbool.h>
#include <string.h>

#include "network_manager.h"
#include "esp_netif.h"

static const char *TAG = "SIP_SERVICE_V2";

static char *_get_network_ip(void) {
  esp_netif_t *active_netif = get_active_netif();
  if (active_netif) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(active_netif, &ip_info) == ESP_OK) {
        static char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        return ip_str;
    }
  }

  media_lib_ipv4_info_t ip_info;
  media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info);
  char *ip_str = media_lib_ipv4_ntoa(&ip_info.ip);
  if (ip_str == NULL) {
    ESP_LOGW(TAG, "获取网络IP失败，使用默认IP");
    // 返回静态字符串，避免返回栈上的临时字符串
    static char default_ip[] = "0.0.0.0";
    return default_ip;
  }
  return ip_str;
}
#include "brtc_app.h"
extern struct baidu_rtc_t s_bdrtc;
bool is_sip_event_calling = false;
bool is_sip_event_incoming = false;
bool is_sip_incoming = false;
bool is_sip_begin = false;
volatile bool is_audio_pipeline_ready = true;

// SIP事件类型定义
typedef enum {
  SIP_EVENT_CALLING,
  SIP_EVENT_INCOMING,
  SIP_EVENT_BEGIN,
  SIP_EVENT_END,
  SIP_EVENT_ANSWERED,
  SIP_EVENT_HANGUP,
  SIP_EVENT_REGISTERED,
  SIP_EVENT_UNREGISTERED,
  SIP_EVENT_ERROR,
} sip_event_type_t;

// SIP事件结构体
typedef struct {
  sip_event_type_t type;
  void *data;
} sip_event_t;

// SIP事件队列句柄
static QueueHandle_t sip_event_queue = NULL;

void sip_event_handler_task(void *pvParameters) {
  sip_event_t event;

  // 创建SIP事件队列
  sip_event_queue = xQueueCreate(10, sizeof(sip_event_t));
  if (sip_event_queue == NULL) {
    ESP_LOGE(TAG, "创建SIP事件队列失败");
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "SIP事件队列创建成功");

  esp_codec_dev_handle_t play_dev = get_play_dev_handle();

  for (;;) {
    // 等待接收事件
    if (xQueueReceive(sip_event_queue, &event, portMAX_DELAY) == pdTRUE) {
      switch (event.type) {
      case SIP_EVENT_CALLING:
        ESP_LOGI(TAG, "处理SIP呼叫事件");
        brtc_set_playing_state(true);
        esp_codec_dev_set_out_vol(play_dev, 45); // 降低初始通话音量，防止功放瞬间电流过大导致Brownout重启
        is_sip_mode = true;
        is_audio_pipeline_ready = false;
        audio_pipe_sip_mode(true);
        is_audio_pipeline_ready = true;
        is_sip_event_calling = true;
        is_sip_event_incoming = false;
        is_sip_flag = false;
        break;
      case SIP_EVENT_INCOMING:
        if (is_sip_incoming == false) {
          is_sip_incoming = true;
          esp_codec_dev_set_out_vol(play_dev, 45); // 降低初始来电音量，防止功放瞬间电流过大导致Brownout重启
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "有电话呼入,请等待响铃后接听！");
          brtc_set_playing_state(true);
        }
        if (brtc_is_playing() == false) {
          ESP_LOGI(TAG, "处理SIP来电事件");
          brtc_set_playing_state(true);
          is_sip_mode = true;
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(true);
          is_audio_pipeline_ready = true;
          is_sip_event_calling = false;
          is_sip_event_incoming = true;
          is_sip_flag = false;
        }
        break;
      case SIP_EVENT_BEGIN:
        if (is_sip_begin == false) {
          ESP_LOGI(TAG, "处理SIP通话开始事件");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_begin = true;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          // vTaskDelay(pdMS_TO_TICKS(500));
        }
        break;
      case SIP_EVENT_ANSWERED:
        if (is_sip_begin == false) {
          ESP_LOGI(TAG, "处理SIP通话开始事件");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_begin = true;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          // vTaskDelay(pdMS_TO_TICKS(500));
        }
        break;
      case SIP_EVENT_END:
        if (is_sip_begin == true) {
          ESP_LOGI(TAG, "处理SIP通话结束事件");
          is_sip_begin = false;
          is_sip_incoming = false;
          audio_playback_stop();
          vTaskDelay(pdMS_TO_TICKS(100));
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(false);
          is_audio_pipeline_ready = true;
          // vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_mode = false;
          brtc_sip_end();
        }
        break;
      case SIP_EVENT_HANGUP:
        if (is_sip_begin == true || is_sip_incoming == true ||
            is_sip_event_calling == true) {
          ESP_LOGI(TAG, "处理SIP通话结束事件");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_begin = false;
          is_sip_incoming = false;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          vTaskDelay(pdMS_TO_TICKS(100));
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(false);
          is_audio_pipeline_ready = true;
          // vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_mode = false;
          brtc_sip_end();
        }
        break;
      case SIP_EVENT_REGISTERED:
        ESP_LOGI(TAG, "处理SIP注册事件");
        break;
      case SIP_EVENT_UNREGISTERED:
        if (is_sip_mode) {
          ESP_LOGI(TAG, "处理SIP注销事件");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_incoming = false;
          is_sip_begin = false;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_flag = false;
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(false);
          is_audio_pipeline_ready = true;
          // vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_mode = false;
          brtc_sip_error();
          break;
        }
      case SIP_EVENT_ERROR:
        if (is_sip_mode) {
          ESP_LOGI(TAG, "处理SIP错误事件");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_incoming = false;
          is_sip_begin = false;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_flag = false;
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(false);
          is_audio_pipeline_ready = true;
          // vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_mode = false;
          brtc_sip_error();
          break;
        }
      default:
        if (is_sip_mode) {
          ESP_LOGW(TAG, "未知的SIP事件类型");
          esp_codec_dev_set_out_vol(play_dev, s_bdrtc.current_volume);
          is_sip_incoming = false;
          is_sip_begin = false;
          is_sip_event_calling = false;
          is_sip_event_incoming = false;
          audio_playback_stop();
          vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_flag = false;
          is_audio_pipeline_ready = false;
          audio_pipe_sip_mode(false);
          is_audio_pipeline_ready = true;
          // vTaskDelay(pdMS_TO_TICKS(100));
          is_sip_mode = false;
          brtc_sip_error();
          break;
        }
      }
    }
  }
}

static int _esp_sip_event_handler(esp_rtc_event_t event, void *ctx) {
  (void)ctx;
  sip_event_t sip_event = {0};
  switch ((int)event) {
  case ESP_RTC_EVENT_REGISTERED:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_REGISTERED");
    sip_event.type = SIP_EVENT_REGISTERED;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_UNREGISTERED:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_UNREGISTERED");
    sip_event.type = SIP_EVENT_UNREGISTERED;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_CALLING:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_CALLING");
    sip_event.type = SIP_EVENT_CALLING;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_INCOMING:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_INCOMING");
    sip_event.type = SIP_EVENT_INCOMING;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_AUDIO_SESSION_BEGIN:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_AUDIO_SESSION_BEGIN");
    sip_event.type = SIP_EVENT_BEGIN;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_AUDIO_SESSION_END:
    sip_event.type = SIP_EVENT_END;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_CALL_ANSWERED:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_CALL_ANSWERED");
    sip_event.type = SIP_EVENT_ANSWERED;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_HANGUP:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_HANGUP");
    sip_event.type = SIP_EVENT_HANGUP;
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  case ESP_RTC_EVENT_ERROR:
    ESP_LOGI(TAG, "ESP_RTC_EVENT_ERROR");
    sip_event.type = SIP_EVENT_ERROR;
    xQueueReset(sip_event_queue);
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  default:
    sip_event.type = SIP_EVENT_ERROR;
    xQueueReset(sip_event_queue);
    xQueueSend(sip_event_queue, &sip_event, pdMS_TO_TICKS(10));
    break;
  }
  return ESP_OK;
}

static int _send_audio_dummy(unsigned char *data, int len, void *ctx) {
  // ESP_LOGI(TAG, "send audio len=%d", len);
  (void)ctx;
  if (data == NULL || len <= 0) {
    return 0;
  }
  if (!is_audio_pipeline_ready) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
  }
  int ret = audio_recorder_read_data(data, len);
  if (ret < 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
  }
  return ret;
}

static int _receive_audio_dummy(unsigned char *data, int len, void *ctx) {
  // ESP_LOGW(TAG, "receive audio len=%d", len);
  if ((len == 6) && (data != NULL) && !strncasecmp((char *)data, "DTMF-", 5)) {
    ESP_LOGI(TAG, "Receive DTMF Event ID: %d", data[5]);
    return 0;
  }
  if (!is_audio_pipeline_ready) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return len;
  }
  // ESP_LOGW(TAG, "dummy receive audio len=%d", len);
  audio_feeder_feed_data(data, len);
  return len;
}

esp_rtc_handle_t sip_service_start(const char *uri) {
  if (uri == NULL) {
    ESP_LOGE(TAG, "uri is NULL");
    return NULL;
  }

  esp_rtc_data_cb_t data_cb = {
      .send_audio = _send_audio_dummy,
      .receive_audio = _receive_audio_dummy,
  };
  ESP_LOGI(TAG, "New SIP service start, uri=%s", uri);
  esp_rtc_config_t sip_service_config = {
      .uri = uri,
      .ctx = NULL,
      .local_addr = _get_network_ip(),
      .acodec_type = RTC_ACODEC_G711A,
      .data_cb = &data_cb,
      .event_handler = _esp_sip_event_handler,
  };

  ESP_LOGI(TAG, "start SIP service, uri=%s", uri);
  return esp_rtc_service_init(&sip_service_config);
}

int sip_service_stop(esp_rtc_handle_t esp_sip) {
  if (esp_sip) {
    return esp_rtc_service_deinit(esp_sip);
  }
  return ESP_FAIL;
}
