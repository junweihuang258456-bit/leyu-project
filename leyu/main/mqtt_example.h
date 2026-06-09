/**
 * @file mqtt_example.h
 * @brief 专项市场接口规范MQTT接入头文件
 * 
 * 该头文件声明了专项市场接口规范中的所有MQTT收发函数，包括：
 * 1. 设备指令完成反馈接口 (/device/specific/rui/send/response)
 * 2. 服务端指令完成反馈接口 (/device/specific/app/send/response/{imei})
 * 3. 语音拦截规则下发接口 (/device/specific/app/gsm/rules/{imei})
 * 4. 语音呼叫指令接口 (/device/specific/app/gsm/call/{imei})
 * 5. 短信发送指令接口 (/device/specific/app/sms/call/{imei})
 * 6. AT指令发送接口 (/device/specific/app/at/call/{imei})
 * 7. 语音来电接口 (/device/specific/rui/gsm/call)
 * 8. 短信来信接口 (/device/specific/rui/sms/call)
 * 9. 通讯录同步接口 (/device/specific/app/contact/sync/{imei})
 * 
 * @author Espressif Systems
 * @version 1.0
 * @date 2023
 */

#ifndef MQTT_EXAMPLE_H
#define MQTT_EXAMPLE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化MQTT应用
 * 
 * 该函数初始化MQTT客户端，配置连接参数，并启动MQTT服务。
 * 同时创建多个任务处理MQTT事件和消息。
 */
void mqtt_app_init(void);

/**
 * @brief 从NVS中读取key值
 * 
 * @param key 键名
 * @param value_buffer 值缓冲区
 * @param buffer_size 缓冲区大小
 * @return esp_err_t 操作结果
 */
esp_err_t read_key_from_nvs(const char *key, char *value_buffer, size_t buffer_size);

/**
 * @brief 获取设备IMEI号
 * 
 * @return const char* 设备IMEI号
 */
extern char device_imei[25];

/**
 * @brief 生成时间戳
 * 
 * @return char* 时间戳字符串，调用者需释放内存
 */
char* generate_timestamp(void);

/**
 * @brief 生成流水号
 * 
 * @return char* 流水号字符串，调用者需释放内存
 */
char* generate_serial(void);

/**
 * @brief 发送心跳包
 * 
 * @param status 状态值："on"、"heartbeat"或"off"
 * @return true 发送成功，false 发送失败
 */
bool send_heartbeat_packet(const char *status);


/**
 * @brief 发送完整通讯录同步数据到服务器
 * 
 * @return true 发送成功，false 发送失败
 */
bool send_contact_sync_data(void);

void mqtt_flash_load(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_EXAMPLE_H */