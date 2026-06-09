#include "mqtt_msg_builder.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mqtt5_client.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include <string.h>

#include "uart_command.h"
#include "contact_manager.h"
#include "at_3gpp_ts_27_007.h"

typedef struct {
  void *cdc_port;
  at_handle_t at_handle;
} at_ctx_t;

extern at_ctx_t g_at_ctx;
extern char *g_at_command_buffer;
extern char *g_at_response_buffer;
extern SemaphoreHandle_t g_at_mutex;
extern const char *firmware_version;
extern esp_err_t read_sn_from_nvs(char *buf, size_t size);

extern char device_imei[25]; // Using size 25 to match mqtt_example.c
static const char *TAG = "mqtt_msg_builder";
char *generate_timestamp(void);
char *generate_serial(void);
int mqtt5_client_publish(const char *topic, const char *data, int data_len, int qos, bool retain);

// forward declare removed

// (Extracted functions below)
bool send_heartbeat_packet(const char *status) {
  ESP_LOGI(TAG, "开始发送心跳包，status=%s", status);

  // 发送AT+MUESTATS="radio"指令获取基站信息
  char cell_data[128] = {0};

  // 添加详细的诊断日志
  ESP_LOGI(TAG,
           "AT句柄状态: at_handle=%p, command_buffer=%p, response_buffer=%p",
           g_at_ctx.at_handle, g_at_command_buffer, g_at_response_buffer);

  if (g_at_ctx.at_handle != NULL && g_at_command_buffer != NULL &&
      g_at_response_buffer != NULL && g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
    memset(g_at_command_buffer, 0, 256);
    memset(g_at_response_buffer, 0, 1024);
    strcpy(g_at_command_buffer, "AT+MUESTATS=\"radio\"");

    esp_err_t at_ret = at_send_custom_command(
        g_at_ctx.at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
    xSemaphoreGive(g_at_mutex);
    if (at_ret == ESP_OK) {
      ESP_LOGI(TAG, "AT+MUESTATS=\"radio\"指令响应: %s", g_at_response_buffer);

      // 解析响应数据
      // 格式: +MUESTATS:
      // "radio",4,-890,-660,-32768,0,0,07789130,255,140,1850,398,-30
      char radio_str[16] = {0};
      int field2, rsrp, rssi, field5, field6, field7, last_cellid, field9,
          field10, last_earfcn, last_pci, last_sinr;

      int parsed = sscanf(
          g_at_response_buffer,
          "+MUESTATS: \"%15[^\"]\",%d,%d,%d,%d,%d,%d,%x,%d,%d,%d,%d,%d",
          radio_str, &field2, &rsrp, &rssi, &field5, &field6, &field7,
          &last_cellid, &field9, &field10, &last_earfcn, &last_pci, &last_sinr);

      if (parsed == 13) {
        ESP_LOGI(TAG,
                 "基站信息解析成功: cellid=0x%x, earfcn=%d, pci=%d, rsrp=%d, "
                 "rssi=%d, sinr=%d",
                 last_cellid, last_earfcn, last_pci, rsrp, rssi, last_sinr);

        // 保存解析后的数据
        snprintf(cell_data, sizeof(cell_data), "0x%x,%d,%d,%d,%d,%d",
                 last_cellid, last_earfcn, last_pci, rsrp, rssi, last_sinr);
      } else {
        ESP_LOGW(TAG, "基站信息解析失败，parsed=%d", parsed);
      }
    } else {
      ESP_LOGW(TAG, "AT+MUESTATS=\"radio\"指令发送失败");
    }
  } else {
    ESP_LOGW(TAG, "AT句柄或缓冲区未初始化，无法获取基站信息");
  }

  // 获取ICCID
  char iccid_str[32] = {0};
  if (g_at_ctx.at_handle != NULL && g_at_command_buffer != NULL && g_at_response_buffer != NULL && 
      g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
    memset(g_at_command_buffer, 0, 256);
    memset(g_at_response_buffer, 0, 1024);
    strcpy(g_at_command_buffer, "AT+MCCID");
    esp_err_t at_ret = at_send_custom_command(g_at_ctx.at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
    xSemaphoreGive(g_at_mutex);
    if (at_ret == ESP_OK) {
      char *p = strstr(g_at_response_buffer, "+MCCID: ");
      if (p) {
        p += 8;
        int i = 0;
        while (p[i] >= '0' && p[i] <= '9' && i < 31) {
          iccid_str[i] = p[i];
          i++;
        }
        iccid_str[i] = '\0';
        ESP_LOGI(TAG, "获取MCCID成功: %s", iccid_str);
      } else {
        ESP_LOGW(TAG, "MCCID解析失败: %s", g_at_response_buffer);
      }
    }
  }

  char *timestamp = generate_timestamp();
  char *serial = generate_serial();
  if (!timestamp || !serial) {
    ESP_LOGE(TAG, "生成时间戳或流水号失败");
    if (timestamp)
      free(timestamp);
    if (serial)
      free(serial);
    return false;
  }

  ESP_LOGI(TAG, "生成时间戳: %s, 流水号: %s", timestamp, serial);

  // 获取MAC地址
  uint8_t mac[6];
  char mac_str[18] = {0};
  esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (ret == ESP_OK) {
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    strlcpy(mac_str, "00:00:00:00:00:00", sizeof(mac_str));
  }

  ESP_LOGI(TAG, "获取MAC地址: %s", mac_str);

  // 获取WiFi SSID（如果使用WiFi网络）
  char wifi_ssid[33] = {0};
  wifi_ap_record_t ap_info;
  ret = esp_wifi_sta_get_ap_info(&ap_info);
  if (ret == ESP_OK) {
    strlcpy(wifi_ssid, (char *)ap_info.ssid, sizeof(wifi_ssid));
  }

  // // 获取SN号作为IMEI
  // char imei_buffer[64] = {0};
  // read_sn_from_nvs(imei_buffer, sizeof(imei_buffer));
  // if (strlen(imei_buffer) == 0) {
  //   strlcpy(imei_buffer, device_imei, sizeof(imei_buffer));
  // }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "创建心跳包JSON对象失败");
    free(timestamp);
    free(serial);
    return false;
  }

  // 创建body对象
  cJSON *body = cJSON_CreateObject();
  if (!body) {
    ESP_LOGE(TAG, "创建心跳包body JSON对象失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }

  // 添加字段
  cJSON_AddStringToObject(root, "model", "sos");
  cJSON_AddStringToObject(root, "timeStamp", timestamp);
  cJSON_AddStringToObject(root, "serial", serial);
  cJSON_AddStringToObject(root, "imei", device_imei);

  // 添加body字段
  cJSON_AddStringToObject(body, "version", firmware_version);
  cJSON_AddStringToObject(body, "mac", mac_str);
  cJSON_AddStringToObject(body, "imei", device_imei);
  cJSON_AddStringToObject(body, "iccid", iccid_str);
  cJSON_AddStringToObject(body, "status", status);
  if (strlen(wifi_ssid) > 0) {
    cJSON_AddStringToObject(body, "wifi", wifi_ssid);
  }
  cJSON_AddStringToObject(body, "mdata", "wifi");

  // 添加cell字段
  if (strlen(cell_data) > 0) {
    cJSON *cell = cJSON_CreateObject();
    if (cell) {
      // 解析cell_data中的值
      int cellid, earfcn, pci, rsrp_val, rssi_val, sinr_val;
      sscanf(cell_data, "%x,%d,%d,%d,%d,%d", &cellid, &earfcn, &pci, &rsrp_val,
             &rssi_val, &sinr_val);

      // 添加cell字段（使用十六进制字符串表示cellid）
      char cellid_str[16] = {0};
      snprintf(cellid_str, sizeof(cellid_str), "%x", cellid);
      cJSON_AddStringToObject(cell, "last_cellid", cellid_str);
      cJSON_AddNumberToObject(cell, "last_earfcn", earfcn);
      cJSON_AddNumberToObject(cell, "last_pci", pci);
      cJSON_AddNumberToObject(cell, "rsrp", rsrp_val / 10.0);      // 转换为dBm
      cJSON_AddNumberToObject(cell, "rssi", rssi_val / 10.0);      // 转换为dBm
      cJSON_AddNumberToObject(cell, "last_sinr", sinr_val / 10.0); // 转换为dB

      cJSON_AddItemToObject(body, "cell", cell);
    }
  }

  cJSON_AddItemToObject(root, "body", body);

  // 转换为JSON字符串
  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    ESP_LOGE(TAG, "转换心跳包JSON字符串失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }

  ESP_LOGI(TAG, "心跳包JSON内容: %s", json_str);

  // 发布消息到心跳包主题
  int msg_id = mqtt5_client_publish("/device/specific/rui/status/beat",
                                    json_str, strlen(json_str), 0, false);
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "心跳包发送成功，msg_id=%d", msg_id);
    free(json_str);
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return true;
  } else {
    ESP_LOGE(TAG, "心跳包发送失败");
    free(json_str);
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }
}
bool send_contact_sync_data(void) {
  ESP_LOGI(TAG, "开始发送完整通讯录同步数据");

  // 检查MQTT连接状态
  if (mqtt5_client_get_state() != MQTT_STATE_CONNECTED) {
    ESP_LOGW(TAG, "MQTT未连接，无法发送通讯录同步数据");
    return false;
  }

  // 获取所有联系人数据
  const contact_t *contacts = NULL;
  size_t contact_count = 0;
  esp_err_t ret = contact_manager_get_all(&contacts, &contact_count);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "获取通讯录数据失败，错误代码: %d", ret);
    return false;
  }

  ESP_LOGI(TAG, "获取到 %zu 个联系人", contact_count);

  // 获取IMEI号作为SN
  char imei_buffer[64] = {0};
  read_sn_from_nvs(imei_buffer, sizeof(imei_buffer));
  if (strlen(imei_buffer) == 0) {
    strlcpy(imei_buffer, device_imei, sizeof(imei_buffer));
  }

  // 生成时间戳和流水号
  char *timestamp = generate_timestamp();
  char *serial = generate_serial();
  if (!timestamp || !serial) {
    ESP_LOGE(TAG, "生成时间戳或流水号失败");
    if (timestamp)
      free(timestamp);
    if (serial)
      free(serial);
    return false;
  }

  ESP_LOGI(TAG, "生成时间戳: %s, 流水号: %s", timestamp, serial);

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "创建通讯录同步JSON对象失败");
    free(timestamp);
    free(serial);
    return false;
  }

  // 添加字段
  cJSON_AddStringToObject(root, "timeStamp", timestamp);
  cJSON_AddStringToObject(root, "serial", serial);
  cJSON_AddStringToObject(root, "imei", imei_buffer);

  // 创建body对象
  cJSON *body = cJSON_CreateObject();
  if (!body) {
    ESP_LOGE(TAG, "创建通讯录同步body JSON对象失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }

  // 创建联系人数组
  cJSON *contacts_array = cJSON_CreateArray();
  if (!contacts_array) {
    ESP_LOGE(TAG, "创建联系人数组失败");
    cJSON_Delete(body);
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }

  // 遍历所有联系人，添加到数组中
  for (size_t i = 0; i < contact_count; i++) {
    cJSON *contact = cJSON_CreateObject();
    if (!contact) {
      ESP_LOGE(TAG, "创建联系人对象失败");
      continue;
    }

    cJSON_AddStringToObject(contact, "name", contacts[i].name);
    cJSON_AddStringToObject(contact, "phone", contacts[i].phone);

    cJSON_AddItemToArray(contacts_array, contact);
  }

  // 将联系人数组添加到body
  cJSON_AddItemToObject(body, "contacts", contacts_array);
  cJSON_AddItemToObject(root, "body", body);

  // 转换为JSON字符串
  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    ESP_LOGE(TAG, "转换通讯录同步JSON字符串失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }

  ESP_LOGI(TAG, "通讯录同步JSON内容: %s", json_str);

  // 构建MQTT主题: /device/specific/app/contact/sync/{imei}
  char topic[128] = {0};
  snprintf(topic, sizeof(topic), "/device/specific/app/contact/sync/%s",
           imei_buffer);

  // 发布消息到通讯录同步主题
  int msg_id =
      mqtt5_client_publish(topic, json_str, strlen(json_str), 0, false);
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "完整通讯录同步数据发送成功，msg_id=%d", msg_id);
    free(json_str);
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return true;
  } else {
    ESP_LOGE(TAG, "完整通讯录同步数据发送失败");
    free(json_str);
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return false;
  }
}
void send_device_command_response(const char *ori_serial,
                                         const char *busi, const char *code,
                                         const char *msg, const char *cmd_res) {
  if (mqtt5_client_get_state() != MQTT_STATE_CONNECTED) {
    ESP_LOGW(TAG, "MQTT未连接，无法发送设备指令完成反馈");
    return;
  }

  char *timestamp = generate_timestamp();
  char *serial = generate_serial();
  if (!timestamp || !serial) {
    ESP_LOGE(TAG, "生成时间戳或流水号失败");
    if (timestamp)
      free(timestamp);
    if (serial)
      free(serial);
    return;
  }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "创建JSON对象失败");
    free(timestamp);
    free(serial);
    return;
  }

  // 创建body对象
  cJSON *body = cJSON_CreateObject();
  if (!body) {
    ESP_LOGE(TAG, "创建body JSON对象失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 添加字段
  cJSON_AddStringToObject(root, "timeStamp", timestamp);
  cJSON_AddStringToObject(root, "serial", serial);
  cJSON_AddStringToObject(root, "imei", device_imei);

  cJSON_AddStringToObject(body, "oriSerial", ori_serial);
  cJSON_AddStringToObject(body, "busi", busi);
  cJSON_AddStringToObject(body, "code", code);
  cJSON_AddStringToObject(body, "msg", msg);
  if (cmd_res) {
    cJSON_AddStringToObject(body, "cmdRes", cmd_res);
  }

  cJSON_AddItemToObject(root, "body", body);

  // 转换为JSON字符串
  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    ESP_LOGE(TAG, "转换JSON字符串失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 发布消息
  int msg_id =
      mqtt5_client_publish("/device/specific/rui/send/response", json_str,
                           strlen(json_str), 0, false); // 降低QoS到0
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "设备指令完成反馈发送成功，msg_id=%d", msg_id);
  } else {
    ESP_LOGE(TAG, "设备指令完成反馈发送失败");
  }

  // 清理资源
  free(json_str);
  cJSON_Delete(root);
  free(timestamp);
  free(serial);
}
void send_voice_call_notification(const char *phone,
                                         const char *call_time) {
  if (mqtt5_client_get_state() != MQTT_STATE_CONNECTED) {
    ESP_LOGW(TAG, "MQTT未连接，无法发送语音来电通知");
    return;
  }

  char *timestamp = generate_timestamp();
  char *serial = generate_serial();
  if (!timestamp || !serial) {
    ESP_LOGE(TAG, "生成时间戳或流水号失败");
    if (timestamp)
      free(timestamp);
    if (serial)
      free(serial);
    return;
  }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "创建JSON对象失败");
    free(timestamp);
    free(serial);
    return;
  }

  // 创建body对象
  cJSON *body = cJSON_CreateObject();
  if (!body) {
    ESP_LOGE(TAG, "创建body JSON对象失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 添加字段
  cJSON_AddStringToObject(root, "timeStamp", timestamp);
  cJSON_AddStringToObject(root, "serial", serial);
  cJSON_AddStringToObject(root, "imei", device_imei);

  cJSON_AddStringToObject(body, "phone", phone);
  cJSON_AddStringToObject(body, "callTime", call_time);

  cJSON_AddItemToObject(root, "body", body);

  // 转换为JSON字符串
  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    ESP_LOGE(TAG, "转换JSON字符串失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 发布消息
  int msg_id = mqtt5_client_publish("/device/specific/rui/gsm/call", json_str,
                                    strlen(json_str), 0, false); // 降低QoS到0
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "语音来电通知发送成功，msg_id=%d", msg_id);
  } else {
    ESP_LOGE(TAG, "语音来电通知发送失败");
  }

  // 清理资源
  free(json_str);
  cJSON_Delete(root);
  free(timestamp);
  free(serial);
}
void send_sms_notification(const char *phone, const char *content) {
  if (mqtt5_client_get_state() != MQTT_STATE_CONNECTED) {
    ESP_LOGW(TAG, "MQTT未连接，无法发送短信来信通知");
    return;
  }

  char *timestamp = generate_timestamp();
  char *serial = generate_serial();
  if (!timestamp || !serial) {
    ESP_LOGE(TAG, "生成时间戳或流水号失败");
    if (timestamp)
      free(timestamp);
    if (serial)
      free(serial);
    return;
  }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "创建JSON对象失败");
    free(timestamp);
    free(serial);
    return;
  }

  // 创建body对象
  cJSON *body = cJSON_CreateObject();
  if (!body) {
    ESP_LOGE(TAG, "创建body JSON对象失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 添加字段
  cJSON_AddStringToObject(root, "timeStamp", timestamp);
  cJSON_AddStringToObject(root, "serial", serial);
  cJSON_AddStringToObject(root, "imei", device_imei);

  cJSON_AddStringToObject(body, "phone", phone);
  cJSON_AddStringToObject(body, "content", content);

  cJSON_AddItemToObject(root, "body", body);

  // 转换为JSON字符串
  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    ESP_LOGE(TAG, "转换JSON字符串失败");
    cJSON_Delete(root);
    free(timestamp);
    free(serial);
    return;
  }

  // 发布消息
  int msg_id = mqtt5_client_publish("/device/specific/rui/sms/call", json_str,
                                    strlen(json_str), 0, false); // 降低QoS到0
  if (msg_id >= 0) {
    ESP_LOGI(TAG, "短信来信通知发送成功，msg_id=%d", msg_id);
  } else {
    ESP_LOGE(TAG, "短信来信通知发送失败");
  }

  // 清理资源
  free(json_str);
  cJSON_Delete(root);
  free(timestamp);
  free(serial);
}
