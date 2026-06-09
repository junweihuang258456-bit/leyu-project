/**
 * @file mqtt5_client.h
 * @brief 可复用的MQTT5客户端模块
 * 
 * 该模块提供了一个简单易用的MQTT5客户端接口，支持在RTOS中作为独立任务运行，
 * 并提供与其他任务进行数据和事件交互的机制。
 * 
 * @author Espressif Systems
 * @version 1.0
 * @date 2023
 */

#ifndef MQTT5_CLIENT_H
#define MQTT5_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT连接状态枚举
 */
typedef enum {
    MQTT_STATE_DISCONNECTED = 0,  /**< 未连接状态 */
    MQTT_STATE_CONNECTING,         /**< 连接中状态 */
    MQTT_STATE_CONNECTED,          /**< 已连接状态 */
    MQTT_STATE_RECONNECTING,       /**< 重连中状态 */
    MQTT_STATE_DATA_RECEIVED,      /**< 数据接收状态 */
    MQTT_STATE_ERROR,              /**< 错误状态 */
    MQTT_STATE_IDLE                /**< 空闲状态 */
} mqtt_state_t;

/**
 * @brief MQTT消息结构体
 */
typedef struct {
    char *topic;                   /**< 主题 */
    char *data;                    /**< 数据 */
    int data_len;                  /**< 数据长度 */
    int qos;                       /**< 服务质量等级 */
    int msg_id;                    /**< 消息ID */
    bool retain;                   /**< 保留标志 */
} mqtt_message_t;

/**
 * @brief MQTT事件结构体
 */
typedef struct {
    mqtt_state_t state;            /**< 当前状态 */
    int error_code;                /**< 错误代码 */
    mqtt_message_t message;        /**< 消息内容 */
} mqtt_event_t;

/**
 * @brief MQTT配置结构体
 */
typedef struct {
    char *broker_uri;              /**< 代理服务器URI */
    char *username;                /**< 用户名 */
    char *password;                /**< 密码 */
    char *client_id;               /**< 客户端ID */
    char *will_topic;              /**< 遗嘱主题 */
    char *will_message;            /**< 遗嘱消息 */
    int will_qos;                  /**< 遗嘱QoS */
    bool will_retain;              /**< 遗嘱保留标志 */
    int keepalive;                 /**< 保活时间(秒) */
    bool disable_auto_reconnect;   /**< 禁用自动重连 */
    int network_timeout;           /**< 网络超时时间(毫秒) */
    bool clean_session;            /**< 清除会话标志 */
} mqtt_config_t;

/**
 * @brief MQTT回调函数类型
 */
typedef void (*mqtt_event_callback_t)(mqtt_event_t *event, void *user_data);

/**
 * @brief 初始化MQTT客户端
 * 
 * @param config MQTT配置参数
 * @param callback 事件回调函数
 * @param user_data 用户数据
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t mqtt5_client_init(mqtt_config_t *config, mqtt_event_callback_t callback, void *user_data);

/**
 * @brief 启动MQTT客户端
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t mqtt5_client_start(void);

/**
 * @brief 停止MQTT客户端
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t mqtt5_client_stop(void);

/**
 * @brief 销毁MQTT客户端
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t mqtt5_client_destroy(void);

/**
 * @brief 获取当前MQTT连接状态
 * 
 * @return mqtt_state_t 当前连接状态
 */
mqtt_state_t mqtt5_client_get_state(void);

/**
 * @brief 发布消息
 * 
 * @param topic 主题
 * @param data 数据
 * @param data_len 数据长度
 * @param qos 服务质量等级
 * @param retain 保留标志
 * @return int 消息ID，负值表示失败
 */
int mqtt5_client_publish(const char *topic, const char *data, int data_len, int qos, bool retain);

/**
 * @brief 订阅主题
 * 
 * @param topic 主题
 * @param qos 服务质量等级
 * @return int 消息ID，负值表示失败
 */
int mqtt5_client_subscribe(const char *topic, int qos);

/**
 * @brief 取消订阅主题
 * 
 * @param topic 主题
 * @return int 消息ID，负值表示失败
 */
int mqtt5_client_unsubscribe(const char *topic);

/**
 * @brief 设置用户属性
 * 
 * @param key 键
 * @param value 值
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t mqtt5_client_set_user_property(const char *key, const char *value);

/**
 * @brief 获取事件队列句柄
 * 
 * @return QueueHandle_t 事件队列句柄
 */
QueueHandle_t mqtt5_client_get_event_queue(void);

/**
 * @brief 获取消息队列句柄
 * 
 * @return QueueHandle_t 消息队列句柄
 */
QueueHandle_t mqtt5_client_get_message_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT5_CLIENT_H */