/**
 * @file mqtt5_client.c
 * @brief 可复用的MQTT5客户端模块实现
 * 
 * 该模块实现了一个简单易用的MQTT5客户端接口，支持在RTOS中作为独立任务运行，
 * 并提供与其他任务进行数据和事件交互的机制。
 * 
 * @author Espressif Systems
 * @version 1.0
 * @date 2023
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mqtt5_client.h"

static const char *TAG = "mqtt5_client";

/* 默认配置参数 */
#define DEFAULT_KEEPALIVE          120
#define DEFAULT_NETWORK_TIMEOUT    20000
#define MAX_USER_PROPERTIES        10

/* MQTT客户端实例结构体 */
typedef struct {
    esp_mqtt_client_handle_t client;    /**< ESP-IDF MQTT客户端句柄 */
    mqtt_config_t config;              /**< MQTT配置 */
    mqtt_state_t state;                 /**< 当前状态 */
    mqtt_event_callback_t callback;    /**< 事件回调函数 */
    void *user_data;                   /**< 用户数据 */
    TaskHandle_t task_handle;           /**< MQTT任务句柄 */
    QueueHandle_t event_queue;          /**< 事件队列 */
    esp_mqtt5_user_property_item_t user_properties[MAX_USER_PROPERTIES]; /**< 用户属性数组 */
    int user_property_count;            /**< 用户属性数量 */
    bool initialized;                   /**< 初始化标志 */
    bool running;                      /**< 运行标志 */
} mqtt_client_instance_t;

/* 全局MQTT客户端实例 */
static mqtt_client_instance_t g_mqtt_client = {0};

/**
 * @brief 内部函数：释放消息资源
 */
static void free_message(mqtt_message_t *message)
{
    if (message) {
        if (message->topic) {
            free(message->topic);
            message->topic = NULL;
        }
        if (message->data) {
            free(message->data);
            message->data = NULL;
        }
    }
}

/**
 * @brief 内部函数：复制消息
 */
static void copy_message(mqtt_message_t *dest, const mqtt_message_t *src)
{
    if (dest && src) {
        dest->topic = src->topic ? strdup(src->topic) : NULL;
        dest->data = src->data ? malloc(src->data_len) : NULL;
        if (dest->data) {
            memcpy(dest->data, src->data, src->data_len);
        }
        dest->data_len = src->data_len;
        dest->qos = src->qos;
        dest->msg_id = src->msg_id;
        dest->retain = src->retain;
    }
}

/**
 * @brief 内部函数：MQTT事件处理
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    mqtt_event_t mqtt_event = {0};
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT已连接");
        g_mqtt_client.state = MQTT_STATE_CONNECTED;
        mqtt_event.state = MQTT_STATE_CONNECTED;
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT已断开连接");
        g_mqtt_client.state = MQTT_STATE_DISCONNECTED;
        mqtt_event.state = MQTT_STATE_DISCONNECTED;
        break;
        
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT已订阅，msg_id=%d", event->msg_id);
        mqtt_event.state = MQTT_STATE_CONNECTING;
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT已取消订阅，msg_id=%d", event->msg_id);
        mqtt_event.state = MQTT_STATE_CONNECTING;
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT已发布，msg_id=%d", event->msg_id);
        mqtt_event.state = event->msg_id;
        break;
        
    case MQTT_EVENT_DATA:
                ESP_LOGI(TAG, "MQTT收到数据");
                // 设置为数据接收状态，而不是连接状态，避免触发重新订阅
                mqtt_event.state = MQTT_STATE_DATA_RECEIVED;
                mqtt_event.message.topic = malloc(event->topic_len + 1);
                if (!mqtt_event.message.topic) {
                    ESP_LOGE(TAG, "分配主题内存失败");
                    break;
                }
                memcpy(mqtt_event.message.topic, event->topic, event->topic_len);
                mqtt_event.message.topic[event->topic_len] = '\0';
                
                mqtt_event.message.data = malloc(event->data_len + 1);
                if (!mqtt_event.message.data) {
                    ESP_LOGE(TAG, "分配数据内存失败");
                    free(mqtt_event.message.topic);
                    mqtt_event.message.topic = NULL;
                    break;
                }
                memcpy(mqtt_event.message.data, event->data, event->data_len);
                mqtt_event.message.data[event->data_len] = '\0';
                
                mqtt_event.message.data_len = event->data_len;
                mqtt_event.message.qos = event->qos;
                mqtt_event.message.msg_id = event->msg_id;
                mqtt_event.message.retain = event->retain;
                
                break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT发生错误");
        g_mqtt_client.state = MQTT_STATE_ERROR;
        mqtt_event.state = MQTT_STATE_ERROR;
        mqtt_event.error_code = event->error_handle->connect_return_code;
        break;
        
    default:
        ESP_LOGI(TAG, "其他MQTT事件，ID=%d", event->event_id);
        mqtt_event.state = MQTT_STATE_IDLE;
        break;
    }
    
    /* 调用用户回调函数 */
    if (g_mqtt_client.callback) {
        g_mqtt_client.callback(&mqtt_event, g_mqtt_client.user_data);
    }
    
    /* 将事件放入事件队列 */
    if (g_mqtt_client.event_queue) {
        mqtt_event_t *event_copy = malloc(sizeof(mqtt_event_t));
        if (event_copy) {
            *event_copy = mqtt_event;
            /* 对于数据事件，需要复制消息内容 */
            if (event_id == MQTT_EVENT_DATA) {
                event_copy->message.topic = mqtt_event.message.topic;
                event_copy->message.data = mqtt_event.message.data;
                mqtt_event.message.topic = NULL;
                mqtt_event.message.data = NULL;
            }
            
            if (xQueueSend(g_mqtt_client.event_queue, &event_copy, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGE(TAG, "事件队列已满，无法添加事件");
                free_message(&event_copy->message);
                free(event_copy);
            }
        }
    }
    
    /* 清理资源 */
    free_message(&mqtt_event.message);
}


esp_err_t mqtt5_client_init(mqtt_config_t *config, mqtt_event_callback_t callback, void *user_data)
{
    if (g_mqtt_client.initialized) {
        ESP_LOGE(TAG, "MQTT客户端已经初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!config || !config->broker_uri) {
        ESP_LOGE(TAG, "无效的配置参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 复制配置 */
    memset(&g_mqtt_client, 0, sizeof(g_mqtt_client));
    memcpy(&g_mqtt_client.config, config, sizeof(mqtt_config_t));
    
    /* 设置默认值 */
    if (g_mqtt_client.config.keepalive <= 0) {
        g_mqtt_client.config.keepalive = DEFAULT_KEEPALIVE;
    }
    
    if (g_mqtt_client.config.network_timeout <= 0) {
        g_mqtt_client.config.network_timeout = DEFAULT_NETWORK_TIMEOUT;
    }
    
    /* 设置回调函数和用户数据 */
    g_mqtt_client.callback = callback;
    g_mqtt_client.user_data = user_data;
    
    /* 创建事件队列 */
    g_mqtt_client.event_queue = xQueueCreate(30, sizeof(mqtt_event_t*));
    if (!g_mqtt_client.event_queue) {
        ESP_LOGE(TAG, "创建事件队列失败");
        return ESP_ERR_NO_MEM;
    }
    
    /* 配置MQTT客户端 */
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = g_mqtt_client.config.broker_uri,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = g_mqtt_client.config.disable_auto_reconnect,
        .network.timeout_ms = g_mqtt_client.config.network_timeout,
        .credentials.username = g_mqtt_client.config.username,
        .credentials.authentication.password = g_mqtt_client.config.password,
        .credentials.client_id = g_mqtt_client.config.client_id,
        .session.keepalive = g_mqtt_client.config.keepalive,
        .session.disable_clean_session = !g_mqtt_client.config.clean_session,
        .task.stack_size = 7*1024,  // 设置为6KB，足够应对TLS握手，因为JSON解析已剥离到上层任务
        .task.priority = 5,
        .buffer.size = 1240,  // 增加缓冲区大小，减少栈使用
        .network.reconnect_timeout_ms = 10000,
    };
    
    /* 设置遗嘱消息 */
    if (g_mqtt_client.config.will_topic && g_mqtt_client.config.will_message) {
        mqtt_config.session.last_will.topic = g_mqtt_client.config.will_topic;
        mqtt_config.session.last_will.msg = g_mqtt_client.config.will_message;
        mqtt_config.session.last_will.msg_len = strlen(g_mqtt_client.config.will_message);
        mqtt_config.session.last_will.qos = g_mqtt_client.config.will_qos;
        mqtt_config.session.last_will.retain = g_mqtt_client.config.will_retain;
    }
    
    /* 初始化MQTT客户端 */
    g_mqtt_client.client = esp_mqtt_client_init(&mqtt_config);
    if (!g_mqtt_client.client) {
        ESP_LOGE(TAG, "初始化MQTT客户端失败");
        vQueueDelete(g_mqtt_client.event_queue);
        return ESP_FAIL;
    }
    
    /* 注册事件处理函数 */
    esp_err_t ret = esp_mqtt_client_register_event(g_mqtt_client.client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册MQTT事件处理函数失败");
        esp_mqtt_client_destroy(g_mqtt_client.client);
        vQueueDelete(g_mqtt_client.event_queue);
        return ret;
    }
    
    g_mqtt_client.state = MQTT_STATE_DISCONNECTED;
    g_mqtt_client.initialized = true;
    g_mqtt_client.running = false;
    
    ESP_LOGI(TAG, "MQTT客户端初始化成功");
    return ESP_OK;
}

esp_err_t mqtt5_client_start(void)
{
    if (!g_mqtt_client.initialized) {
        ESP_LOGE(TAG, "MQTT客户端未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_mqtt_client.running) {
        ESP_LOGE(TAG, "MQTT客户端已在运行");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 启动MQTT客户端 */
    esp_err_t ret = esp_mqtt_client_start(g_mqtt_client.client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动MQTT客户端失败");
        return ret;
    }
    
    g_mqtt_client.state = MQTT_STATE_CONNECTING;
    g_mqtt_client.running = true;
    
    ESP_LOGI(TAG, "MQTT客户端启动成功");
    return ESP_OK;
}

esp_err_t mqtt5_client_stop(void)
{
    if (!g_mqtt_client.initialized) {
        ESP_LOGE(TAG, "MQTT客户端未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!g_mqtt_client.running) {
        ESP_LOGE(TAG, "MQTT客户端未运行");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 停止MQTT客户端 */
    esp_err_t ret = esp_mqtt_client_stop(g_mqtt_client.client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停止MQTT客户端失败");
        return ret;
    }
    
    g_mqtt_client.running = false;
    g_mqtt_client.state = MQTT_STATE_DISCONNECTED;
    
    ESP_LOGI(TAG, "MQTT客户端停止成功");
    return ESP_OK;
}

esp_err_t mqtt5_client_destroy(void)
{
    if (!g_mqtt_client.initialized) {
        ESP_LOGE(TAG, "MQTT客户端未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 如果正在运行，先停止 */
    if (g_mqtt_client.running) {
        mqtt5_client_stop();
    }
    
    /* 销毁MQTT客户端 */
    esp_mqtt_client_destroy(g_mqtt_client.client);
    
    /* 删除队列 */
    if (g_mqtt_client.event_queue) {
        vQueueDelete(g_mqtt_client.event_queue);
        g_mqtt_client.event_queue = NULL;
    }
    
    /* 清理配置字符串 */
    if (g_mqtt_client.config.broker_uri) {
        free(g_mqtt_client.config.broker_uri);
    }
    if (g_mqtt_client.config.username) {
        free(g_mqtt_client.config.username);
    }
    if (g_mqtt_client.config.password) {
        free(g_mqtt_client.config.password);
    }
    if (g_mqtt_client.config.client_id) {
        free(g_mqtt_client.config.client_id);
    }
    if (g_mqtt_client.config.will_topic) {
        free(g_mqtt_client.config.will_topic);
    }
    if (g_mqtt_client.config.will_message) {
        free(g_mqtt_client.config.will_message);
    }
    
    /* 清理用户属性 */
    for (int i = 0; i < g_mqtt_client.user_property_count; i++) {
        if (g_mqtt_client.user_properties[i].key) {
            free((void*)g_mqtt_client.user_properties[i].key);
        }
        if (g_mqtt_client.user_properties[i].value) {
            free((void*)g_mqtt_client.user_properties[i].value);
        }
    }
    
    memset(&g_mqtt_client, 0, sizeof(g_mqtt_client));
    
    ESP_LOGI(TAG, "MQTT客户端销毁成功");
    return ESP_OK;
}

mqtt_state_t mqtt5_client_get_state(void)
{
    return g_mqtt_client.state;
}

int mqtt5_client_publish(const char *topic, const char *data, int data_len, int qos, bool retain)
{
    if (!g_mqtt_client.initialized || !g_mqtt_client.running) {
        ESP_LOGE(TAG, "MQTT客户端未初始化或未运行");
        return -1;
    }
    
    if (!topic || !data) {
        ESP_LOGE(TAG, "无效的主题或数据");
        return -1;
    }
    
    /* 设置发布属性 */
    esp_mqtt5_publish_property_config_t publish_property = {0};
    
    /* 设置用户属性 */
    if (g_mqtt_client.user_property_count > 0) {
        esp_mqtt5_client_set_user_property(&publish_property.user_property, 
                                          g_mqtt_client.user_properties, 
                                          g_mqtt_client.user_property_count);
    }
    
    /* 发布消息 */
    int msg_id = esp_mqtt_client_publish(g_mqtt_client.client, topic, data, data_len, qos, retain);
    
    /* 删除用户属性 */
    if (publish_property.user_property) {
        esp_mqtt5_client_delete_user_property(publish_property.user_property);
    }
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "发布消息失败");
    } else {
        ESP_LOGI(TAG, "发布消息成功，msg_id=%d", msg_id);
    }
    
    return msg_id;
}

int mqtt5_client_subscribe(const char *topic, int qos)
{
    if (!g_mqtt_client.initialized || !g_mqtt_client.running) {
        ESP_LOGE(TAG, "MQTT客户端未初始化或未运行");
        return -1;
    }
    
    if (!topic) {
        ESP_LOGE(TAG, "无效的主题");
        return -1;
    }
    
    /* 设置订阅属性 */
    esp_mqtt5_subscribe_property_config_t subscribe_property = {0};
    
    /* 设置用户属性 */
    if (g_mqtt_client.user_property_count > 0) {
        esp_mqtt5_client_set_user_property(&subscribe_property.user_property, 
                                          g_mqtt_client.user_properties, 
                                          g_mqtt_client.user_property_count);
    }
    
    /* 订阅主题 */
    int msg_id = esp_mqtt_client_subscribe(g_mqtt_client.client, topic, qos);
    
    /* 删除用户属性 */
    if (subscribe_property.user_property) {
        esp_mqtt5_client_delete_user_property(subscribe_property.user_property);
    }
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "订阅主题失败");
    } else {
        ESP_LOGI(TAG, "订阅主题成功，msg_id=%d", msg_id);
    }
    
    return msg_id;
}

int mqtt5_client_unsubscribe(const char *topic)
{
    if (!g_mqtt_client.initialized || !g_mqtt_client.running) {
        ESP_LOGE(TAG, "MQTT客户端未初始化或未运行");
        return -1;
    }
    
    if (!topic) {
        ESP_LOGE(TAG, "无效的主题");
        return -1;
    }
    
    /* 设置取消订阅属性 */
    esp_mqtt5_unsubscribe_property_config_t unsubscribe_property = {0};
    
    /* 设置用户属性 */
    if (g_mqtt_client.user_property_count > 0) {
        esp_mqtt5_client_set_user_property(&unsubscribe_property.user_property, 
                                          g_mqtt_client.user_properties, 
                                          g_mqtt_client.user_property_count);
    }
    
    /* 取消订阅主题 */
    int msg_id = esp_mqtt_client_unsubscribe(g_mqtt_client.client, topic);
    
    /* 删除用户属性 */
    if (unsubscribe_property.user_property) {
        esp_mqtt5_client_delete_user_property(unsubscribe_property.user_property);
    }
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "取消订阅主题失败");
    } else {
        ESP_LOGI(TAG, "取消订阅主题成功，msg_id=%d", msg_id);
    }
    
    return msg_id;
}

esp_err_t mqtt5_client_set_user_property(const char *key, const char *value)
{
    if (!key || !value) {
        ESP_LOGE(TAG, "无效的键或值");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_mqtt_client.user_property_count >= MAX_USER_PROPERTIES) {
        ESP_LOGE(TAG, "用户属性数量已达上限");
        return ESP_ERR_NO_MEM;
    }
    
    /* 检查是否已存在相同的键 */
    for (int i = 0; i < g_mqtt_client.user_property_count; i++) {
        if (g_mqtt_client.user_properties[i].key && strcmp(g_mqtt_client.user_properties[i].key, key) == 0) {
            /* 更新值 */
            if (g_mqtt_client.user_properties[i].value) {
                free((void*)g_mqtt_client.user_properties[i].value);
            }
            g_mqtt_client.user_properties[i].value = strdup(value);
            if (!g_mqtt_client.user_properties[i].value) {
                ESP_LOGE(TAG, "分配内存失败");
                return ESP_ERR_NO_MEM;
            }
            return ESP_OK;
        }
    }
    
    /* 添加新的用户属性 */
    g_mqtt_client.user_properties[g_mqtt_client.user_property_count].key = strdup(key);
    if (!g_mqtt_client.user_properties[g_mqtt_client.user_property_count].key) {
        ESP_LOGE(TAG, "分配键内存失败");
        return ESP_ERR_NO_MEM;
    }
    
    g_mqtt_client.user_properties[g_mqtt_client.user_property_count].value = strdup(value);
    if (!g_mqtt_client.user_properties[g_mqtt_client.user_property_count].value) {
        ESP_LOGE(TAG, "分配值内存失败");
        free((void*)g_mqtt_client.user_properties[g_mqtt_client.user_property_count].key);
        g_mqtt_client.user_properties[g_mqtt_client.user_property_count].key = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    g_mqtt_client.user_property_count++;
    return ESP_OK;
}

QueueHandle_t mqtt5_client_get_event_queue(void)
{
    return g_mqtt_client.event_queue;
}

