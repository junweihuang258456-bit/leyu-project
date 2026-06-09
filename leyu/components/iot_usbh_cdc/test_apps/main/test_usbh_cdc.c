/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "unity.h"
#include "iot_usbh_cdc.h"
#include "esp_log.h"

#define TAG "cdc_test"

extern int force_link;
ssize_t TEST_MEMORY_LEAK_THRESHOLD = 0;

#define UPDATE_LEAK_THRESHOLD(first_val) \
static bool is_first = true; \
if (is_first) { \
    TEST_MEMORY_LEAK_THRESHOLD = first_val; \
} else { \
    TEST_MEMORY_LEAK_THRESHOLD = 0; \
}

EventGroupHandle_t s_event_group_hdl = NULL;

#define USBH_CDC_DEVICE_COMMON_CONFIG(conn_cb, disconn_cb, _idVendor, _idProduct) \
    { \
        .dev_match_id_list = (usb_device_match_id_t[]) { \
            { \
                .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT, \
                .idVendor = _idVendor, \
                .idProduct = _idProduct, \
            }, \
            { 0 } \
        }, \
        .cbs = { \
            .connect = conn_cb, \
            .disconnect = disconn_cb, \
            .user_data = NULL, \
        }, \
    }

#define USBH_CDC_PORT_COMMON_CONFIG(_itf_num, _notif_cb, _recv_data_cb) \
    { \
        .itf_num = _itf_num, \
        .in_transfer_buffer_size = 512 * 4, \
        .out_transfer_buffer_size = 512 * 4, \
        .cbs = { \
            .notif_cb = _notif_cb, \
            .recv_data = _recv_data_cb, \
            .user_data = NULL, \
        }, \
    }

#define EVENT_CONNECT BIT0
#define EVENT_CONNECT2 BIT1

static usbh_cdc_port_handle_t cdc_port1 = NULL, cdc_port2 = NULL;
static EventGroupHandle_t event_group;

static void usb_communication(uint8_t loop_count, size_t data_length, usbh_cdc_port_handle_t port_handle)
{
    uint8_t buff[512] = {0};
    for (int i = 0; i < data_length; i++) {
        buff[i] = i;
    }

    while (loop_count--) {
        size_t length = data_length;

        /*!< Send data */
        usbh_cdc_write_bytes(port_handle, buff, length, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Send data len: %d", length);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buff, length, ESP_LOG_INFO);
        vTaskDelay(500 / portTICK_PERIOD_MS);

        /*!< Receive data */
        usbh_cdc_get_rx_buffer_size(port_handle, &length);
        usbh_cdc_read_bytes(port_handle, buff, &length, 0);
        ESP_LOGI(TAG, "Recv data len: %d", length);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buff, length, ESP_LOG_INFO);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

static void usb_at_test(uint8_t loop_count, usbh_cdc_port_handle_t port_handle)
{
    while (loop_count--) {
        char buff[64] = "AT+GMR\r\n";
        size_t length = strlen(buff);

        /*!< Send data */
        usbh_cdc_write_bytes(port_handle, (uint8_t *)buff, length, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Send data len: %d", length);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buff, length, ESP_LOG_INFO);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void cdc_notif_cb(usbh_cdc_port_handle_t port_handle, iot_cdc_notification_t *notif, void *user_data)
{
    ESP_LOGI(TAG, "cdc device notify");
}

static void cdc_recv_data_cb(usbh_cdc_port_handle_t port_handle, void *arg)
{
    int rx_length = 0;
    static uint8_t buf[512] = {0};
    usbh_cdc_get_rx_buffer_size(port_handle, (size_t *)&rx_length);
    if (rx_length > sizeof(buf)) {
        rx_length = sizeof(buf);
    }
    if (rx_length > 0) {
        usbh_cdc_read_bytes(port_handle, buf, (size_t *)&rx_length, 0);
        ESP_LOGI(TAG, "USB received data len: %d", rx_length);
        ESP_LOG_BUFFER_HEXDUMP("cdc: if_input", buf, rx_length, ESP_LOG_INFO);
    }
}

static void cdc_disconnect_cb(usbh_cdc_handle_t handle, void *arg)
{
    ESP_LOGW(TAG, "cdc device disconnect");
}

static void cdc_send_request(usbh_cdc_handle_t handle)
{
    uint8_t bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    uint8_t bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    uint16_t wValue = (USB_W_VALUE_DT_STRING << 8) | (1 & 0xFF);
#define LANG_ID_ENGLISH 0x409
    uint16_t wIndex = LANG_ID_ENGLISH;

    uint8_t str[64] = {0};
    uint16_t wLength = 64;
    esp_err_t ret = usbh_cdc_send_custom_request(handle, bmRequestType, bRequest, wValue, wIndex, wLength, str);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ESP_LOG_BUFFER_HEXDUMP("Received string descriptor", str, sizeof(str), ESP_LOG_INFO);
}

static void install()
{
    usbh_cdc_driver_config_t config = {
        .task_stack_size = 1024 * 4,
        .task_priority = 5,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
    };

    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_driver_install(&config));
}

static void uninstall()
{
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_driver_uninstall());
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

TEST_CASE("usb cdc driver install/uninstall", "[iot_usbh_cdc][install-uninstall]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);
    install();
    uninstall();
}

TEST_CASE("usbh cdc device memory leak", "[iot_usbh_cdc][create-delete][auto]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);
    install();

    usbh_cdc_device_config_t dev_config = USBH_CDC_DEVICE_COMMON_CONFIG(NULL, NULL, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_handle_t handle[5] = {};
    for (int i = 0; i < sizeof(handle) / sizeof(handle[0]); i++) {
        ESP_LOGI(TAG, "Create cdc handle %d", i);
        TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_create(&dev_config, &handle[i]));
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    for (int i = 0; i < 5; i++) {
        ESP_LOGI(TAG, "Delete cdc handle %d", i);
        TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle[i]));
    }

    uninstall();
}

TEST_CASE("usb cdc open error", "[iot_usbh_cdc][open-close][auto]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);

    install();

    usbh_cdc_device_config_t dev_config = USBH_CDC_DEVICE_COMMON_CONFIG(NULL, cdc_disconnect_cb, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_handle_t handle = NULL;
    usbh_cdc_create(&dev_config, &handle);

    usbh_cdc_port_config_t cdc_port_config = USBH_CDC_PORT_COMMON_CONFIG(20, cdc_notif_cb, cdc_recv_data_cb);
    ESP_LOGI(TAG, "Try to open invalid interface");
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(100), &cdc_port1));

    ESP_LOGI(TAG, "Try to open invalid interface1");
    cdc_port_config.itf_num = 40;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(10000), &cdc_port1));

    ESP_LOGI(TAG, "Try to open invalid interface2");
    cdc_port_config.itf_num = 30;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(10000), &cdc_port2));

    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle));
    uninstall();
}

TEST_CASE("usb cdc R/W", "[iot_usbh_cdc][read-write][auto]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);

    install();

    usbh_cdc_device_config_t dev_config = USBH_CDC_DEVICE_COMMON_CONFIG(NULL, cdc_disconnect_cb, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_handle_t handle = NULL;
    usbh_cdc_create(&dev_config, &handle);

    usbh_cdc_port_config_t cdc_port_config = USBH_CDC_PORT_COMMON_CONFIG(0, cdc_notif_cb, cdc_recv_data_cb);
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(10000), &cdc_port1));
    cdc_port_config.itf_num = 3;
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(10000), &cdc_port2));

    cdc_send_request(handle);

    // usb_communication(5, 128, cdc_port1);
    // usb_communication(5, 64, cdc_port1);
    usb_at_test(5, cdc_port2);

    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle));
    uninstall();
}

static void single_dev_cdc_connect_cb(usbh_cdc_handle_t handle, void *arg, int matched_intf)
{
    ESP_LOGI(TAG, "FUNC(%s)", __func__);

    usbh_cdc_port_config_t cdc_port_config = USBH_CDC_PORT_COMMON_CONFIG(0, cdc_notif_cb, cdc_recv_data_cb);
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(1000), &cdc_port1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(1000), &cdc_port1));

    if (event_group) {
        xEventGroupSetBits(event_group, EVENT_CONNECT);
    }
}

TEST_CASE("usb cdc R/W callback", "[iot_usbh_cdc][read-write][auto]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);

    install();
    event_group = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(event_group);

    usbh_cdc_device_config_t dev_config = USBH_CDC_DEVICE_COMMON_CONFIG(single_dev_cdc_connect_cb, cdc_disconnect_cb, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_handle_t handle = NULL;
    usbh_cdc_create(&dev_config, &handle);

    EventBits_t bits = xEventGroupWaitBits(event_group, EVENT_CONNECT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    TEST_ASSERT_BITS(EVENT_CONNECT, EVENT_CONNECT, bits);

    // usb_communication(5, 128, cdc_port1);
    // usb_communication(5, 64, cdc_port1);
    usb_at_test(5, cdc_port1);

    vEventGroupDelete(event_group);
    event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle));
    uninstall();
}

static void multi_dev1_cdc_connect_cb(usbh_cdc_handle_t handle, void *arg, int matched_intf)
{
    ESP_LOGI(TAG, "FUNC(%s)", __func__);

    usbh_cdc_port_config_t cdc_port_config = USBH_CDC_PORT_COMMON_CONFIG(0, cdc_notif_cb, cdc_recv_data_cb);
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(1000), &cdc_port1));

    if (event_group) {
        xEventGroupSetBits(event_group, EVENT_CONNECT);
    }
}

static void multi_dev2_cdc_connect_cb(usbh_cdc_handle_t handle, void *arg, int matched_intf)
{
    ESP_LOGI(TAG, "FUNC(%s)", __func__);

    usbh_cdc_port_config_t cdc_port_config = USBH_CDC_PORT_COMMON_CONFIG(0, cdc_notif_cb, cdc_recv_data_cb);
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_port_open(handle, &cdc_port_config, pdMS_TO_TICKS(1000), &cdc_port2));

    if (event_group) {
        xEventGroupSetBits(event_group, EVENT_CONNECT2);
    }
}

TEST_CASE("usb cdc R/W callback multiple match", "[iot_usbh_cdc][read-write]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    esp_log_level_set("USBH_CDC", ESP_LOG_DEBUG);

    install();
    event_group = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(event_group);

    usbh_cdc_device_config_t dev_config = USBH_CDC_DEVICE_COMMON_CONFIG(multi_dev1_cdc_connect_cb, cdc_disconnect_cb, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_handle_t handle1 = NULL, handle2 = NULL;
    usbh_cdc_create(&dev_config, &handle1);
    usbh_cdc_device_config_t dev_config2 = USBH_CDC_DEVICE_COMMON_CONFIG(multi_dev2_cdc_connect_cb, cdc_disconnect_cb, USB_DEVICE_VENDOR_ANY, USB_DEVICE_PRODUCT_ANY);
    usbh_cdc_create(&dev_config2, &handle2);

    EventBits_t bits = xEventGroupWaitBits(event_group, EVENT_CONNECT | EVENT_CONNECT2, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));
    TEST_ASSERT_BITS(EVENT_CONNECT | EVENT_CONNECT2, EVENT_CONNECT | EVENT_CONNECT2, bits);

    usb_communication(5, 128, cdc_port1);
    usb_communication(5, 64, cdc_port1);
    usb_at_test(5, cdc_port2);

    vEventGroupDelete(event_group);
    event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle1));
    TEST_ASSERT_EQUAL(ESP_OK, usbh_cdc_delete(handle2));
    uninstall();
}

static size_t before_free_8bit;
static size_t before_free_32bit;

static void check_leak(size_t before_free, size_t after_free, const char *type)
{
    ssize_t delta = after_free - before_free;
    printf("MALLOC_CAP_%s: Before: %u bytes free, After: %u bytes free (delta:%d)\n", type, before_free, after_free, delta);
    if (!(delta >= TEST_MEMORY_LEAK_THRESHOLD)) {
        ESP_LOGE(TAG, "Memory leak detected, delta: %d bytes, threshold: %d bytes", delta, TEST_MEMORY_LEAK_THRESHOLD);
    }
}

void setUp(void)
{
    before_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    before_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
}

void tearDown(void)
{
    size_t after_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t after_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    check_leak(before_free_8bit, after_free_8bit, "8BIT");
    check_leak(before_free_32bit, after_free_32bit, "32BIT");
}

void app_main(void)
{
    force_link = 1;

    printf("IOT USBH CDC TEST \n");
    unity_run_menu();
}
