#include "mqtt_msg_parser.h"
#include "mqtt_msg_builder.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "uart_command.h"
#include "at_3gpp_ts_27_007.h"
#include "contact_manager.h"
#include "call_manager.h"
#include "sip_service.h"
#include "baidu_rtc_client.h"

extern bool is_service_status;
extern esp_rtc_handle_t s_esp_sip;
extern void control_device_by_service_status(bool action);
extern esp_err_t write_key_to_nvs(const char *key, const char *value);
extern esp_err_t init_phone_numbers_from_flash(void);
extern const char *get_phone_number_by_key(int key_index);
typedef struct {
  void *cdc_port;
  at_handle_t at_handle;
} at_ctx_t;

extern at_ctx_t g_at_ctx;
extern char *g_at_command_buffer;
extern char *g_at_response_buffer;
extern SemaphoreHandle_t g_at_mutex;

static const char *TAG = "mqtt_msg_parser";
extern nvs_handle_t nvs_mqtt_handle;
extern char s_mqtt_rtc_serial[64];

esp_err_t ota_start(const char *url);
void update_contact_sync_status(int status, int success_count, int fail_count);

// (Extracted functions below)
void parse_service_status(const char *json_str) {
  ESP_LOGI(TAG, "解析服务状态JSON字符串: %s", json_str);

  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析服务状态JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "服务状态JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *action = cJSON_GetObjectItem(body, "action");

  if (!cJSON_IsString(action)) {
    ESP_LOGE(TAG, "服务状态body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "服务状态: %s", action->valuestring);
  if (strcmp(action->valuestring, "on") == 0 && is_service_status == false) {
    ESP_LOGI(TAG, "之前是关闭状态，现在切换为开启状态");
    is_service_status = true;
    control_device_by_service_status(true);
  } else if (strcmp(action->valuestring, "off") == 0 &&
             is_service_status == true) {
    ESP_LOGI(TAG, "之前是开启状态，现在切换为关闭状态");
    is_service_status = false;
    control_device_by_service_status(false);
  }
  
  cJSON_Delete(root);
}
void parse_sip_info(const char *json_str) {
  ESP_LOGI(TAG, "解析SIP信息JSON字符串: %s", json_str);
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析SIP信息JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "SIP信息JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *host = cJSON_GetObjectItem(body, "host");
  cJSON *user = cJSON_GetObjectItem(body, "user");
  cJSON *passwd = cJSON_GetObjectItem(body, "passwd");

  if (!cJSON_IsString(host) || !cJSON_IsString(user) ||
      !cJSON_IsString(passwd)) {
    ESP_LOGE(TAG, "SIP信息body格式错误");
    cJSON_Delete(root);
    return;
  }

  // 合并host、user、passwd为URI
  char uri[128];
  ESP_LOGI(TAG, "接收到SIP配置 -> host: %s, user: %s, passwd: %s", 
           host->valuestring, user->valuestring, passwd->valuestring);
  snprintf(uri, sizeof(uri), "udp://%s:%s@%s", user->valuestring,
           passwd->valuestring, host->valuestring);
           
  // 销毁可能存在的旧SIP实例，防止每次网络重连时泄漏Socket
  if (s_esp_sip != NULL) {
    ESP_LOGW(TAG, "检测到旧的SIP服务，正在销毁以释放资源...");
    sip_service_stop(s_esp_sip);
    s_esp_sip = NULL;
  }
  
  s_esp_sip = sip_service_start(uri);
  if (s_esp_sip == NULL) {
    ESP_LOGE(TAG, "SIP服务启动失败");
  }
  
  cJSON_Delete(root);
}
void parse_server_command_response(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析服务端指令完成反馈JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "服务端指令完成反馈JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *ori_serial = cJSON_GetObjectItem(body, "oriSerial");
  cJSON *busi = cJSON_GetObjectItem(body, "busi");
  cJSON *code = cJSON_GetObjectItem(body, "code");
  cJSON *msg = cJSON_GetObjectItem(body, "msg");

  if (!cJSON_IsString(ori_serial) || !cJSON_IsString(busi) ||
      !cJSON_IsString(code) || !cJSON_IsString(msg)) {
    ESP_LOGE(TAG, "服务端指令完成反馈body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到服务端指令完成反馈:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "原始流水号: %s", ori_serial->valuestring);
  ESP_LOGI(TAG, "业务类型: %s", busi->valuestring);
  ESP_LOGI(TAG, "状态码: %s", code->valuestring);
  ESP_LOGI(TAG, "消息: %s", msg->valuestring);

  // 根据业务类型和状态码处理
  if (strcmp(code->valuestring, "200") == 0) {
    ESP_LOGI(TAG, "指令执行成功");
  } else {
    ESP_LOGW(TAG, "指令执行失败: %s", msg->valuestring);
  }

  cJSON_Delete(root);
}
void parse_voice_intercept_rules(const char *json_str) {
  // 保存到NVS
  esp_err_t err = nvs_open("mqtt_data", NVS_READWRITE, &nvs_mqtt_handle);
  if (err == ESP_OK) {
      nvs_set_str(nvs_mqtt_handle, "Rules", json_str);
      nvs_commit(nvs_mqtt_handle);
      nvs_close(nvs_mqtt_handle);
      ESP_LOGI(TAG, "GSM规则已存储到NVS");
  }

  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析语音拦截规则下发JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "语音拦截规则下发JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *rules = cJSON_GetObjectItem(body, "rules");
  if (!cJSON_IsArray(rules)) {
    ESP_LOGE(TAG, "语音拦截规则body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到语音拦截规则下发:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);

  // 清除旧的拦截规则
  call_manager_clear_intercept_rules();

  // 解析规则数组
  int rules_count = cJSON_GetArraySize(rules);
  ESP_LOGI(TAG, "规则数量: %d", rules_count);

  for (int i = 0; i < rules_count; i++) {
    cJSON *rule = cJSON_GetArrayItem(rules, i);
    if (cJSON_IsString(rule)) {
      ESP_LOGI(TAG, "规则%d: %s", i + 1, rule->valuestring);
      // 将规则添加到call_manager，设置为放行
      call_manager_add_intercept_rule(rule->valuestring, false);
    }
  }

  // 发送指令完成反馈
  send_device_command_response(serial->valuestring, "01", "200", "规则设置成功",
                               "9000");

  cJSON_Delete(root);
}
void parse_voice_call_command(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析语音呼叫指令JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "语音呼叫指令JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *phone = cJSON_GetObjectItem(body, "phone");
  if (!cJSON_IsString(phone)) {
    ESP_LOGE(TAG, "语音呼叫指令body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到语音呼叫指令:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "呼叫号码: %s", phone->valuestring);

  // 这里应该执行实际的语音呼叫操作
  ESP_LOGI(TAG, "执行语音呼叫操作...");

  // 发送指令完成反馈
  send_device_command_response(serial->valuestring, "02", "200", "语音呼叫成功",
                               "9000");

  cJSON_Delete(root);
}
void parse_sms_send_command(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析短信发送指令JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "短信发送指令JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *phone = cJSON_GetObjectItem(body, "phone");
  cJSON *content = cJSON_GetObjectItem(body, "content");

  if (!cJSON_IsString(phone) || !cJSON_IsString(content)) {
    ESP_LOGE(TAG, "短信发送指令body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到短信发送指令:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "发送号码: %s", phone->valuestring);
  ESP_LOGI(TAG, "发送内容: %s", content->valuestring);

  // 这里应该执行实际的短信发送操作
  ESP_LOGI(TAG, "执行短信发送操作...");

  // 发送指令完成反馈
  send_device_command_response(serial->valuestring, "03", "200", "短信发送成功",
                               "9000");

  cJSON_Delete(root);
}
void parse_at_command(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析AT指令JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "AT指令JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *cmd = cJSON_GetObjectItem(body, "cmd");
  if (!cJSON_IsString(cmd)) {
    ESP_LOGE(TAG, "AT指令body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到AT指令:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "指令: %s", cmd->valuestring);

  // 检查AT句柄和缓冲区是否已初始化
  if (g_at_ctx.at_handle == NULL || g_at_command_buffer == NULL ||
      g_at_response_buffer == NULL) {
    ESP_LOGE(TAG, "AT句柄或缓冲区未初始化");
    send_device_command_response(serial->valuestring, "04", "500",
                                 "AT句柄或缓冲区未初始化", "9001");
    cJSON_Delete(root);
    return;
  }

  // 清空缓冲区和发送AT指令
  if (g_at_mutex != NULL && xSemaphoreTake(g_at_mutex, portMAX_DELAY) == pdTRUE) {
      memset(g_at_command_buffer, 0, 256);
      memset(g_at_response_buffer, 0, 1024);
      snprintf(g_at_command_buffer, 256, "%s\r\n", cmd->valuestring);

      // 使用USB端口发送AT指令
      esp_err_t ret = at_send_custom_command(
          g_at_ctx.at_handle, g_at_command_buffer, g_at_response_buffer, 1024);
      xSemaphoreGive(g_at_mutex);
      
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AT指令已通过USB端口发送: %s", g_at_command_buffer);
        ESP_LOGI(TAG, "AT指令响应: %s", g_at_response_buffer);
        // 发送指令完成反馈，将AT指令响应放到cmdRes字段
        send_device_command_response(serial->valuestring, "04", "200",
                                     "AT指令执行成功", g_at_response_buffer);
      } else {
        ESP_LOGE(TAG, "AT指令发送失败: %s", esp_err_to_name(ret));
        // 发送指令失败反馈
        send_device_command_response(serial->valuestring, "04", "500",
                                     "AT指令执行失败", "9001");
      }
  } else {
      ESP_LOGE(TAG, "无法获取 AT Mutex");
      send_device_command_response(serial->valuestring, "04", "500",
                                   "系统繁忙无法发送AT指令", "9001");
  }

  cJSON_Delete(root);
}
void parse_key_bind_message(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析设备绑定JSON失败");
    return;
  }

  // 解析key1, key2, key3字段
  cJSON *key1 = cJSON_GetObjectItem(root, "key1");
  cJSON *key2 = cJSON_GetObjectItem(root, "key2");
  cJSON *key3 = cJSON_GetObjectItem(root, "key3");

  if (!cJSON_IsString(key1) || !cJSON_IsString(key2) || !cJSON_IsString(key3)) {
    ESP_LOGE(TAG, "设备绑定JSON格式错误，缺少key1/key2/key3字段");
    cJSON_Delete(root);
    return;
  }

  const char *key1_value = key1->valuestring;
  const char *key2_value = key2->valuestring;
  const char *key3_value = key3->valuestring;

  ESP_LOGI(TAG, "收到设备绑定消息: key1=%s, key2=%s, key3=%s", key1_value,
           key2_value, key3_value);

  // 保存到flash中
  esp_err_t ret1 = write_key_to_nvs("key1", key1_value);
  esp_err_t ret2 = write_key_to_nvs("key2", key2_value);
  esp_err_t ret3 = write_key_to_nvs("key3", key3_value);

  if (ret1 == ESP_OK && ret2 == ESP_OK && ret3 == ESP_OK) {
    ESP_LOGI(TAG, "设备绑定信息保存成功");

    // 重新初始化PSRAM中的号码数组，从Flash中读取最新的号码
    esp_err_t psram_ret = init_phone_numbers_from_flash();
    if (psram_ret == ESP_OK) {
      ESP_LOGI(TAG, "PSRAM中的号码数组已更新");
      // 验证更新结果
      const char *updated_key1 = get_phone_number_by_key(0);
      const char *updated_key2 = get_phone_number_by_key(1);
      const char *updated_key3 = get_phone_number_by_key(2);
      ESP_LOGI(TAG, "验证PSRAM更新结果: key1=%s, key2=%s, key3=%s",
               updated_key1 ? updated_key1 : "NULL",
               updated_key2 ? updated_key2 : "NULL",
               updated_key3 ? updated_key3 : "NULL");
    } else {
      ESP_LOGE(TAG, "更新PSRAM中的号码数组失败");
    }

    // 发送成功响应
    send_device_command_response("key_bind", "key_bind", "200",
                                 "设备绑定信息保存成功", NULL);
  } else {
    ESP_LOGE(TAG, "设备绑定信息保存失败");
    // 发送失败响应
    send_device_command_response("key_bind", "key_bind", "500",
                                 "设备绑定信息保存失败", NULL);
  }

  cJSON_Delete(root);
}
void parse_ota_upgrade_command(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析OTA升级指令JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "OTA升级指令JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  // 获取body内容
  cJSON *version = cJSON_GetObjectItem(body, "version");
  cJSON *url = cJSON_GetObjectItem(body, "url");

  if (!cJSON_IsString(version) || !cJSON_IsString(url)) {
    ESP_LOGE(TAG, "OTA升级指令body格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到OTA升级指令:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);
  ESP_LOGI(TAG, "版本: %s", version->valuestring);
  ESP_LOGI(TAG, "URL: %s", url->valuestring);

  // 发送指令完成反馈
  send_device_command_response(serial->valuestring, "05", "200",
                               "OTA升级指令接收成功", "9000");

  // 启动OTA升级
  ESP_LOGI(TAG, "启动OTA升级流程");
  esp_err_t ret = ota_start(url->valuestring);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "启动OTA升级失败: %s", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "OTA升级已启动");
  }

  cJSON_Delete(root);
}
void parse_contact_sync_message(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "解析通讯录同步JSON失败");
    return;
  }

  // 获取字段
  cJSON *timestamp = cJSON_GetObjectItem(root, "timeStamp");
  cJSON *serial = cJSON_GetObjectItem(root, "serial");
  cJSON *imei = cJSON_GetObjectItem(root, "imei");
  cJSON *body = cJSON_GetObjectItem(root, "body");

  if (!cJSON_IsString(timestamp) || !cJSON_IsString(serial) ||
      !cJSON_IsString(imei) || !cJSON_IsObject(body)) {
    ESP_LOGE(TAG, "通讯录同步JSON格式错误");
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到通讯录同步消息:");
  ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
  ESP_LOGI(TAG, "流水号: %s", serial->valuestring);
  ESP_LOGI(TAG, "IMEI: %s", imei->valuestring);

  // 获取body内容中的contacts数组
  cJSON *contacts = cJSON_GetObjectItem(body, "contacts");
  if (!cJSON_IsArray(contacts)) {
    ESP_LOGE(TAG, "通讯录同步body格式错误，缺少contacts数组");
    cJSON_Delete(root);
    return;
  }

  // 获取联系人数量
  int contact_count = cJSON_GetArraySize(contacts);
  if (contact_count <= 0 || contact_count > 1000) {
    ESP_LOGE(TAG, "联系人数量无效: %d", contact_count);
    cJSON_Delete(root);
    return;
  }

  ESP_LOGI(TAG, "收到 %d 个联系人", contact_count);

  // 分配临时内存存储联系人数据
  contact_t *temp_contacts =
      (contact_t *)malloc(contact_count * sizeof(contact_t));
  if (temp_contacts == NULL) {
    ESP_LOGE(TAG, "分配临时联系人内存失败");
    cJSON_Delete(root);
    return;
  }

  // 解析联系人数据
  int valid_count = 0;
  for (int i = 0; i < contact_count; i++) {
    cJSON *contact_item = cJSON_GetArrayItem(contacts, i);
    if (!cJSON_IsObject(contact_item)) {
      ESP_LOGW(TAG, "联系人 %d 不是对象类型", i);
      continue;
    }

    cJSON *name = cJSON_GetObjectItem(contact_item, "name");
    cJSON *phone = cJSON_GetObjectItem(contact_item, "phone");

    if (!cJSON_IsString(name) || !cJSON_IsString(phone)) {
      ESP_LOGW(TAG, "联系人 %d 缺少name或phone字段", i);
      continue;
    }

    // 检查长度是否合法
    if (strlen(name->valuestring) == 0 ||
        strlen(name->valuestring) >= MAX_NAME_LENGTH) {
      ESP_LOGW(TAG, "联系人 %d 姓名长度无效", i);
      continue;
    }

    if (strlen(phone->valuestring) == 0 ||
        strlen(phone->valuestring) >= MAX_PHONE_LENGTH) {
      ESP_LOGW(TAG, "联系人 %d 电话长度无效", i);
      continue;
    }

    // 复制联系人数据
    strncpy(temp_contacts[valid_count].name, name->valuestring,
            MAX_NAME_LENGTH - 1);
    temp_contacts[valid_count].name[MAX_NAME_LENGTH - 1] = '\0';
    strncpy(temp_contacts[valid_count].phone, phone->valuestring,
            MAX_PHONE_LENGTH - 1);
    temp_contacts[valid_count].phone[MAX_PHONE_LENGTH - 1] = '\0';

    ESP_LOGI(TAG, "联系人 %d: %s - %s", valid_count + 1,
             temp_contacts[valid_count].name, temp_contacts[valid_count].phone);
    valid_count++;
  }

  cJSON_Delete(root);

  if (valid_count == 0) {
    ESP_LOGE(TAG, "没有有效的联系人数据");
    free(temp_contacts);
    return;
  }

  // 更新通讯录
  ESP_LOGI(TAG, "开始更新通讯录，有效联系人数量: %d", valid_count);
  esp_err_t ret = contact_manager_update(temp_contacts, valid_count);
  free(temp_contacts);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "通讯录更新成功");
    // 发送成功响应
    send_device_command_response(serial->valuestring, "09", "200",
                                 "通讯录同步成功", "9000");
  } else {
    ESP_LOGE(TAG, "通讯录更新失败: %s", esp_err_to_name(ret));
    // 发送失败响应
    send_device_command_response(serial->valuestring, "09", "500",
                                 "通讯录同步失败", "9000");
  }
}