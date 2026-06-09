/**
 * 网络管理器 - 处理WiFi和4G模块之间的切换
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// 网络状态结构体
typedef struct {
    bool is_4g_active;          // 4G网络是否活动
    bool is_wifi_active;         // WiFi网络是否活动
    bool auto_switch_enabled;     // 自动切换是否启用
    int auto_switch_state;       // 自动切换状态：0=初始，1=已切换到4G，2=已切换回WiFi
} network_status_t;

// 获取当前活动的网络接口
esp_netif_t *get_active_netif(void);

// 切换到4G网络
esp_err_t switch_to_4g_network(void);

// 切换到WiFi网络
esp_err_t switch_to_wifi_network(void);

// 启动自动切换（30秒后切换到4G，再30秒后切换回WiFi）
esp_err_t start_auto_switch(void);

// 停止自动切换
esp_err_t stop_auto_switch(void);

// 获取当前网络状态
network_status_t get_network_status(void);

// 初始化网络管理器
esp_err_t network_manager_init(void);

// 配置WiFi AP路由，使连接到AP的设备可以通过4G网络访问互联网
esp_err_t configure_ap_routing(esp_netif_t *ap_netif, esp_netif_t *uplink_netif);

// 测试网络连通性
esp_err_t test_network_connectivity(const char *network_name);

// 测试指定网络接口的连通性
esp_err_t test_network_connectivity_with_netif(const char *network_name, esp_netif_t *netif);

// 等待NTP时间同步完成
bool wait_for_ntp_sync(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_MANAGER_H