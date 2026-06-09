/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef USB_RNDIS_4G_MODULE_H
#define USB_RNDIS_4G_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_netif.h"

/**
 * @brief 初始化USB RNDIS 4G模块
 * 
 * 该函数负责初始化USB RNDIS 4G模块，包括：
 * 1. 初始化TCP/IP协议栈
 * 2. 创建事件组
 * 3. 安装USBH CDC驱动
 * 4. 创建USB RNDIS驱动
 * 5. 安装以太网驱动
 * 6. 创建网络接口
 * 7. 启动网络连接
 * 8. 等待获取IP地址
 * 9. 启动WiFi应用
 * 10. 如果启用了AT命令，执行4G拨号流程
 */
void usb_module_app_init(void);

/**
 * @brief 互联网连接状态标志
 * 
 * 该变量用于指示4G模块是否已成功连接到互联网
 * 0: 未连接
 * 1: 已连接
 */
extern uint8_t internet_connected;

/**
 * @brief 获取4G模块的网络接口
 * 
 * @return esp_netif_t* 返回4G模块的网络接口指针，如果未初始化则返回NULL
 */
esp_netif_t *get_usb_netif(void);

/**
 * @brief 释放AT指令缓冲区内存
 */
void free_at_buffers(void);

/**
 * @brief 硬件复位4G模块
 */
void reset_4g_module_hardware(void);

#ifdef __cplusplus
}
#endif

#endif // USB_RNDIS_4G_MODULE_H