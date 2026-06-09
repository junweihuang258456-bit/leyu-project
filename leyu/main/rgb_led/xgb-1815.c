#include "xgb-1815.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/rmt.h"
#include "driver/gpio.h"

// RMT通道配置
#define RMT_LED_CHANNEL RMT_CHANNEL_0

// XGB-1815时序定义（单位：RMT时钟周期，10MHz时钟）
// 0码(0.295±0.15μs高电平, 0.595±0.25μs低电平)
// 1码(0.595±0.25μs高电平, 0.295±0.15μs低电平)
// 复位码：低电平时间≥80μs
#define XGB_1815_T0H_TICKS 3     // 逻辑0的高电平时间 (0.3us)
#define XGB_1815_T0L_TICKS 6     // 逻辑0的低电平时间 (0.6us)
#define XGB_1815_T1H_TICKS 6     // 逻辑1的高电平时间 (0.6us)
#define XGB_1815_T1L_TICKS 3     // 逻辑1的低电平时间 (0.3us)
#define XGB_1815_RESET_TICKS 800 // 复位信号 (80us)

// RMT配置
static rmt_channel_t led_channel = RMT_CHANNEL_0;

const uint8_t off_led[3] = {0, 0, 0};         // 关闭 LED
const uint8_t white_led[3] = {255, 255, 255}; // 白色 LED
const uint8_t red_led[3] = {255, 0, 0};       // 红色 LED
const uint8_t green_led[3] = {0, 255, 0};     // 绿色 LED
const uint8_t blue_led[3] = {0, 0, 255};      // 蓝色 LED
const uint8_t yellow_led[3] = {255, 255, 0};  // 黄色 LED
const uint8_t cyan_led[3] = {0, 255, 255};    // 青色 LED
const uint8_t purple_led[3] = {255, 0, 255};  // 紫色 LED
const uint8_t orange_led[3] = {255, 165, 0};  // 橙色 LED

static void xgb_1815_set_data(uint8_t *data)
{
    // 准备RGB数据，按照GRB顺序发送
    uint8_t rgb_data[3];
    rgb_data[0] = data[1]; // G
    rgb_data[1] = data[0]; // R
    rgb_data[2] = data[2]; // B

    // 创建RMT数据项，包括数据位和复位信号
    rmt_item32_t rmt_items[25]; // 8 bits * 3 bytes = 24 items + 1 reset item

    // 将每个字节转换为RMT项
    for (int byte = 0; byte < 3; byte++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            int item_index = byte * 8 + bit;
            bool bit_value = (rgb_data[byte] >> (7 - bit)) & 0x01;

            if (bit_value)
            {
                // 逻辑1: 高电平0.6us，低电平0.3us
                rmt_items[item_index].duration0 = XGB_1815_T1H_TICKS;
                rmt_items[item_index].level0 = 1;
                rmt_items[item_index].duration1 = XGB_1815_T1L_TICKS;
                rmt_items[item_index].level1 = 0;
            }
            else
            {
                // 逻辑0: 高电平0.3us，低电平0.6us
                rmt_items[item_index].duration0 = XGB_1815_T0H_TICKS;
                rmt_items[item_index].level0 = 1;
                rmt_items[item_index].duration1 = XGB_1815_T0L_TICKS;
                rmt_items[item_index].level1 = 0;
            }
        }
    }

    // 添加复位信号
    rmt_items[24].duration0 = XGB_1815_RESET_TICKS;
    rmt_items[24].level0 = 0;
    rmt_items[24].duration1 = 0;
    rmt_items[24].level1 = 0;

    // 写入RMT数据
    rmt_write_items(led_channel, rmt_items, 25, true);
}

static void xgb_1815_set_one_colour(char colour)
{
    switch (colour)
    {
    case '0': // 关闭 LED
        xgb_1815_set_data(off_led);
        break;
    case 'W': // 白色
        xgb_1815_set_data(white_led);
        break;
    case 'R': // 红色
        xgb_1815_set_data(red_led);
        break;
    case 'G': // 绿色
        xgb_1815_set_data(green_led);
        break;
    case 'B': // 蓝色
        xgb_1815_set_data(blue_led);
        break;
    case 'Y': // 黄色
        xgb_1815_set_data(yellow_led);
        break;
    default:
        break;
    }
}

void xgb_1815_set_colour_array(char *colour_array, int length, uint8_t level)
{
    // 创建RMT数据项，包括所有LED的数据位和一次复位信号
    rmt_item32_t rmt_items[length * 24 + 1]; // 每个LED 24 bits + 1 reset item

    // 为每个LED创建数据
    for (int led_index = 0; led_index < length; led_index++)
    {
        // 获取当前LED的颜色数据
        uint8_t *color_data;
        switch (colour_array[led_index])
        {
        case '0': // 关闭 LED
            color_data = (uint8_t *)off_led;
            break;
        case 'W': // 白色
            color_data = (uint8_t *)white_led;
            break;
        case 'R': // 红色
            color_data = (uint8_t *)red_led;
            break;
        case 'G': // 绿色
            color_data = (uint8_t *)green_led;
            break;
        case 'B': // 蓝色
            color_data = (uint8_t *)blue_led;
            break;
        case 'Y': // 黄色
            color_data = (uint8_t *)yellow_led;
            break;
        case 'C': // 青色
            color_data = (uint8_t *)cyan_led;
            break;
        case 'P': // 紫色
            color_data = (uint8_t *)purple_led;
            break;
        case 'O': // 橙色
            color_data = (uint8_t *)orange_led;
            break;
        default:
            continue; // 跳过无效颜色
        }

        // 准备RGB数据，按照GRB顺序发送
        uint8_t rgb_data[3];
        rgb_data[0] = color_data[1] * level / 5; // G
        rgb_data[1] = color_data[0] * level / 5; // R
        rgb_data[2] = color_data[2] * level / 5; // B

        // 将每个字节转换为RMT项
        for (int byte = 0; byte < 3; byte++)
        {
            for (int bit = 0; bit < 8; bit++)
            {
                int item_index = led_index * 24 + byte * 8 + bit;
                bool bit_value = (rgb_data[byte] >> (7 - bit)) & 0x01;

                if (bit_value)
                {
                    // 逻辑1: 高电平0.6us，低电平0.3us
                    rmt_items[item_index].duration0 = XGB_1815_T1H_TICKS;
                    rmt_items[item_index].level0 = 1;
                    rmt_items[item_index].duration1 = XGB_1815_T1L_TICKS;
                    rmt_items[item_index].level1 = 0;
                }
                else
                {
                    // 逻辑0: 高电平0.3us，低电平0.6us
                    rmt_items[item_index].duration0 = XGB_1815_T0H_TICKS;
                    rmt_items[item_index].level0 = 1;
                    rmt_items[item_index].duration1 = XGB_1815_T0L_TICKS;
                    rmt_items[item_index].level1 = 0;
                }
            }
        }
    }

    // 添加复位信号
    rmt_items[length * 24].duration0 = XGB_1815_RESET_TICKS;
    rmt_items[length * 24].level0 = 0;
    rmt_items[length * 24].duration1 = 0;
    rmt_items[length * 24].level1 = 0;

    // 一次性写入所有RMT数据
    rmt_write_items(led_channel, rmt_items, length * 24 + 1, true);
}

void xgb_1815_set_colour_change(uint8_t colour[], int length, uint8_t level)
{
    // 创建RMT数据项，包括所有LED的数据位和一次复位信号
    rmt_item32_t rmt_items[length * 24 + 1]; // 每个LED 24 bits + 1 reset item

    // 准备RGB数据，按照GRB顺序发送
        uint8_t rgb_data[3];
        rgb_data[0] = colour[1] * level / 5; // G
        rgb_data[1] = colour[0] * level / 5; // R
        rgb_data[2] = colour[2] * level / 5; // B
        
    // 为每个LED创建数据
    for (int led_index = 0; led_index < length; led_index++)
    {
        // 将每个字节转换为RMT项
        for (int byte = 0; byte < 3; byte++)
        {
            for (int bit = 0; bit < 8; bit++)
            {
                int item_index = led_index * 24 + byte * 8 + bit;
                bool bit_value = (rgb_data[byte] >> (7 - bit)) & 0x01;

                if (bit_value)
                {
                    // 逻辑1: 高电平0.6us，低电平0.3us
                    rmt_items[item_index].duration0 = XGB_1815_T1H_TICKS;
                    rmt_items[item_index].level0 = 1;
                    rmt_items[item_index].duration1 = XGB_1815_T1L_TICKS;
                    rmt_items[item_index].level1 = 0;
                }
                else
                {
                    // 逻辑0: 高电平0.3us，低电平0.6us
                    rmt_items[item_index].duration0 = XGB_1815_T0H_TICKS;
                    rmt_items[item_index].level0 = 1;
                    rmt_items[item_index].duration1 = XGB_1815_T0L_TICKS;
                    rmt_items[item_index].level1 = 0;
                }
            }
        }
    }

    // 添加复位信号
    rmt_items[length * 24].duration0 = XGB_1815_RESET_TICKS;
    rmt_items[length * 24].level0 = 0;
    rmt_items[length * 24].duration1 = 0;
    rmt_items[length * 24].level1 = 0;

    // 一次性写入所有RMT数据
    rmt_write_items(led_channel, rmt_items, length * 24 + 1, true);
}

void xgb_1815_flush(void)
{
    // 等待所有RMT数据发送完成
    rmt_wait_tx_done(led_channel, pdMS_TO_TICKS(100));
}

void xgb_1815_init(void)
{
    // 配置RMT
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(XGB_1815_DATA_GPIO, led_channel);
    config.clk_div = 8; // 80MHz/8 = 10MHz, 0.1us per tick，提供更高精度

    // 设置时钟源
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.carrier_freq_hz = 10000;
    config.tx_config.carrier_duty_percent = 50;
    config.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    config.tx_config.idle_output_en = true;

    // 应用配置
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    // 初始化GPIO为低电平
    gpio_set_level(XGB_1815_DATA_GPIO, 0);

    ESP_LOGI("XGB-1815", "RMT初始化完成，GPIO: %d，时钟分频: %d", XGB_1815_DATA_GPIO, config.clk_div);
}