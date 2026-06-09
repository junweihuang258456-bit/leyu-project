/* UART 4G模组通信测试程序

   本程序用于ESP32-S3通过UART与4G模组通信
   - 发送AT指令测试4G模组
   - 接收并显示4G模组响应
   - 默认使用UART2，GPIO13(TXD), GPIO14(RXD)
   - 波特率：115200
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

/**
 * UART 4G模组通信测试程序
 * 
 * 功能：
 * 1. 自动发送AT指令到4G模组
 * 2. 接收并显示4G模组的响应
 * 3. 每5秒自动发送一次AT指令
 * 
 * 硬件连接：
 * - ESP32-S3 GPIO13(TXD) -> 4G模组RXD
 * - ESP32-S3 GPIO14(RXD) <- 4G模组TXD
 * - GND连接
 * 
 * 配置参数：
 * - UART端口：UART2
 * - 波特率：115200（可在menuconfig中修改）
 * - 数据位：8位
 * - 停止位：1位
 * - 无校验位
 * - 无硬件流控
 */

#define ECHO_TEST_TXD (13)  // 使用GPIO13作为TXD
#define ECHO_TEST_RXD (14)  // 使用GPIO14作为RXD
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (2)
#define ECHO_UART_BAUD_RATE     (115200)
#define ECHO_TASK_STACK_SIZE    (3072)

static const char *TAG = "UART TEST";

#define BUF_SIZE (600)

static void echo_task(void *arg)
{
    // 检查UART是否已经初始化
    esp_err_t uart_status = uart_is_driver_installed(ECHO_UART_PORT_NUM);
    
    if (uart_status != ESP_OK) {
        /* Configure parameters of an UART driver,
         * communication pins and install the driver */
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

    #if CONFIG_UART_ISR_IN_IRAM
        intr_alloc_flags = ESP_INTR_FLAG_IRAM;
    #endif

        ESP_LOGI(TAG, "UART%d 配置: TXD=GPIO%d, RXD=GPIO%d, 波特率=%d", 
                 ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_UART_BAUD_RATE);
        ESP_LOGI(TAG, "硬件连接确认: GPIO13(TXD) <-> GPIO14(RXD) 用于回环测试");
        ESP_LOGI(TAG, "ESP32-S3支持GPIO矩阵映射，任意GPIO都可以配置为UART功能");

        // 配置GPIO引脚模式为UART功能
        gpio_set_direction(ECHO_TEST_TXD, GPIO_MODE_OUTPUT);
        gpio_set_direction(ECHO_TEST_RXD, GPIO_MODE_INPUT);
        gpio_set_pull_mode(ECHO_TEST_RXD, GPIO_PULLUP_ONLY);  // 启用上拉电阻
        
        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));
    } else {
        ESP_LOGI(TAG, "UART%d 已初始化，跳过配置步骤", ECHO_UART_PORT_NUM);
    }
    
    ESP_LOGI(TAG, "4G模组通信测试已启动");
    ESP_LOGI(TAG, "连接状态: GPIO13(TXD) -> 4G模组RXD, GPIO14(RXD) <- 4G模组TXD");
    
    // 发送AT指令测试4G模组
    const char *at_command = "AT\r\n";
    uart_write_bytes(ECHO_UART_PORT_NUM, at_command, strlen(at_command));
    ESP_LOGI(TAG, "已发送AT指令: %s", at_command);
    
    int at_count = 1;  // AT指令计数器

    // 使用栈分配的缓冲区，避免内存泄漏
    uint8_t data[BUF_SIZE];

    while (1) {
        // Read data from the UART - 接收4G模组的响应
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 500 / portTICK_PERIOD_MS);
        if (len > 0) {
            // 只打印接收到的数据，不再回发
            data[len] = '\0';
            ESP_LOGI(TAG, "4G模组响应: %s", (char *) data);
        } else if (len < 0) {
            ESP_LOGE(TAG, "UART读取错误: %d", len);
        } else {
            // 超时情况，打印调试信息（调试用，可注释掉）
            ESP_LOGD(TAG, "等待4G模组响应...");
            
            // 每20秒发送一次AT指令（40次超时后）
            static int timeout_count = 0;
            timeout_count++;
            if (timeout_count >= 40) {
                timeout_count = 0;
                at_count++;
                
                // 检查可用内存
                size_t free_heap = esp_get_free_heap_size();
                ESP_LOGI(TAG, "=== 第%d次发送AT指令，当前可用内存: %" PRIu32 " 字节 ===", 
                         at_count, (uint32_t)free_heap);
                
                // 如果内存过低，记录警告
                if (free_heap < 10000) {  // 如果可用内存小于10KB
                    ESP_LOGW(TAG, "警告：可用内存过低！仅剩 %" PRIu32 " 字节", (uint32_t)free_heap);
                }
                
                uart_write_bytes(ECHO_UART_PORT_NUM, at_command, strlen(at_command));
            }
        }
    }
}

void uart_app_init(void)
{
    xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 10, NULL);
    
}
