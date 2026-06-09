/* OTA 静态库头文件
 * 对外暴露 ota_app_init 和 ota_start 函数
 * 基于ESP-IDF实现OTA升级功能
 */

#ifndef OTA_H
#define OTA_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化OTA库
 * 创建OTA任务，初始化必要资源
 * 需要在调用ota_start之前调用
 * 
 * @return 无返回值
 */
void ota_app_init(void);

/**
 * @brief 开始OTA升级
 * 触发OTA任务执行固件升级
 * 需要先调用ota_app_init初始化OTA库
 * 
 * @param url 固件升级包的URL地址
 * @return esp_err_t 返回ESP_OK表示成功，其他值表示失败
 */
esp_err_t ota_start(const char *url);

#ifdef __cplusplus
}
#endif

#endif // OTA_H