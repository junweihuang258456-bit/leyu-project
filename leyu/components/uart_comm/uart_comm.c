/**
 * UART通信模块实现
 * 用于与4G模组或其他串口设备通信
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#include "uart_comm.h"

static const char *TAG = "uart_comm";

// UART通信任务句柄
static TaskHandle_t uart_comm_task_handle = NULL;
// 响应队列句柄
static QueueHandle_t response_queue = NULL;
// UART互斥锁
static SemaphoreHandle_t uart_mutex = NULL;
// 全局AT指令队列
static QueueHandle_t global_at_command_queue = NULL;
// 全局AT指令信号量
static SemaphoreHandle_t global_at_command_sem = NULL;
// 全局AT指令响应数据
static at_command_response_t global_at_response = {0};
// AT指令响应完成信号量
static SemaphoreHandle_t at_response_sem = NULL;

// UART通信任务
static void uart_comm_task(void *arg)
{
    uint8_t *data = (uint8_t *) malloc(UART_COMM_BUF_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "分配UART接收缓冲区失败");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART通信任务已启动");
    
    // 定期发送AT指令的计数器
    static uint32_t last_at_time = 0;
    static uint32_t at_count = 0;
    const char *at_command = "AT\r\n";
    
    // 立即发送一次AT指令
    uart_write_bytes(UART_COMM_PORT_NUM, at_command, strlen(at_command));
    ESP_LOGI(TAG, "已发送AT指令: %s", at_command);
    at_count++;
    last_at_time = xTaskGetTickCount();
    
    while (1) {
        // 检查是否有全局AT指令需要处理
        if (global_at_command_queue != NULL && uxQueueMessagesWaiting(global_at_command_queue) > 0) {
            global_at_command_t at_cmd;
            if (xQueueReceive(global_at_command_queue, &at_cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
                // 发送AT指令
                int cmd_len = strlen(at_cmd.command);
                int bytes_written = uart_write_bytes(UART_COMM_PORT_NUM, at_cmd.command, cmd_len);
                if (bytes_written != cmd_len) {
                    ESP_LOGE(TAG, "发送全局AT指令失败: 期望%d字节，实际%d字节", cmd_len, bytes_written);
                } else {
                    ESP_LOGI(TAG, "发送全局AT指令: %s", at_cmd.command);
                    
                    // 如果需要等待响应
                    if (at_cmd.need_response) {
                        // 初始化响应数据结构
                        memset(&global_at_response, 0, sizeof(global_at_response));
                        global_at_response.timestamp = xTaskGetTickCount();
                        
                        char response_buffer[UART_COMM_BUF_SIZE] = {0};
                        bool response_received = false;
                        TickType_t start_ticks = xTaskGetTickCount();
                        TickType_t timeout_ticks = pdMS_TO_TICKS(at_cmd.timeout_ms);
                        
                        // 等待响应
                        while ((xTaskGetTickCount() - start_ticks) < timeout_ticks) {
                            // 从UART读取数据
                            int len = uart_read_bytes(UART_COMM_PORT_NUM, data, (UART_COMM_BUF_SIZE - 1), 
                                                      pdMS_TO_TICKS(100));
                            
                            if (len > 0) {
                                data[len] = '\0';
                                ESP_LOGI(TAG, "全局AT指令响应: %s", (char *)data);
                                
                                // 将响应数据放入队列
                                if (response_queue != NULL) {
                                    xQueueSend(response_queue, data, pdMS_TO_TICKS(100));
                                }
                                
                                // 拼接响应数据到全局响应缓冲区
                                if (strlen(response_buffer) + len < sizeof(response_buffer) - 1) {
                                    strcat(response_buffer, (char *)data);
                                }
                                
                                // 检查是否包含"OK"或"ERROR"
                                if (strstr((char *)data, "OK") != NULL || strstr((char *)data, "ERROR") != NULL) {
                                    response_received = true;
                                    break;
                                }
                            }
                        }
                        
                        // 存储响应数据到全局结构体
                        if (response_received) {
                            strncpy(global_at_response.response, response_buffer, sizeof(global_at_response.response) - 1);
                            global_at_response.response[sizeof(global_at_response.response) - 1] = '\0';
                            global_at_response.response_len = strlen(global_at_response.response);
                            global_at_response.is_valid = true;
                            ESP_LOGI(TAG, "AT指令响应已保存: %s", global_at_response.response);
                        } else {
                            ESP_LOGW(TAG, "全局AT指令响应超时");
                            global_at_response.is_valid = false;
                        }
                        
                        // 触发响应完成信号量
                        xSemaphoreGive(at_response_sem);
                    }
                }
            }
        }
        
        // 从UART读取数据
        int len = uart_read_bytes(UART_COMM_PORT_NUM, data, (UART_COMM_BUF_SIZE - 1), 
                                  pdMS_TO_TICKS(100));  // 减少超时时间，提高响应性
        
        if (len > 0) {
            data[len] = '\0';
            ESP_LOGI(TAG, "接收到数据: %s", (char *)data);
            
            // 将响应数据放入队列
            if (response_queue != NULL) {
                xQueueSend(response_queue, data, pdMS_TO_TICKS(100));
            }
        }
        
        // 定期检查任务删除请求
        if (uart_comm_task_handle == NULL) {
            break;  // 如果任务句柄被清空，退出循环
        }
        
        // 每20秒发送一次AT指令
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_at_time) >= pdMS_TO_TICKS(20000)) {
            last_at_time = current_time;
            at_count++;
            
            // 检查可用内存
            size_t free_heap = esp_get_free_heap_size();
            ESP_LOGI(TAG, "=== 第%" PRIu32 "次发送AT指令，当前可用内存: %" PRIu32 " 字节 ===", 
                     at_count, (uint32_t)free_heap);
            
            // 如果内存过低，记录警告
            if (free_heap < 10000) {  // 如果可用内存小于10KB
                ESP_LOGW(TAG, "警告：可用内存过低！仅剩 %" PRIu32 " 字节", (uint32_t)free_heap);
            }
            
            uart_write_bytes(UART_COMM_PORT_NUM, at_command, strlen(at_command));
        }
    }
    
    // 释放内存并删除任务
    free(data);
    vTaskDelete(NULL);
}

esp_err_t uart_comm_init(void)
{
    esp_err_t ret = ESP_OK;
    
    // 创建互斥锁
    uart_mutex = xSemaphoreCreateMutex();
    if (uart_mutex == NULL) {
        ESP_LOGE(TAG, "创建UART互斥锁失败");
        return ESP_ERR_NO_MEM;
    }
    
    // 创建响应队列
    response_queue = xQueueCreate(10, UART_COMM_BUF_SIZE);
    if (response_queue == NULL) {
        ESP_LOGE(TAG, "创建响应队列失败");
        vSemaphoreDelete(uart_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // 创建全局AT指令队列
    global_at_command_queue = xQueueCreate(5, sizeof(global_at_command_t));
    if (global_at_command_queue == NULL) {
        ESP_LOGE(TAG, "创建全局AT指令队列失败");
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // 创建全局AT指令信号量
    global_at_command_sem = xSemaphoreCreateBinary();
    if (global_at_command_sem == NULL) {
        ESP_LOGE(TAG, "创建全局AT指令信号量失败");
        vQueueDelete(global_at_command_queue);
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // 创建AT指令响应完成信号量
    at_response_sem = xSemaphoreCreateBinary();
    if (at_response_sem == NULL) {
        ESP_LOGE(TAG, "创建AT指令响应完成信号量失败");
        vSemaphoreDelete(global_at_command_sem);
        vQueueDelete(global_at_command_queue);
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // 配置UART参数
    uart_config_t uart_config = {
        .baud_rate = UART_COMM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // 配置GPIO引脚模式为UART功能
    gpio_set_direction(UART_COMM_TXD, GPIO_MODE_OUTPUT);
    gpio_set_direction(UART_COMM_RXD, GPIO_MODE_INPUT);
    gpio_set_pull_mode(UART_COMM_RXD, GPIO_PULLUP_ONLY);  // 启用上拉电阻
    
    // 安装UART驱动程序
    ret = uart_driver_install(UART_COMM_PORT_NUM, UART_COMM_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "安装UART驱动程序失败: %s", esp_err_to_name(ret));
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ret;
    }
    
    // 配置UART参数
    ret = uart_param_config(UART_COMM_PORT_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置UART参数失败: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_COMM_PORT_NUM);
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ret;
    }
    
    // 设置UART引脚
    ret = uart_set_pin(UART_COMM_PORT_NUM, UART_COMM_TXD, UART_COMM_RXD, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置UART引脚失败: %s", esp_err_to_name(ret));
        uart_driver_delete(UART_COMM_PORT_NUM);
        vQueueDelete(response_queue);
        vSemaphoreDelete(uart_mutex);
        return ret;
    }
    
    ESP_LOGI(TAG, "UART初始化成功: 端口=%d, 波特率=%d, TXD=GPIO%d, RXD=GPIO%d", 
             UART_COMM_PORT_NUM, UART_COMM_BAUD_RATE, UART_COMM_TXD, UART_COMM_RXD);
    
    return ESP_OK;
}

esp_err_t uart_send_and_wait(const char *command, char *response, size_t response_len, uint32_t timeout_ms)
{
    if (command == NULL || response == NULL) {
        ESP_LOGE(TAG, "参数为NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (uart_mutex == NULL) {
        ESP_LOGE(TAG, "UART未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 获取互斥锁
    if (xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "获取UART互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }
    
    // 清空响应队列中的旧数据
    if (response_queue != NULL) {
        xQueueReset(response_queue);
    }
    
    // 发送命令
    int cmd_len = strlen(command);
    int bytes_written = uart_write_bytes(UART_COMM_PORT_NUM, command, cmd_len);
    if (bytes_written != cmd_len) {
        ESP_LOGE(TAG, "发送命令失败: 期望%d字节，实际%d字节", cmd_len, bytes_written);
        xSemaphoreGive(uart_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "已发送命令: %s", command);
    
    // 等待响应
    uint8_t rx_data[UART_COMM_BUF_SIZE];
    bool response_received = false;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    
    // 清空响应缓冲区
    memset(response, 0, response_len);
    size_t response_offset = 0;
    
    while ((xTaskGetTickCount() - start_ticks) < timeout_ticks) {
        // 检查响应队列
        if (response_queue != NULL && uxQueueMessagesWaiting(response_queue) > 0) {
            if (xQueueReceive(response_queue, rx_data, pdMS_TO_TICKS(100)) == pdTRUE) {
                size_t rx_len = strlen((char *)rx_data);
                // 确保不会超出响应缓冲区大小
                if (response_offset + rx_len < response_len - 1) {
                    memcpy(response + response_offset, rx_data, rx_len);
                    response_offset += rx_len;
                    response[response_offset] = '\0';
                    response_received = true;
                } else {
                    ESP_LOGW(TAG, "响应缓冲区已满");
                    break;
                }
            }
        }
        
        // 如果已经收到响应且包含"OK"或"ERROR"，则提前退出
        if (response_received) {
            if (strstr(response, "OK") != NULL || strstr(response, "ERROR") != NULL) {
                break;
            }
        }
        
        // 短暂延时以避免忙等待
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // 释放互斥锁
    xSemaphoreGive(uart_mutex);
    
    if (response_received) {
        ESP_LOGI(TAG, "接收到响应: %s", response);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "等待响应超时");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t uart_send(const char *command)
{
    if (command == NULL) {
        ESP_LOGE(TAG, "参数为NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (uart_mutex == NULL) {
        ESP_LOGE(TAG, "UART未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 获取互斥锁
    if (xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "获取UART互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }
    
    // 发送命令
    int cmd_len = strlen(command);
    int bytes_written = uart_write_bytes(UART_COMM_PORT_NUM, command, cmd_len);
    if (bytes_written != cmd_len) {
        ESP_LOGE(TAG, "发送命令失败: 期望%d字节，实际%d字节", cmd_len, bytes_written);
        xSemaphoreGive(uart_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "已发送命令: %s", command);
    
    // 释放互斥锁
    xSemaphoreGive(uart_mutex);
    
    return ESP_OK;
}

esp_err_t uart_comm_task_start(void)
{
    if (uart_comm_task_handle != NULL) {
        ESP_LOGW(TAG, "UART通信任务已启动");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查可用内存
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "启动UART通信任务前可用内存: %" PRIu32 " 字节", (uint32_t)free_heap);
    
    if (free_heap < UART_COMM_TASK_STACK + 2048) {  // 预留额外的2KB内存
        ESP_LOGE(TAG, "内存不足，无法启动UART通信任务");
        return ESP_ERR_NO_MEM;
    }
    
    BaseType_t ret = xTaskCreate(uart_comm_task, "uart_comm_task", 
                                UART_COMM_TASK_STACK, NULL, 5, &uart_comm_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建UART通信任务失败");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "UART通信任务启动成功");
    return ESP_OK;
}

esp_err_t uart_comm_task_stop(void)
{
    if (uart_comm_task_handle == NULL) {
        ESP_LOGW(TAG, "UART通信任务未启动");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 先清空任务句柄，通知任务退出循环
    TaskHandle_t temp_handle = uart_comm_task_handle;
    uart_comm_task_handle = NULL;
    
    // 等待任务自然退出
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 删除任务
    vTaskDelete(temp_handle);
    
    // 清空队列
    if (response_queue != NULL) {
        xQueueReset(response_queue);
    }
    
    ESP_LOGI(TAG, "UART通信任务已停止");
    return ESP_OK;
}

esp_err_t uart_send_global_at_command(const char *command, uint32_t timeout_ms)
{
    if (command == NULL) {
        ESP_LOGE(TAG, "AT指令参数为NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (global_at_command_queue == NULL || global_at_command_sem == NULL) {
        ESP_LOGE(TAG, "全局AT指令模块未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查指令长度
    if (strlen(command) >= sizeof(((global_at_command_t *)0)->command)) {
        ESP_LOGE(TAG, "AT指令长度超过限制");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 构造AT指令结构体
    global_at_command_t at_cmd;
    strncpy(at_cmd.command, command, sizeof(at_cmd.command) - 1);
    at_cmd.command[sizeof(at_cmd.command) - 1] = '\0';
    at_cmd.timeout_ms = timeout_ms;
    at_cmd.need_response = (timeout_ms > 0);
    
    // 将AT指令放入队列
    if (xQueueSend(global_at_command_queue, &at_cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "全局AT指令队列已满");
        return ESP_ERR_NO_MEM;
    }
    
    // 触发信号量，通知任务处理AT指令
    xSemaphoreGive(global_at_command_sem);
    
    ESP_LOGI(TAG, "已添加全局AT指令到队列: %s, 超时: %" PRIu32 "ms", command, timeout_ms);
    
    // 如果需要等待响应，则等待响应完成
    if (at_cmd.need_response) {
        // 等待响应完成，使用指定的超时时间加上额外的缓冲时间
        esp_err_t ret = uart_wait_for_at_response(timeout_ms + 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "等待AT指令响应失败: %s", esp_err_to_name(ret));
            return ret;
        }
        
        // 检查响应数据是否有效
        if (!global_at_response.is_valid) {
            ESP_LOGE(TAG, "AT指令响应数据无效");
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        ESP_LOGI(TAG, "AT指令响应已完成，响应数据长度: %zu", global_at_response.response_len);
    }
    
    return ESP_OK;
}

esp_err_t uart_get_at_command_response(char *response, size_t response_len)
{
    if (response == NULL || response_len == 0) {
        ESP_LOGE(TAG, "参数无效");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 清空响应缓冲区
    memset(response, 0, response_len);
    
    // 检查响应数据是否有效
    if (!global_at_response.is_valid) {
        ESP_LOGW(TAG, "AT指令响应数据无效");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 复制响应数据
    size_t copy_len = (global_at_response.response_len < response_len - 1) ? 
                      global_at_response.response_len : response_len - 1;
    memcpy(response, global_at_response.response, copy_len);
    response[copy_len] = '\0';
    
    ESP_LOGI(TAG, "获取AT指令响应数据成功: %s", response);
    
    return ESP_OK;
}

esp_err_t uart_wait_for_at_response(uint32_t timeout_ms)
{
    if (at_response_sem == NULL) {
        ESP_LOGE(TAG, "AT指令响应信号量未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 等待响应完成信号量
    if (xSemaphoreTake(at_response_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ESP_LOGI(TAG, "AT指令响应已完成");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "等待AT指令响应超时");
        return ESP_ERR_TIMEOUT;
    }
}