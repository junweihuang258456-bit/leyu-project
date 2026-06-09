/**
 * 系统监控模块实现
 * 用于监控系统内存状态
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "driver/gpio.h"  // 添加GPIO头文件
#include "mqtt_example.h" // 添加MQTT头文件
#include "system_monitor.h"

static const char *TAG = "system_monitor";

// 声明外部关机标志
extern int g_shutdown_request;

// 系统监控任务句柄
static TaskHandle_t system_monitor_task_handle = NULL;

// 系统监控任务
static void system_monitor_task(void *arg) {
  uint32_t last_report_time = 0;
  uint32_t last_min_free_heap = esp_get_minimum_free_heap_size();

  while (1) {
    uint32_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒

    // 每30秒报告一次系统状态
    if (current_time - last_report_time >= 30000) {
      last_report_time = current_time;

      // 获取内存信息
      uint32_t free_heap = esp_get_free_heap_size();
      uint32_t min_free_heap = esp_get_minimum_free_heap_size();

      // 检查内存是否持续减少
      if (min_free_heap < last_min_free_heap) {
        ESP_LOGW(TAG, "内存减少 最小值:%" PRIu32, min_free_heap);
        last_min_free_heap = min_free_heap;
      }
    }

    // 检查关机请求
    if (g_shutdown_request == 1) {
      ESP_LOGI(TAG, "检测到关机请求，发送关机心跳包");

      // 发送关机心跳包，status为"off"
      bool heartbeat_sent = send_heartbeat_packet("off");

      if (heartbeat_sent) {
        ESP_LOGI(TAG, "关机心跳包发送成功，延迟2秒后执行关机");
        vTaskDelay(pdMS_TO_TICKS(2000)); // 延迟2秒确保心跳包发送完成
      } else {
        ESP_LOGW(TAG, "关机心跳包发送失败，延迟5秒后执行关机");
        // vTaskDelay(pdMS_TO_TICKS(5000));  // 延迟5秒
      }

      ESP_LOGI(TAG, "执行关机操作");
      gpio_set_level(GPIO_NUM_48, 0); // 先关闭功放 PA，防止断电瞬间产生爆音
      vTaskDelay(pdMS_TO_TICKS(100)); // 稍作延时等待电容放电
      gpio_set_level(GPIO_NUM_1, 0);  // 电源键自锁释放，切断总电源
      g_shutdown_request = 0;         // 重置标志
    }

    // 定期检查任务删除请求
    if (system_monitor_task_handle == NULL) {
      break; // 如果任务句柄被清空，退出循环
    }

    // 每5秒检查一次
    vTaskDelay(pdMS_TO_TICKS(3900));
  }

  vTaskDelete(NULL);
}

esp_err_t system_monitor_init(void) {
  ESP_LOGD(TAG, "系统监控模块初始化成功");
  return ESP_OK;
}

esp_err_t system_monitor_task_start(void) {
  if (system_monitor_task_handle != NULL) {
    ESP_LOGW(TAG, "系统监控任务已启动");
    return ESP_ERR_INVALID_STATE;
  }

  // 检查可用内存
  size_t free_heap = esp_get_free_heap_size();
  ESP_LOGD(TAG, "启动前可用内存: %" PRIu32, (uint32_t)free_heap);

  if (free_heap < 1824 + 1024) { // 预留额外的1KB内存
    ESP_LOGE(TAG, "内存不足，无法启动系统监控任务");
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ret = xTaskCreate(system_monitor_task, "system_monitor_task", 3024,
                               NULL, 3, &system_monitor_task_handle);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "创建系统监控任务失败");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGD(TAG, "系统监控任务启动成功");
  return ESP_OK;
}

esp_err_t system_monitor_task_stop(void) {
  if (system_monitor_task_handle == NULL) {
    ESP_LOGW(TAG, "系统监控任务未启动");
    return ESP_ERR_INVALID_STATE;
  }

  // 先清空任务句柄，通知任务退出循环
  TaskHandle_t temp_handle = system_monitor_task_handle;
  system_monitor_task_handle = NULL;

  // 等待任务自然退出
  vTaskDelay(pdMS_TO_TICKS(100));

  // 删除任务
  vTaskDelete(temp_handle);

  ESP_LOGD(TAG, "系统监控任务已停止");
  return ESP_OK;
}

esp_err_t system_monitor_get_memory_info(uint32_t *free_heap,
                                         uint32_t *min_free_heap) {
  if (free_heap == NULL || min_free_heap == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *free_heap = esp_get_free_heap_size();
  *min_free_heap = esp_get_minimum_free_heap_size();

  return ESP_OK;
}