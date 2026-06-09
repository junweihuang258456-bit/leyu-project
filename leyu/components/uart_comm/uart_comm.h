/**
 * UART通信模块
 * 用于与4G模组或其他串口设备通信
 */

#ifndef UART_COMM_H
#define UART_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_COMM_PORT_NUM      2       // 使用UART2
#define UART_COMM_BAUD_RATE     115200  // 波特率
#define UART_COMM_TXD           13      // TXD引脚
#define UART_COMM_RXD           14      // RXD引脚
#define UART_COMM_BUF_SIZE      1024    // 缓冲区大小
#define UART_COMM_TASK_STACK    6144    // 任务堆栈大小
#define UART_COMM_TIMEOUT_MS    500     // 超时时间(毫秒)

/**
 * @brief 初始化UART通信模块
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_comm_init(void);

/**
 * @brief 发送AT命令并等待响应
 * 
 * @param command 要发送的AT命令
 * @param response 存储响应的缓冲区
 * @param response_len 响应缓冲区大小
 * @param timeout_ms 等待响应的超时时间(毫秒)
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_send_and_wait(const char *command, char *response, size_t response_len, uint32_t timeout_ms);

/**
 * @brief 发送AT命令(简化版)
 * 
 * @param command 要发送的AT命令
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_send(const char *command);

/**
 * @brief 启动UART通信任务
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_comm_task_start(void);

/**
 * @brief 停止UART通信任务
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_comm_task_stop(void);

/**
 * @brief 全局AT指令结构体
 */
typedef struct {
    char command[256];    ///< AT指令字符串
    uint32_t timeout_ms;  ///< 超时时间(毫秒)
    bool need_response;    ///< 是否需要等待响应
} global_at_command_t;

/**
 * @brief AT指令响应数据结构体
 */
typedef struct {
    char response[1024];   ///< 响应数据缓冲区
    size_t response_len;   ///< 响应数据长度
    bool is_valid;         ///< 响应数据是否有效
    uint32_t timestamp;    ///< 响应时间戳
} at_command_response_t;

/**
 * @brief 发送全局AT指令
 * 
 * 该函数可以在任意任务中调用，通过全局变量和信号量触发任务发送AT指令
 * 
 * @param command 要发送的AT指令字符串
 * @param timeout_ms 等待响应的超时时间(毫秒)，0表示不等待响应
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_send_global_at_command(const char *command, uint32_t timeout_ms);

/**
 * @brief 获取最新的AT指令响应数据
 * 
 * @param response 存储响应数据的缓冲区
 * @param response_len 响应缓冲区大小
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t uart_get_at_command_response(char *response, size_t response_len);

/**
 * @brief 等待AT指令响应完成
 * 
 * 该函数会阻塞等待直到AT指令响应完成或超时
 * 
 * @param timeout_ms 最大等待时间(毫秒)
 * @return esp_err_t ESP_OK表示成功，ESP_ERR_TIMEOUT表示超时，其他值表示失败
 */
esp_err_t uart_wait_for_at_response(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // UART_COMM_H