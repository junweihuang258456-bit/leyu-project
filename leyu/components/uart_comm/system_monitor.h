/**
 * 系统监控模块
 * 用于监控系统内存和电源状态
 */

#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统监控模块
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t system_monitor_init(void);

/**
 * @brief 启动系统监控任务
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t system_monitor_task_start(void);

/**
 * @brief 停止系统监控任务
 * 
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t system_monitor_task_stop(void);

/**
 * @brief 获取当前内存使用情况
 * 
 * @param free_heap 存储空闲堆内存大小
 * @param min_free_heap 存储历史最小空闲堆内存大小
 * @return esp_err_t ESP_OK表示成功，其他值表示失败
 */
esp_err_t system_monitor_get_memory_info(uint32_t *free_heap, uint32_t *min_free_heap);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_MONITOR_H