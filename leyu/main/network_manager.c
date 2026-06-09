/**
 * 网络管理器 - 处理WiFi和4G模块之间的切换
 */

#include "network_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ping.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "usb_rndis_4g_module.h"
#include "app_wifi.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <time.h>

extern void brtc_force_reconnect(void);
extern void audio_prompt_play(const char *uri);

static const char *TAG = "network_manager";
static bool is_4g_active = false;
static bool is_wifi_active = false;
static TimerHandle_t auto_switch_timer = NULL;
static bool auto_switch_enabled = false;
static int auto_switch_state = 0; // 0: 初始状态, 1: 已切换到4G, 2: 已切换回WiFi
static bool ntp_initialized = false;
static bool ntp_sync_completed = false;
static EventGroupHandle_t ntp_event_group = NULL;
#define NTP_SYNC_COMPLETED_BIT BIT0

// NTP时间同步回调函数
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "时间同步成功");
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "当前时间: %s", strftime_buf);
    
    // 设置时间同步完成标志
    ntp_sync_completed = true;
    if (ntp_event_group) {
        xEventGroupSetBits(ntp_event_group, NTP_SYNC_COMPLETED_BIT);
    }
}

// 初始化NTP时间同步
static void initialize_ntp(void)
{
    if (ntp_initialized) {
        ESP_LOGI(TAG, "NTP已初始化，跳过");
        return;
    }
    
    ESP_LOGI(TAG, "初始化NTP时间同步...");
    
    // 创建NTP事件组
    if (!ntp_event_group) {
        ntp_event_group = xEventGroupCreate();
        if (!ntp_event_group) {
            ESP_LOGE(TAG, "创建NTP事件组失败");
            return;
        }
    }
    
    // 设置NTP服务器
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    
    // 设置时区为东八区
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // 注册时间同步回调
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    // 启动SNTP
    esp_sntp_init();
    
    ntp_initialized = true;
    ESP_LOGI(TAG, "NTP初始化完成，开始同步时间...");
}

// 等待NTP时间同步完成
bool wait_for_ntp_sync(uint32_t timeout_ms)
{
    // 如果NTP事件组未创建，说明NTP未初始化，直接返回成功
    if (!ntp_event_group) {
        ESP_LOGW(TAG, "NTP事件组未创建，NTP可能未初始化，跳过等待");
        return true;
    }
    
    if (ntp_sync_completed) {
        ESP_LOGI(TAG, "NTP时间同步已完成");
        return true;
    }
    
    ESP_LOGI(TAG, "等待NTP时间同步完成，超时时间: %u ms", timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        ntp_event_group,
        NTP_SYNC_COMPLETED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    
    if (bits & NTP_SYNC_COMPLETED_BIT) {
        ESP_LOGI(TAG, "NTP时间同步成功完成");
        return true;
    } else {
        ESP_LOGW(TAG, "NTP时间同步超时");
        return false;
    }
}

// 获取当前活动的网络接口
esp_netif_t *get_active_netif(void)
{
    esp_netif_t *netif = NULL;
    
    if (is_4g_active) {
        netif = get_usb_netif();
        ESP_LOGD(TAG, "获取4G网络接口: %p", netif);
    } else if (is_wifi_active) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        ESP_LOGD(TAG, "获取WiFi STA网络接口: %p", netif);
    }
    
    return netif;
}

// 智能网络切换监控任务
static void network_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "网络智能监控任务已启动，将自动切换最优网络...");
    static int wifi_fail_count = 0;
    static int wifi_recovery_count = 0;
    static int wifi_cooldown_counter = 0; // 新增：WiFi冷却期计数器
    
    while (1) {
        // 强制执行路由表对齐：确保 LWIP 的默认网卡（Default Netif）始终与当前的 active_netif 一致，
        // 防止 4G 模块 RNDIS 获取 IP 时抢占默认路由。
        esp_netif_t *current_active = get_active_netif();
        if (current_active) {
            esp_netif_set_default_netif(current_active);
        }

        // 每 10 秒检查一次，避免频繁探测导致系统资源占用过高或 LwIP 崩溃
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        if (wifi_cooldown_counter > 0) {
            wifi_cooldown_counter--;
        }
        
        // 只有当当前使用的是 4G 网络时，才去检查 WiFi 是否恢复
        if (is_4g_active && !is_wifi_active) {
            extern uint8_t internet_connected;

            // 如果 4G 网络是通的，执行严格的冷却防抖；但如果 4G 也是断的（设备处于彻底断网的绝境），则无视冷却期，随时准备抢救WiFi
            if (wifi_cooldown_counter > 0 && internet_connected == 1) {
                ESP_LOGD(TAG, "[4G] WiFi正在冷却期(剩余%d秒)，暂不尝试切回WiFi", wifi_cooldown_counter * 10);
                continue;
            }

            esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (wifi_netif) {
                // 判断 WiFi 是否连接上路由器
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                    ESP_LOGD(TAG, "[4G] 当前使用4G路由，检测到WiFi已连接路由器，测试WiFi外网...");
                    // 测试 WiFi 的外网连通性
                    if (test_network_connectivity_with_netif("[WiFi]", wifi_netif) == ESP_OK) {
                        wifi_recovery_count++;
                        ESP_LOGI(TAG, "WiFi外网连通测试成功 (%d)", wifi_recovery_count);
                        
                        // 急救模式：如果4G也是断的，只要WiFi通1次就立刻切回；如果4G正常，则需要连续稳3次
                        int required_recovery = (internet_connected == 1) ? 3 : 1;
                        
                        if (wifi_recovery_count >= required_recovery) {
                            ESP_LOGI(TAG, "满足WiFi恢复条件！自动切换回WiFi路由");
                            audio_prompt_play("file:///spiffs/networkchange.mp3"); // 播放网络切换提示
                            switch_to_wifi_network();
                            wifi_recovery_count = 0;
                            wifi_fail_count = 0;
                            wifi_cooldown_counter = 0; // 切回WiFi后，提前清空冷却器
                        }
                    } else {
                        wifi_recovery_count = 0;
                        ESP_LOGD(TAG, "WiFi仍无外网，保持4G路由");
                    }
                } else {
                    wifi_recovery_count = 0;
                }
            }
        } 
        // 当当前使用的是 WiFi 时，监控它是否断网
        else if (is_wifi_active && !is_4g_active) {
            esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            bool wifi_lost = false;
            
            if (!wifi_netif) {
                wifi_lost = true;
                ESP_LOGW(TAG, "未找到WiFi网络接口");
            } else {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(wifi_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
                    wifi_lost = true;
                    ESP_LOGW(TAG, "WiFi已断开连接或失去IP地址");
                } else {
                    // WiFi 表面上连着路由器，测试外网连通性
                    ESP_LOGD(TAG, "[WiFi] 当前使用WiFi路由，测试其外网连通性...");
                    if (test_network_connectivity_with_netif("[WiFi]", wifi_netif) != ESP_OK) {
                        wifi_lost = true;
                        ESP_LOGW(TAG, "WiFi虽然连着路由器，但是无法访问外网");
                    }
                }
            }
            
            if (wifi_lost) {
                wifi_fail_count++;
                ESP_LOGW(TAG, "检测到WiFi异常，累计失败次数: %d", wifi_fail_count);
                if (wifi_fail_count >= 1) {
                    extern uint8_t internet_connected;
                    if (internet_connected == 1) {
                        ESP_LOGW(TAG, "[WiFi] 极速检测失败，紧急切换到4G路由兜底！");
                        audio_prompt_play("file:///spiffs/networkchange.mp3"); // 播放网络切换提示
                    } else {
                        ESP_LOGW(TAG, "[WiFi] 极速检测失败，且4G未就绪，网络已彻底断开！");
                        audio_prompt_play("file:///spiffs/wifierror.mp3"); // 播放网络断开提示
                    }
                    switch_to_4g_network();
                    wifi_fail_count = 0;
                    wifi_recovery_count = 0;
                    wifi_cooldown_counter = 30; // 设置 30 * 10s = 5 分钟的WiFi冷却期
                    ESP_LOGI(TAG, "由于WiFi不稳定，已强制设置5分钟的WiFi冷却期，期间不尝试切回WiFi。");
                }
            } else {
                wifi_fail_count = 0; // 只要有一次成功就清零
            }
        }
    }
}

// 切换到4G网络
esp_err_t switch_to_4g_network(void)
{
    ESP_LOGI(TAG, "正在切换到4G网络...");
    
    // 检查是否已经是4G网络
    if (is_4g_active && !is_wifi_active) {
        ESP_LOGW(TAG, "已经是4G网络，跳过切换");
        return ESP_OK;
    }
    
    // 检查4G模块是否已初始化
    esp_netif_t *usb_netif = get_usb_netif();
    if (!usb_netif) {
        ESP_LOGE(TAG, "4G模块未初始化，无法切换");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 设置4G网络为默认路由
    esp_err_t ret = esp_netif_set_default_netif(usb_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置4G为默认网络失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    is_4g_active = true;
    is_wifi_active = false;
    
    // 网络切换成功，初始化NTP时间同步
    // initialize_ntp();
    
    // 配置AP路由，使连接到AP的设备可以通过4G网络访问互联网
    esp_netif_t *ap_netif = NULL;
    
    // 尝试多次获取AP网络接口，因为AP可能还在初始化过程中
    int retry_count = 0;
    const int max_retries = 5;
    
    while (retry_count < max_retries && !ap_netif) {
        ESP_LOGI(TAG, "尝试获取AP网络接口，第 %d 次", retry_count + 1);
        ap_netif = app_wifi_get_ap_netif();
        
        if (!ap_netif) {
            // 如果直接获取失败，尝试通过接口键获取
            ESP_LOGI(TAG, "直接获取失败，尝试通过接口键获取AP网络接口");
            ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            
            if (!ap_netif) {
                // 如果还是失败，等待一段时间后重试
                ESP_LOGW(TAG, "未找到AP网络接口，等待后重试");
                vTaskDelay(pdMS_TO_TICKS(500));
                retry_count++;
            }
        }
    }
    
    if (ap_netif) {
        ESP_LOGI(TAG, "成功获取AP网络接口");
        ret = configure_ap_routing(ap_netif, usb_netif);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "配置AP路由失败: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "已配置AP路由通过4G网络访问互联网");
        }
    } else {
        ESP_LOGW(TAG, "经过 %d 次尝试后仍未找到AP网络接口，跳过AP路由配置", max_retries);
    }
    
    ESP_LOGI(TAG, "[4G] 已成功切换到4G网络");
    
    // 强制重新建连
    brtc_force_reconnect();
    
    // 测试4G网络连通性
    ESP_LOGI(TAG, "等待3秒让4G网络稳定...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 确保我们使用的是4G接口进行测试
    esp_err_t test_result = test_network_connectivity_with_netif("[4G]", usb_netif);
    if (test_result == ESP_OK) {
        ESP_LOGI(TAG, "4G网络连通性测试通过");
    } else {
        ESP_LOGW(TAG, "4G网络连通性测试失败");
    }
    
    return ESP_OK;
}

// 测试指定网络接口的连通性


#include <fcntl.h>
#include <sys/select.h>
#include <lwip/sockets.h>
#include <sys/ioctl.h>

static int connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_sec) {
    int non_blocking = 1;
    if (ioctl(sock, FIONBIO, &non_blocking) < 0) return -1;

    int res = connect(sock, addr, addrlen);
    if (res < 0 && errno != EINPROGRESS) {
        non_blocking = 0;
        ioctl(sock, FIONBIO, &non_blocking);
        return -1;
    }

    if (res == 0) {
        non_blocking = 0;
        ioctl(sock, FIONBIO, &non_blocking);
        return 0;
    }

    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    res = select(sock + 1, NULL, &fdset, NULL, &tv);
    if (res <= 0) {
        non_blocking = 0;
        ioctl(sock, FIONBIO, &non_blocking);
        return -1;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    
    non_blocking = 0;
    ioctl(sock, FIONBIO, &non_blocking);
    
    if (so_error == 0) {
        return 0;
    }
    return -1;
}

esp_err_t test_network_connectivity_with_netif(const char *network_name, esp_netif_t *netif)
{
    ESP_LOGI(TAG, "开始测试 %s 网络接口连通性", network_name);
    
    if (!netif) {
        ESP_LOGE(TAG, "网络接口参数为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取网络接口IP信息
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取网络接口IP信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "%s 网络接口IP: " IPSTR, network_name, IP2STR(&ip_info.ip));
    
    // 测试1: 检查是否有有效IP地址
    if (ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "%s 网络接口没有有效IP地址", network_name);
        return ESP_FAIL;
    }
    
    // 测试2: 尝试创建一个TCP套接字连接到公共DNS服务器
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "%s 创建套接字失败", network_name);
        return ESP_FAIL;
    }
    
    // 设置超时
    struct timeval timeout;
    timeout.tv_sec = 5;  // 5秒超时
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // 绑定到指定的网络接口
    char ifname[10] = {0};
    if (esp_netif_get_netif_impl_name(netif, ifname) == ESP_OK) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
        if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            ESP_LOGE(TAG, "%s 绑定套接字到网络接口 %s 失败，errno: %d", network_name, ifname, errno);
            close(sock);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "已强制将测试套接字绑定到接口: %s", ifname);
    } else {
        ESP_LOGW(TAG, "无法获取 %s 的底层网络接口名，测试可能走默认路由", network_name);
    }
    
    // 连接到Ali DNS服务器 (223.5.5.5:443) 防止被 Captive Portal 劫持端口53和80
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("223.5.5.5");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(443);
    
    ESP_LOGI(TAG, "正在尝试通过 %s 接口连接到 223.5.5.5:443...", network_name);
    int err = connect_with_timeout(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr), 2);
    
    // 关闭套接字
    close(sock);
    
    if (err == 0) {
        ESP_LOGI(TAG, "%s 网络接口连通性测试成功 (223.5.5.5:443)", network_name);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "223.5.5.5:443 连通失败，尝试备用探活地址 (百度 220.181.38.148:443)...");
        
        // 重新创建套接字用于备用探活
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return ESP_FAIL;
        
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        if (ifname[0] != 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
            setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
        }
        
        dest_addr.sin_addr.s_addr = inet_addr("220.181.38.148");
        dest_addr.sin_port = htons(443);
        
        err = connect_with_timeout(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr), 2);
        close(sock);
        
        if (err == 0) {
            ESP_LOGI(TAG, "%s 网络接口连通性测试成功 (HTTP 220.181.38.148:443)", network_name);
            return ESP_OK;
        }
        
        ESP_LOGE(TAG, "%s 网络接口连通性测试最终失败", network_name);
        return ESP_FAIL;
    }
}

// 配置WiFi AP路由，使连接到AP的设备可以通过4G网络访问互联网
esp_err_t configure_ap_routing(esp_netif_t *ap_netif, esp_netif_t *uplink_netif)
{
    if (!ap_netif || !uplink_netif) {
        ESP_LOGE(TAG, "网络接口参数无效");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "配置AP路由，使连接设备可通过上行链路访问互联网");
    
    // 获取上行链路IP地址
    esp_netif_ip_info_t uplink_ip_info;
    esp_err_t ret = esp_netif_get_ip_info(uplink_netif, &uplink_ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取上行链路IP信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 获取AP网络接口IP信息
    esp_netif_ip_info_t ap_ip_info;
    ret = esp_netif_get_ip_info(ap_netif, &ap_ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取AP IP信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "AP网关: " IPSTR ", 上行链路IP: " IPSTR, 
             IP2STR(&ap_ip_info.ip), IP2STR(&uplink_ip_info.ip));
    
    // 停止DHCP服务器
    ret = esp_netif_dhcps_stop(ap_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "停止DHCP服务器失败或已停止: %s", esp_err_to_name(ret));
    }
    
    // 配置AP的网关为上行链路IP地址
    ap_ip_info.gw = uplink_ip_info.ip;
    ret = esp_netif_set_ip_info(ap_netif, &ap_ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置AP IP信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置DNS服务器为上行链路IP地址
    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4 = uplink_ip_info.ip;
    dns_info.ip.type = IPADDR_TYPE_V4;
    ret = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置DNS服务器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置DHCP选项，提供正确的网关和DNS
    uint8_t opt_value;
    
    // 设置网关选项
    opt_value = ROUTER;
    ret = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &opt_value, sizeof(opt_value));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设置DHCP网关选项失败: %s", esp_err_to_name(ret));
    }
    
    // 设置DNS选项
    opt_value = DOMAIN_NAME_SERVER;
    ret = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &opt_value, sizeof(opt_value));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设置DHCP DNS选项失败: %s", esp_err_to_name(ret));
    }
    
    // 重启DHCP服务器
    ret = esp_netif_dhcps_start(ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动DHCP服务器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "AP路由配置完成，网关和DNS已设置为上行链路IP: " IPSTR, IP2STR(&uplink_ip_info.ip));
    return ESP_OK;
}

// 切换到WiFi网络
esp_err_t switch_to_wifi_network(void)
{
    ESP_LOGI(TAG, "正在切换到WiFi网络...");
    
    // 检查是否已经是WiFi网络
    if (is_wifi_active && !is_4g_active) {
        ESP_LOGW(TAG, "已经是WiFi网络，跳过切换");
        return ESP_OK;
    }
    
    // 检查WiFi是否已初始化
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK || current_mode == WIFI_MODE_NULL) {
        ESP_LOGE(TAG, "WiFi未初始化，无法切换");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 直接通过键名获取WiFi网络接口，优先使用STA接口
    esp_netif_t *wifi_netif = NULL;
    
    // 首先尝试获取STA接口
    if (current_mode & WIFI_MODE_STA) {
        wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (wifi_netif) {
            ESP_LOGI(TAG, "通过键名获取到STA网络接口");
        } else {
            ESP_LOGW(TAG, "STA模式已启用但无法找到STA网络接口");
        }
    }
    
    // 如果STA接口不存在，尝试获取AP接口
    if (!wifi_netif && (current_mode & WIFI_MODE_AP)) {
        wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (wifi_netif) {
            ESP_LOGI(TAG, "通过键名获取到AP网络接口");
        } else {
            ESP_LOGW(TAG, "AP模式已启用但无法找到AP网络接口");
        }
    }
    
    // 如果仍然没有获取到网络接口，尝试通过app_wifi模块获取
    if (!wifi_netif) {
        ESP_LOGI(TAG, "尝试通过app_wifi模块获取网络接口");
        
        if (current_mode & WIFI_MODE_STA) {
            wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (wifi_netif) {
                ESP_LOGI(TAG, "通过app_wifi获取到STA网络接口");
            }
        }
        
        if (!wifi_netif && (current_mode & WIFI_MODE_AP)) {
            wifi_netif = app_wifi_get_ap_netif();
            if (wifi_netif) {
                ESP_LOGI(TAG, "通过app_wifi获取到AP网络接口");
            }
        }
    }
    
    // 最后的尝试：遍历所有网络接口，找到WiFi相关的接口
    if (!wifi_netif) {
        ESP_LOGI(TAG, "尝试遍历所有网络接口查找WiFi接口");
        esp_netif_t *netif = NULL;
        // 由于esp_netif_get_handle函数不存在，我们只能通过已知的键名查找
        // 尝试更多可能的WiFi接口键名
        const char *wifi_keys[] = {"WIFI_STA_DEF", "WIFI_AP_DEF", "WIFI_STA", "WIFI_AP"};
        for (int i = 0; i < sizeof(wifi_keys)/sizeof(wifi_keys[0]); i++) {
            netif = esp_netif_get_handle_from_ifkey(wifi_keys[i]);
            if (netif) {
                const char *desc = esp_netif_get_desc(netif);
                if (strstr(desc, "sta") != NULL) {
                    wifi_netif = netif;
                    ESP_LOGI(TAG, "找到STA网络接口: %s (键名: %s)", desc, wifi_keys[i]);
                    break;
                } else if (strstr(desc, "ap") != NULL) {
                    wifi_netif = netif;
                    ESP_LOGI(TAG, "找到AP网络接口: %s (键名: %s)", desc, wifi_keys[i]);
                    // 不break，优先选择STA接口
                }
            }
        }
    }
    
    if (!wifi_netif) {
        ESP_LOGE(TAG, "WiFi网络接口不存在，无法切换。当前WiFi模式: %d", current_mode);
        // 打印所有可用的网络接口键名，用于调试
        ESP_LOGI(TAG, "当前可用的网络接口:");
        // 尝试常见的网络接口键名
        const char *common_keys[] = {"WIFI_STA_DEF", "WIFI_AP_DEF", "WIFI_STA", "WIFI_AP", "ETH_DEF", "USB_DEF"};
        for (int i = 0; i < sizeof(common_keys)/sizeof(common_keys[0]); i++) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey(common_keys[i]);
            if (netif) {
                const char *desc = esp_netif_get_desc(netif);
                const char *key = esp_netif_get_ifkey(netif);
                ESP_LOGI(TAG, "  接口: %s (键名: %s)", desc, key);
            }
        }
        return ESP_ERR_INVALID_STATE;
    }
    
    // 设置WiFi网络为默认路由
    ret = esp_netif_set_default_netif(wifi_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置WiFi为默认网络失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    is_wifi_active = true;
    is_4g_active = false;
    
    // WiFi网络不初始化NTP，避免时间变化导致百度RTC断连
    // initialize_ntp();
    
    // 配置AP路由，使连接到AP的设备可以通过WiFi网络访问互联网
    esp_netif_t *ap_netif = NULL;
    
    // 尝试多次获取AP网络接口，因为AP可能还在初始化过程中
    int retry_count = 0;
    const int max_retries = 5;
    
    while (retry_count < max_retries && !ap_netif) {
        ESP_LOGI(TAG, "尝试获取AP网络接口，第 %d 次", retry_count + 1);
        ap_netif = app_wifi_get_ap_netif();
        
        if (!ap_netif) {
            // 如果直接获取失败，尝试通过接口键获取
            ESP_LOGI(TAG, "直接获取失败，尝试通过接口键获取AP网络接口");
            ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            
            if (!ap_netif) {
                // 如果还是失败，等待一段时间后重试
                ESP_LOGW(TAG, "未找到AP网络接口，等待后重试");
                vTaskDelay(pdMS_TO_TICKS(500));
                retry_count++;
            }
        }
    }
    
    if (ap_netif) {
        ESP_LOGI(TAG, "成功获取AP网络接口");
        // 当WiFi是上行链路时，使用STA接口作为网关
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            ret = configure_ap_routing(ap_netif, sta_netif);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "配置AP路由失败: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "已配置AP路由通过WiFi网络访问互联网");
            }
        } else {
            ESP_LOGW(TAG, "未找到STA网络接口，无法配置AP路由");
        }
    } else {
        ESP_LOGW(TAG, "经过 %d 次尝试后仍未找到AP网络接口，跳过AP路由配置", max_retries);
    }
    
    ESP_LOGI(TAG, "[WiFi] 已成功切换到WiFi网络");
    
    // 强制断开当前百度引擎，使其立即从新的WiFi路由重新建连，避免死等超时
    brtc_force_reconnect();
    
    // 测试WiFi网络连通性
    ESP_LOGI(TAG, "等待5秒让WiFi网络稳定...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 确保我们使用的是WiFi接口进行测试
    esp_err_t test_result = test_network_connectivity_with_netif("[WiFi]", wifi_netif);
    if (test_result == ESP_OK) {
        ESP_LOGI(TAG, "WiFi网络连通性测试通过");
    } else {
        ESP_LOGW(TAG, "WiFi网络连通性测试失败");
    }
    
    return ESP_OK;
}

// 获取当前网络状态
network_status_t get_network_status(void)
{
    network_status_t status = {
        .is_4g_active = is_4g_active,
        .is_wifi_active = is_wifi_active,
        .auto_switch_enabled = auto_switch_enabled,
        .auto_switch_state = auto_switch_state
    };
    
    return status;
}

// 初始化网络管理器
esp_err_t network_manager_init(void)
{
    ESP_LOGI(TAG, "初始化网络管理器");
    
    extern bool check_wifi_config_saved(void);
    extern uint8_t internet_connected;
    
    // 检查初始网络状态
    if (internet_connected == 1 && !check_wifi_config_saved()) {
        ESP_LOGI(TAG, "检测到设备未配网但4G已连接，默认使用4G网络");
        is_4g_active = true;
        is_wifi_active = false;
        
        // 静默将默认路由设为4G
        esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (eth_netif) {
            esp_netif_set_default_netif(eth_netif);
        }
    } else {
        // 检测WiFi的实际状态
        wifi_mode_t current_mode;
        esp_err_t ret = esp_wifi_get_mode(&current_mode);
        if (ret == ESP_OK && current_mode != WIFI_MODE_NULL) {
            is_wifi_active = true;
            ESP_LOGI(TAG, "检测到WiFi已初始化，模式: %d", current_mode);
        } else {
            is_wifi_active = false;
            ESP_LOGI(TAG, "WiFi未初始化");
        }
        
        // 4G模块初始状态为未激活
        is_4g_active = false;
    }
    
    auto_switch_enabled = false;
    auto_switch_state = 0;
    
    // 尝试初始化NTP时间同步
    initialize_ntp();
    
    // 启动智能网络切换监控任务
    if (xTaskCreate(network_monitor_task, "network_monitor_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建网络智能监控任务失败");
    } else {
        ESP_LOGI(TAG, "网络智能监控任务创建成功");
    }
    
    return ESP_OK;
}

// 测试网络连通性
esp_err_t test_network_connectivity(const char *network_name)
{
    ESP_LOGI(TAG, "开始测试 %s 网络连通性", network_name);
    
    // 打印当前网络状态
    ESP_LOGI(TAG, "当前网络状态 - 4G: %s, WiFi: %s", 
             is_4g_active ? "活动" : "非活动", 
             is_wifi_active ? "活动" : "非活动");
    
    // 获取当前活动的网络接口
    esp_netif_t *active_netif = get_active_netif();
    if (!active_netif) {
        ESP_LOGE(TAG, "无法获取活动网络接口");
        
        // 添加更多调试信息
        ESP_LOGI(TAG, "尝试直接获取WiFi STA接口...");
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            ESP_LOGI(TAG, "直接获取WiFi STA接口成功: %p", sta_netif);
            active_netif = sta_netif;
        } else {
            ESP_LOGI(TAG, "直接获取WiFi STA接口失败");
            
            ESP_LOGI(TAG, "尝试直接获取4G USB接口...");
            esp_netif_t *usb_netif = get_usb_netif();
            if (usb_netif) {
                ESP_LOGI(TAG, "直接获取4G USB接口成功: %p", usb_netif);
                active_netif = usb_netif;
            } else {
                ESP_LOGI(TAG, "直接获取4G USB接口失败");
            }
        }
        
        if (!active_netif) {
            ESP_LOGE(TAG, "所有尝试都失败，无法获取网络接口");
            return ESP_FAIL;
        }
    }
    
    // 获取网络接口IP信息
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(active_netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取网络接口IP信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "%s 网络接口IP: " IPSTR, network_name, IP2STR(&ip_info.ip));
    
    // 测试1: 检查是否有有效IP地址
    if (ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "%s 网络接口没有有效IP地址", network_name);
        return ESP_FAIL;
    }
    
    // 测试2: 尝试创建一个TCP套接字连接到公共DNS服务器
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "%s 创建套接字失败", network_name);
        return ESP_FAIL;
    }
    
    // 设置超时
    struct timeval timeout;
    timeout.tv_sec = 5;  // 5秒超时
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // 连接到Ali DNS服务器 (223.5.5.5:53)
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("223.5.5.5");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    ESP_LOGI(TAG, "正在尝试连接到 223.5.5.5:53...");
    int err = connect_with_timeout(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr), 2);
    
    // 关闭套接字
    close(sock);
    
    if (err == 0) {
        ESP_LOGI(TAG, "%s 网络连通性测试成功，已成功连接到 223.5.5.5:53", network_name);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "%s 网络连通性测试失败，无法连接到 223.5.5.5:53，错误: %d", network_name, errno);
        return ESP_FAIL;
    }
}