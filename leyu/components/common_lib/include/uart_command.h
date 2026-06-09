/**
 * @file uart_command.h
 * @brief 串口指令组件头文件
 * 
 * 该组件提供串口指令的接收、解析和处理功能，支持自定义指令集
 * 使用内置RAM启动任务
 * 
 * @author ESP32开发团队
 * @date 2024
 */

#ifndef UART_COMMAND_H
#define UART_COMMAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 最大指令长度
 */
#define MAX_COMMAND_LENGTH 256

/**
 * @brief 最大参数长度
 */
#define MAX_PARAM_LENGTH 128

/**
 * @brief 最大SN号长度
 */
#define MAX_SN_LENGTH 128

/**
 * @brief 指令处理函数类型定义
 * 
 * @param param 指令参数
 * @param param_len 参数长度
 * @return esp_err_t 处理结果
 */
typedef esp_err_t (*command_handler_t)(const char *param, size_t param_len);

/**
 * @brief 指令结构体
 */
typedef struct {
    char command_name[MAX_COMMAND_LENGTH];  ///< 指令名称
    command_handler_t handler;               ///< 指令处理函数
    const char *description;                ///< 指令描述
} command_entry_t;

/**
 * @brief 串口指令组件初始化函数
 * 
 * 该函数初始化串口指令处理模块，使用PSRAM启动任务
 * 对外暴露的主要接口函数
 * 
 * @return esp_err_t 初始化结果
 */
esp_err_t uart_command_app_init(void);

/**
 * @brief 注册指令处理函数
 * 
 * @param command_name 指令名称
 * @param handler 处理函数
 * @param description 指令描述
 * @return esp_err_t 注册结果
 */
esp_err_t uart_command_register(const char *command_name, command_handler_t handler, const char *description);

/**
 * @brief 发送响应到串口
 * 
 * @param response 响应内容
 * @return esp_err_t 发送结果
 */
esp_err_t uart_command_send_response(const char *response);

/**
 * @brief 读取SN号
 * 
 * 该函数用于从NVS中读取SN号，设计为可扩展的接口
 * 方便后期修改内部取SN号的逻辑
 * 
 * @param sn_buffer 存储SN号的缓冲区
 * @param buffer_size 缓冲区大小
 * @return esp_err_t 读取结果
 */
esp_err_t read_sn_from_nvs(char *sn_buffer, size_t buffer_size);

/**
 * @brief 写入SN号到NVS
 * 
 * @param sn 要写入的SN号
 * @return esp_err_t 写入结果
 */
esp_err_t write_sn_to_nvs(const char *sn);

/**
 * @brief 从NVS中清除SN号
 * 
 * @return esp_err_t 清除结果
 */
esp_err_t clear_sn_from_nvs(void);

#ifdef __cplusplus
}
#endif

#endif // UART_COMMAND_H
