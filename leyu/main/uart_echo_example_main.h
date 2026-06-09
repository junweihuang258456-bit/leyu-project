/**
 * UART通信模块头文件
 * 
 * 本头文件提供UART通信模块的函数声明，方便其他模块调用
 */

#ifndef UART_ECHO_EXAMPLE_MAIN_H
#define UART_ECHO_EXAMPLE_MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化UART通信模块
 * 
 * 本函数用于初始化UART通信模块，创建UART通信任务
 * 配置UART参数，启动与4G模组的通信
 * 
 * @return 无
 */
void uart_app_init(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_ECHO_EXAMPLE_MAIN_H */