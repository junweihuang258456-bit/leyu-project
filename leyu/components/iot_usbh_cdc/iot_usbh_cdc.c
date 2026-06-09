/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "esp_private/usb_phy.h"
#include "iot_usbh_cdc.h"
#include "usb/usb_host.h"
#include "iot_usbh_descriptor.h"

static const char *TAG = "USBH_CDC";

// CDC spinlock
static portMUX_TYPE cdc_lock =  portMUX_INITIALIZER_UNLOCKED;
#define CDC_ENTER_CRITICAL()    portENTER_CRITICAL(&cdc_lock)
#define CDC_EXIT_CRITICAL()     portEXIT_CRITICAL(&cdc_lock)

// CDC-ACM events
#define CDC_TEARDOWN            BIT0
#define CDC_TEARDOWN_COMPLETE   BIT1
#define CDC_DEV_CONNECTED       BIT0

#define TIMEOUT_USB_RINGBUF_MS  200                      /*! Timeout for ring buffer operate */
#define CDC_CTRL_TIMEOUT_MS     5000                     /*! Timeout for control transfer */

#define CDC_PORT_SIGNATURE1 0x55AA1234
#define CDC_PORT_SIGNATURE2 0x55AA5678

typedef struct {
    usb_host_client_handle_t cdc_client_hdl;             /*!< USB Host handle reused for all CDC-ACM devices in the system */
    EventGroupHandle_t event_group;
    SemaphoreHandle_t open_close_mutex;
    SLIST_HEAD(list_dev, usbh_cdc_s) cdc_devices_list;   /*!< List of open pseudo devices */
} usbh_cdc_obj_t;

typedef struct usbh_cdc_s {
    usb_device_handle_t dev_hdl;                  // USB device handle
    uint8_t dev_addr;                             // Device address
    const usb_device_match_id_t *match_id_list;   // device match ID
    struct {
        usb_transfer_t *xfer;
        SemaphoreHandle_t mux;
    } ctrl;

    usbh_cdc_event_callbacks_t cbs;        // Callbacks for the pseudo device
    EventGroupHandle_t dev_event_group;
    bool to_disconnect;                     // Flag to indicate if the host wants to disconnect the device
    SLIST_ENTRY(usbh_cdc_s) list_entry;
    SLIST_HEAD(list_dev_port, usbh_cdc_port_s) cdc_devices_port_list;   /*!< List of open pseudo devices */
} usbh_cdc_t;

/**
 * define the USB CDC port structure
 */
typedef struct usbh_cdc_port_s {
    uint32_t signature1; // Used to verify pointer validity
    usbh_cdc_t *cdc; // pointer to the cdc pseudo device
    struct {
        usb_transfer_t *xfer;         // Notification transfer
        const usb_intf_desc_t *intf_desc;
    } notif;
    struct {
        usb_transfer_t *out_xfer;          // OUT data transfer
        SemaphoreHandle_t out_xfer_free_sem;
        SemaphoreHandle_t out_mux;
        usb_transfer_t *in_xfer;           // IN data transfer
        uint16_t in_mps;                   // IN endpoint Maximum Packet Size
        uint8_t *in_data_buffer_base;      // Pointer to IN data buffer in usb_transfer_t
        const usb_intf_desc_t *intf_desc;  // Pointer to data interface descriptor
    } data;

    int itf_num;                            /*!< Interface number of this CDC port */
    usbh_cdc_port_event_callbacks_t cbs; /*!< Callbacks for the CDC port */
    RingbufHandle_t in_ringbuf_handle;     /*!< in ringbuffer handle of corresponding interface */
    size_t in_ringbuf_size;
    RingbufHandle_t out_ringbuf_handle;    /*!< out ringbuffer handle of corresponding interface */
    size_t out_ringbuf_size;
    SLIST_ENTRY(usbh_cdc_port_s) list_entry;
    uint32_t signature2;
} usbh_cdc_port_t;

static usbh_cdc_obj_t *p_usbh_cdc_obj = NULL;

static void _cdc_transfers_free(usbh_cdc_port_t *cdc_port);
static esp_err_t _cdc_dev_ctrl_allocate(usbh_cdc_t *cdc);
static esp_err_t _cdc_dev_ctrl_free(usbh_cdc_t *cdc);
static void _cdc_tx_xfer_submit(usb_transfer_t *out_xfer);

/*--------------------------------- CDC Buffer Handle Code [RINGBUF_TYPE_BYTEBUF] --------------------------------------*/
static size_t _get_ringbuf_len(RingbufHandle_t ringbuf_hdl)
{
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    return uxItemsWaiting;
}

static esp_err_t _ringbuf_pop(RingbufHandle_t ringbuf_hdl, uint8_t *buf, size_t req_bytes, size_t *read_bytes, TickType_t ticks_to_wait)
{
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, read_bytes, ticks_to_wait, req_bytes);
    if (!buf_rcv) {
        return ESP_FAIL;
    }

    memcpy(buf, buf_rcv, *read_bytes);
    vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));

    size_t read_bytes2 = 0;
    if (*read_bytes < req_bytes) {
        buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes2, 0, req_bytes - *read_bytes);
        if (buf_rcv) {
            memcpy(buf + *read_bytes, buf_rcv, read_bytes2);
            *read_bytes += read_bytes2;
            vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        }
    }

    return ESP_OK;
}

static esp_err_t _ringbuf_push(RingbufHandle_t ringbuf_hdl, const uint8_t *buf, size_t write_bytes, TickType_t ticks_to_wait)
{
    int res = xRingbufferSend(ringbuf_hdl, buf, write_bytes, ticks_to_wait);
    if (res != pdTRUE) {
        ESP_LOGW(TAG, "The ringbuffer is full, the data has been lost");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void _ringbuf_flush(RingbufHandle_t ringbuf_hdl)
{
    assert(ringbuf_hdl);
    size_t read_bytes = 0;
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes, 0, uxItemsWaiting);

    if (buf_rcv) {
        vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
    }

    if (uxItemsWaiting > read_bytes) {
        // read the second time to flush all data
        vRingbufferGetInfo(ringbuf_hdl, NULL, NULL, NULL, NULL, &uxItemsWaiting);
        buf_rcv = xRingbufferReceiveUpTo(ringbuf_hdl, &read_bytes, 0, uxItemsWaiting);
        if (buf_rcv) {
            vRingbufferReturnItem(ringbuf_hdl, (void *)(buf_rcv));
        }
    }
}

static void usb_lib_task(void *arg)
{
    usb_host_lib_info_t info;
    esp_err_t res =  usb_host_lib_info(&info);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "USB Host library installed by others, devices: %d, clients: %d", info.num_devices, info.num_clients);
    } else if (res == ESP_ERR_INVALID_STATE) {
        // Install USB Host driver. Should only be called once in entire application
        // 使用共享中断标志，避免与WiFi/蓝牙中断冲突
        const usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_SHARED,
        };
        ESP_ERROR_CHECK(usb_host_install(&host_config));
    } else {
        assert(0); // Should never reach here
    }

    //Signalize the usbh_cdc_driver_install, the USB host library has been installed
    xTaskNotifyGive(arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
            if (ESP_OK == usb_host_device_free_all()) {
                ESP_LOGI(TAG, "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
                has_clients = false;
            } else {
                ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
                has_devices = true;
            }
        }
        if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
            has_clients = false;
        }
    }
    ESP_LOGI(TAG, "No more clients and devices, uninstall USB Host library");

    // Clean up USB Host
    vTaskDelay(100); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    ESP_LOGD(TAG, "USB Host library is uninstalled");
    vTaskDelete(NULL);
}

/**
 * @brief CDC-ACM driver handling task
 *
 * USB host client registration and deregistration is handled here.
 *
 * @param[in] arg User's argument. Handle of a task that started this task.
 */
static void usbh_cdc_client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Task will be resumed from cdc_acm_host_install()
    usbh_cdc_obj_t *usbh_cdc_obj = p_usbh_cdc_obj; // Make local copy of the driver's handle
    assert(usbh_cdc_obj->cdc_client_hdl);
    // Start handling client's events
    while (1) {
        usb_host_client_handle_events(usbh_cdc_obj->cdc_client_hdl, portMAX_DELAY);
        EventBits_t events = xEventGroupGetBits(usbh_cdc_obj->event_group);
        if (events & CDC_TEARDOWN) {
            break;
        }
    }
    xEventGroupSetBits(usbh_cdc_obj->event_group, CDC_TEARDOWN_COMPLETE);
    vTaskDelete(NULL);
}

/**
 * @brief close all CDC ports
 *
 * @param cdc Pointer to the CDC device
 * @return int The number of closed ports
 */
static int _try_close_all_port(usbh_cdc_t *cdc)
{
    int ret = 0;
    usbh_cdc_port_t *current, *tmp;
    SLIST_FOREACH_SAFE(current, &(cdc->cdc_devices_port_list), list_entry, tmp) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(usbh_cdc_port_close((usbh_cdc_port_handle_t)current));
        ret++;
    }
    return ret;
}

static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        ESP_LOGI(TAG, "New device connected, address: %d", event_msg->new_dev.address);
        usb_device_handle_t current_device = NULL;
        esp_err_t err_rc_ = usb_host_device_open(p_usbh_cdc_obj->cdc_client_hdl, event_msg->new_dev.address, &current_device);
        if (ESP_OK != err_rc_) {
            ESP_LOGE(TAG, "Could not open device %d (%s)", event_msg->new_dev.address, esp_err_to_name(err_rc_));
            return;
        }
        assert(current_device);

        const usb_device_desc_t *device_desc;
        const usb_config_desc_t *config_desc;
        ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(current_device, &config_desc));
        ESP_ERROR_CHECK(usb_host_get_device_descriptor(current_device, &device_desc));

        if (device_desc->bDeviceClass == USB_CLASS_HUB) {
            ESP_LOGI(TAG, "Detect hub device, skip");
            usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, current_device);
            return;
        }

        // Find CDC device matched with specified match ID
        usbh_cdc_t *cdc, *found_cdc = NULL;
        SLIST_FOREACH(cdc, &p_usbh_cdc_obj->cdc_devices_list, list_entry) {
            if (cdc->dev_hdl) {
                continue; // already opened
            }
            int match_intf = -1;
            if (usbh_match_id_from_list(device_desc, config_desc, cdc->match_id_list, &match_intf) == 1) {
                cdc->dev_addr = event_msg->new_dev.address;
                cdc->dev_hdl = current_device;
                found_cdc = cdc;
                xEventGroupSetBits(cdc->dev_event_group, CDC_DEV_CONNECTED);
                if (cdc->cbs.connect) {
                    cdc->cbs.connect((usbh_cdc_handle_t)cdc, cdc->cbs.user_data, match_intf);
                }
                break;
            }
        }

        // If no matched device found or the device is not already opened, close the device
        bool need_to_close = (NULL == found_cdc) || SLIST_EMPTY(&found_cdc->cdc_devices_port_list);
        if (need_to_close) {
            usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, current_device);
            if (found_cdc && found_cdc->dev_hdl) {
                found_cdc->dev_hdl = NULL;
            }
        }

        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        ESP_LOGD(TAG, "Device suddenly disconnected");
        // Find CDC pseudo-devices associated with this USB device and close them

        usbh_cdc_t *cdc;
        SLIST_FOREACH(cdc, &p_usbh_cdc_obj->cdc_devices_list, list_entry) {
            if (cdc->dev_hdl != event_msg->dev_gone.dev_hdl) {
                continue;
            }
            cdc->to_disconnect = true; // signal to stop submitting new transfers
            xEventGroupClearBits(cdc->dev_event_group, CDC_DEV_CONNECTED);
            // handling client's events to make sure all transfers are done
            usb_host_client_handle_events(p_usbh_cdc_obj->cdc_client_hdl, pdMS_TO_TICKS(100));

            if (cdc->cbs.disconnect) {
                cdc->cbs.disconnect((usbh_cdc_handle_t)cdc, cdc->cbs.user_data);
            }

            // Close all ports associated with this CDC pseudo-device
            _try_close_all_port(cdc);
            _cdc_dev_ctrl_free(cdc);
            cdc->dev_hdl = NULL;
            cdc->to_disconnect = false; // reset the flag
        }
        break;
    }
    default:
        assert(false);
        break;
    }
}

esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj == NULL, ESP_ERR_INVALID_STATE, TAG, "usbh_cdc already installed");

    ESP_LOGI(TAG, "iot usbh cdc version: %d.%d.%d", IOT_USBH_CDC_VER_MAJOR, IOT_USBH_CDC_VER_MINOR, IOT_USBH_CDC_VER_PATCH);

    if (!config->skip_init_usb_host_driver) {
        BaseType_t core_id = (CONFIG_USBH_TASK_CORE_ID < 0) ? tskNO_AFFINITY : CONFIG_USBH_TASK_CORE_ID;
        BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), CONFIG_USBH_TASK_BASE_PRIORITY, NULL, core_id);
        ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_FAIL, TAG, "xTaskCreatePinnedToCore failed");
        // Wait unit the USB host library is installed
        uint32_t notify_value = ulTaskNotifyTake(false, pdMS_TO_TICKS(1000));
        if (notify_value == 0) {
            ESP_LOGE(TAG, "USB host library not installed");
            return ESP_FAIL;
        }
    }

    p_usbh_cdc_obj = (usbh_cdc_obj_t *) calloc(1, sizeof(usbh_cdc_obj_t));
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_NO_MEM, TAG, "calloc failed");
    EventGroupHandle_t event_group = xEventGroupCreate();
    SemaphoreHandle_t cdc_mutex = xSemaphoreCreateMutex();
    TaskHandle_t driver_task_h = NULL;
    bool client_registered = false;

    BaseType_t core_id = (config->task_coreid < 0) ? tskNO_AFFINITY : config->task_coreid;
    xTaskCreatePinnedToCore(usbh_cdc_client_task, "usbh_cdc", config->task_stack_size, NULL, config->task_priority, &driver_task_h, core_id);
    if (p_usbh_cdc_obj == NULL || driver_task_h == NULL || event_group == NULL || cdc_mutex == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    // Register USB Host client
    usb_host_client_handle_t usb_client = NULL;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usb_event_cb,
        .async.callback_arg = NULL
    };
    ESP_GOTO_ON_ERROR(usb_host_client_register(&client_config, &usb_client), err, TAG, "Failed to register USB host client");
    client_registered = true;

    // Initialize CDC driver structure
    SLIST_INIT(&(p_usbh_cdc_obj->cdc_devices_list));

    p_usbh_cdc_obj->cdc_client_hdl = usb_client;
    p_usbh_cdc_obj->event_group = event_group;
    p_usbh_cdc_obj->open_close_mutex = cdc_mutex;

    xTaskNotifyGive(driver_task_h);
    return ESP_OK;

err: // Clean-up
    if (client_registered) {
        ESP_ERROR_CHECK(usb_host_client_deregister(usb_client));
    }
    if (p_usbh_cdc_obj) {
        free(p_usbh_cdc_obj);
    }
    if (event_group) {
        vEventGroupDelete(event_group);
    }
    if (driver_task_h) {
        vTaskDelete(driver_task_h);
    }
    if (cdc_mutex) {
        vSemaphoreDelete(cdc_mutex);
    }
    return ret;
}

esp_err_t usbh_cdc_driver_uninstall(void)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj, ESP_ERR_INVALID_STATE, TAG, "usbh cdc not installed");

    CDC_ENTER_CRITICAL();
    if (SLIST_EMPTY(&p_usbh_cdc_obj->cdc_devices_list)) {
        // Check that device list is empty (all devices closed)
    } else {
        ESP_EARLY_LOGE(TAG, "Cannot uninstall usbh cdc driver, there are still exist devices");
        ret = ESP_ERR_INVALID_STATE;
        CDC_EXIT_CRITICAL();
        goto unblock;
    }
    CDC_EXIT_CRITICAL();

    // Signal to CDC task to stop, unblock it and wait for its deletion
    xEventGroupSetBits(p_usbh_cdc_obj->event_group, CDC_TEARDOWN);
    usb_host_client_unblock(p_usbh_cdc_obj->cdc_client_hdl);
    ESP_GOTO_ON_FALSE(
        xEventGroupWaitBits(p_usbh_cdc_obj->event_group, CDC_TEARDOWN_COMPLETE, pdTRUE, pdFALSE, portMAX_DELAY),
        ESP_ERR_NOT_FINISHED, unblock, TAG, "Wait for CDC_TEARDOWN_COMPLETE failed");
    vEventGroupDelete(p_usbh_cdc_obj->event_group);
    vSemaphoreDelete(p_usbh_cdc_obj->open_close_mutex);
    ESP_ERROR_CHECK(usb_host_client_deregister(p_usbh_cdc_obj->cdc_client_hdl));

    free(p_usbh_cdc_obj);
    p_usbh_cdc_obj = NULL;

unblock:
    return ret;
}

static esp_err_t _cdc_reset_transfer_endpoint(usb_device_handle_t dev_hdl, usb_transfer_t *transfer)
{
    assert(dev_hdl);
    assert(transfer);
    ESP_RETURN_ON_ERROR(usb_host_endpoint_halt(dev_hdl, transfer->bEndpointAddress), TAG, "usb_host_endpoint_halt failed");
    ESP_RETURN_ON_ERROR(usb_host_endpoint_flush(dev_hdl, transfer->bEndpointAddress), TAG, "usb_host_endpoint_flush failed");
    usb_host_endpoint_clear(dev_hdl, transfer->bEndpointAddress);
    return ESP_OK;
}

static inline esp_err_t _cdc_host_transfer_submit(usbh_cdc_port_t *cdc_port, usb_transfer_t *xfer)
{
    if (cdc_port->cdc->to_disconnect) {
        return ESP_ERR_INVALID_STATE;
    }
    return usb_host_transfer_submit(xfer);
}

static void notif_xfer_cb(usb_transfer_t *notif_xfer)
{
    ESP_LOGD(TAG, "notif xfer cb");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)notif_xfer->context;

    switch (notif_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        if (cdc_port->cbs.notif_cb) {
            iot_cdc_notification_t *notif = (iot_cdc_notification_t *)notif_xfer->data_buffer;
            cdc_port->cbs.notif_cb((usbh_cdc_port_handle_t)cdc_port, notif, cdc_port->cbs.user_data);
        }
        // Start polling for new data again
        ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
        _cdc_host_transfer_submit(cdc_port, cdc_port->notif.xfer);
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return;
    default:
        // Any other error
        ESP_LOGW(TAG, "Notif xfer failed, EP:%d, status %d", notif_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, notif_xfer->status);
        break;
    }
}

static void control_xfer_cb(usb_transfer_t *ctrl_xfer)
{
    ESP_LOGD(TAG, "control xfer cb");
    assert(ctrl_xfer->context);
    xSemaphoreGive((SemaphoreHandle_t)ctrl_xfer->context);
}

static void in_xfer_cb(usb_transfer_t *in_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)in_xfer->context;

    switch (in_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        if (cdc_port->in_ringbuf_handle) {
            size_t data_len = _get_ringbuf_len(cdc_port->in_ringbuf_handle);
            if (data_len + in_xfer->actual_num_bytes >= cdc_port->in_ringbuf_size) {
                // TODO: add notify cb for user
                // if ringbuffer overflow, drop the data
                ESP_LOGW(TAG, "CDC in ringbuf is full!");
            } else {
                ESP_LOG_BUFFER_HEXDUMP(TAG, in_xfer->data_buffer, in_xfer->actual_num_bytes, ESP_LOG_DEBUG);
                if (_ringbuf_push(cdc_port->in_ringbuf_handle, in_xfer->data_buffer, in_xfer->actual_num_bytes, pdMS_TO_TICKS(TIMEOUT_USB_RINGBUF_MS)) != ESP_OK) {
                    ESP_LOGE(TAG, "in ringbuf push failed");
                }
            }
        }

        if (cdc_port->cbs.recv_data) {
            cdc_port->cbs.recv_data((usbh_cdc_port_handle_t)cdc_port, cdc_port->cbs.user_data);
        }

        _cdc_host_transfer_submit(cdc_port, in_xfer);
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return;
    default:
        // Any other error
        ESP_LOGW(TAG, "IN Transfer failed, EP:%d, status %d", in_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, in_xfer->status);
        bool is_connected = (xEventGroupGetBits(cdc_port->cdc->dev_event_group) & CDC_DEV_CONNECTED) != 0;
        if (is_connected) {
            // For improved reliability, the endpoint is reset on any error
            // Reset the endpoint and submit the transfer again if the device is still connected
            _cdc_reset_transfer_endpoint(cdc_port->cdc->dev_hdl, in_xfer);
            _cdc_host_transfer_submit(cdc_port, in_xfer);
        }
        break;
    }
}

static void out_xfer_cb(usb_transfer_t *out_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)out_xfer->context;

    switch (out_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        if (cdc_port->out_ringbuf_handle) {
            _cdc_tx_xfer_submit(out_xfer);
        } else {
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
        return;
    }
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        // User is notified about device disconnection from usb_event_cb
        // No need to do anything
        return;
    default:
        // Any other error
        ESP_LOGW(TAG, "OUT Transfer failed, EP:%d, status %d", out_xfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK, out_xfer->status);
        break;
    }
}

static void _cdc_tx_xfer_submit(usb_transfer_t *out_xfer)
{
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) out_xfer->context;

    if (cdc_port->out_ringbuf_handle) {
        size_t data_len = _get_ringbuf_len(cdc_port->out_ringbuf_handle);
        if (data_len > 0) {
            if (data_len > out_xfer->data_buffer_size) {
                data_len = out_xfer->data_buffer_size;
            }
            size_t actual_num_bytes = 0;
            _ringbuf_pop(cdc_port->out_ringbuf_handle, out_xfer->data_buffer, data_len, &actual_num_bytes, 0);
            out_xfer->num_bytes = actual_num_bytes;
            usb_host_transfer_submit(out_xfer);
        } else {
            xSemaphoreGive(cdc_port->data.out_xfer_free_sem);
        }
    } else {
        usb_host_transfer_submit(out_xfer);
    }
}

static esp_err_t _cdc_dev_ctrl_free(usbh_cdc_t *cdc)
{
    if (cdc->ctrl.xfer) {
        xSemaphoreTake(cdc->ctrl.mux, portMAX_DELAY);

        if (cdc->ctrl.xfer->context) {
            vSemaphoreDelete(cdc->ctrl.xfer->context);
        }
        if (cdc->ctrl.mux) {
            vSemaphoreDelete(cdc->ctrl.mux);
        }
        usb_host_transfer_free(cdc->ctrl.xfer);
        cdc->ctrl.xfer = NULL;
    }
    return ESP_OK;
}

static esp_err_t _cdc_dev_ctrl_allocate(usbh_cdc_t *cdc)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(CONFIG_USBH_CDC_CONTROL_TRANSFER_BUFFER_SIZE + sizeof(usb_setup_packet_t), 0, &cdc->ctrl.xfer), err, TAG, "Failed to allocate ctrl transfer");
    cdc->ctrl.xfer->timeout_ms = 1000;
    cdc->ctrl.xfer->bEndpointAddress = 0;
    cdc->ctrl.xfer->device_handle = cdc->dev_hdl;
    cdc->ctrl.xfer->callback = control_xfer_cb;
    cdc->ctrl.xfer->context = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(cdc->ctrl.xfer->context, ESP_ERR_NO_MEM, err, TAG,);
    cdc->ctrl.mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc->ctrl.mux, ESP_ERR_NO_MEM, err, TAG,);
    return ret;
err:
    _cdc_dev_ctrl_free(cdc);
    return ret;
}

static esp_err_t _cdc_transfers_allocate(usbh_cdc_port_t *cdc_port, const usb_ep_desc_t *notif_ep_desc, const usb_ep_desc_t *in_ep_desc, const usb_ep_desc_t *out_ep_desc, size_t in_buf_len, size_t out_buf_len)
{
    esp_err_t ret = ESP_OK;

    if (notif_ep_desc) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(USB_EP_DESC_GET_MPS(notif_ep_desc), 0, &cdc_port->notif.xfer),
                          err, TAG, "Failed to allocate notif transfer");
        assert(cdc_port->notif.xfer);
        cdc_port->notif.xfer->device_handle = cdc_port->cdc->dev_hdl;
        cdc_port->notif.xfer->bEndpointAddress = notif_ep_desc->bEndpointAddress;
        cdc_port->notif.xfer->callback = notif_xfer_cb;
        cdc_port->notif.xfer->context = cdc_port;
        cdc_port->notif.xfer->num_bytes = USB_EP_DESC_GET_MPS(notif_ep_desc);
    }

    if (in_ep_desc) {
        if (in_buf_len > 0) {
            ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(in_buf_len, 0, &cdc_port->data.in_xfer),
                              err, TAG, "Failed to allocate data.in_xfer transfer");
            assert(cdc_port->data.in_xfer);
            cdc_port->data.in_xfer->context = cdc_port;
            cdc_port->data.in_xfer->num_bytes = in_buf_len;
            cdc_port->data.in_xfer->bEndpointAddress = in_ep_desc->bEndpointAddress;
            cdc_port->data.in_xfer->device_handle = cdc_port->cdc->dev_hdl;
            cdc_port->data.in_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
            cdc_port->data.in_data_buffer_base = cdc_port->data.in_xfer->data_buffer;
            cdc_port->data.in_xfer->callback = in_xfer_cb;
        }
    }

    if (out_ep_desc) {
        if (out_buf_len > 0) {
            ESP_GOTO_ON_ERROR(
                usb_host_transfer_alloc(out_buf_len, 0, &cdc_port->data.out_xfer),
                err, TAG, "Failed to allocate data.out_xfer transfer");
            assert(cdc_port->data.out_xfer);
            cdc_port->data.out_xfer->bEndpointAddress = out_ep_desc->bEndpointAddress;
            cdc_port->data.out_xfer->device_handle = cdc_port->cdc->dev_hdl;
            cdc_port->data.out_xfer->context = cdc_port;
            cdc_port->data.out_xfer->num_bytes = out_buf_len;
            cdc_port->data.out_xfer->callback = out_xfer_cb;
        }
    }
    return ret;
err:
    _cdc_transfers_free(cdc_port);
    return ret;
}

static void _cdc_transfers_free(usbh_cdc_port_t *cdc_port)
{
    assert(cdc_port);

    if (cdc_port->notif.xfer) {
        usb_host_transfer_free(cdc_port->notif.xfer);
    }
    if (cdc_port->data.in_xfer) {
        usb_host_transfer_free(cdc_port->data.in_xfer);
    }
    if (cdc_port->data.out_xfer) {
        usb_host_transfer_free(cdc_port->data.out_xfer);
    }
}

esp_err_t usbh_cdc_port_open(usbh_cdc_handle_t cdc_handle, const usbh_cdc_port_config_t *port_config, TickType_t ticks_to_wait, usbh_cdc_port_handle_t *cdc_port_out)
{
    esp_err_t ret = ESP_OK;
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    ESP_RETURN_ON_FALSE(cdc_port_out != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_out is NULL");
    ESP_RETURN_ON_FALSE(port_config != NULL, ESP_ERR_INVALID_ARG, TAG, "port_config is NULL");
    ESP_RETURN_ON_FALSE(port_config->in_transfer_buffer_size != 0, ESP_ERR_INVALID_ARG, TAG, "in_transfer_buffer_size is 0");
    ESP_RETURN_ON_FALSE(port_config->out_transfer_buffer_size != 0, ESP_ERR_INVALID_ARG, TAG, "out_transfer_buffer_size is 0");

    xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);

    // Check if the port is already opened
    usbh_cdc_port_t *tmp_cdc_port = NULL;
    SLIST_FOREACH(tmp_cdc_port, &cdc->cdc_devices_port_list, list_entry) {
        ESP_GOTO_ON_FALSE(!(tmp_cdc_port->cdc == cdc && tmp_cdc_port->itf_num == port_config->itf_num),
                          ESP_ERR_INVALID_STATE, err_exit, TAG, "port is already opened");
    }

    // Wait for the device to be connected
    EventBits_t bits = xEventGroupWaitBits(cdc->dev_event_group, CDC_DEV_CONNECTED, pdFALSE, pdFALSE, ticks_to_wait);
    ESP_GOTO_ON_FALSE((bits & CDC_DEV_CONNECTED) != 0, ESP_ERR_TIMEOUT, err_exit, TAG, "device not connected in time");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *)calloc(1, sizeof(usbh_cdc_port_t));
    ESP_GOTO_ON_FALSE(cdc_port != NULL, ESP_ERR_NO_MEM, err, TAG, "calloc failed");

    // If the device handle is not set, try to open the device
    if (NULL == cdc->dev_hdl) {
        usb_device_handle_t current_device = NULL;
        ESP_GOTO_ON_ERROR(usb_host_device_open(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_addr, &current_device), err, TAG, "Could not open device %d", cdc->dev_addr);
        cdc->dev_hdl = current_device;
    }

    // Get Device and Configuration descriptors
    const usb_config_desc_t *config_desc;
    const usb_device_desc_t *device_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(cdc->dev_hdl, &config_desc));
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(cdc->dev_hdl, &device_desc));
    usbh_cdc_parsed_info_t cdc_info = {0};
    ESP_GOTO_ON_ERROR(cdc_parse_interface_descriptor(device_desc, config_desc, port_config->itf_num, &cdc_info), err, TAG, "Could not parse descriptor on interface %d", port_config->itf_num);

    cdc_port->cbs = port_config->cbs;
    cdc_port->cdc = cdc;
    cdc_port->out_ringbuf_size = port_config->out_ringbuf_size;
    cdc_port->in_ringbuf_size = port_config->in_ringbuf_size;
    cdc_port->itf_num = port_config->itf_num;

    if (cdc_port->in_ringbuf_size != 0) {
        cdc_port->in_ringbuf_handle = xRingbufferCreate(cdc_port->in_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_GOTO_ON_FALSE(cdc_port->in_ringbuf_handle != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create ring buffer");
    }

    if (cdc_port->out_ringbuf_size != 0) {
        cdc_port->out_ringbuf_handle = xRingbufferCreate(cdc_port->out_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_GOTO_ON_FALSE(cdc_port->out_ringbuf_handle != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create ring buffer");
    }

    cdc_port->data.out_xfer_free_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(cdc_port->data.out_xfer_free_sem != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create mutex");
    xSemaphoreGive(cdc_port->data.out_xfer_free_sem);

    cdc_port->data.out_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_port->data.out_mux, ESP_ERR_NO_MEM, err, TAG, "Failed to create mutex for out transfer");

    if (cdc_info.notif_ep) {
        cdc_port->notif.intf_desc = cdc_info.notif_intf;
    }

    // Must have data interface descriptor
    cdc_port->data.intf_desc = cdc_info.data_intf;

    if (cdc_port->in_ringbuf_handle) {
        _ringbuf_flush(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        _ringbuf_flush(cdc_port->out_ringbuf_handle);
    }

    ESP_GOTO_ON_ERROR(_cdc_transfers_allocate(cdc_port, cdc_info.notif_ep, cdc_info.in_ep, cdc_info.out_ep,
                                              port_config->in_transfer_buffer_size, port_config->out_transfer_buffer_size), err, TAG, "Failed to allocate transfers");

    // Claim the interface
    ESP_GOTO_ON_ERROR(
        usb_host_interface_claim(
            p_usbh_cdc_obj->cdc_client_hdl,
            cdc_port->cdc->dev_hdl,
            cdc_port->data.intf_desc->bInterfaceNumber,
            cdc_port->data.intf_desc->bAlternateSetting),
        err, TAG, "Could not claim interface");

    if (cdc_port->notif.xfer) {
        // If notification are supported, claim its interface and start polling its IN endpoint
        if (cdc_port->notif.intf_desc != cdc_port->data.intf_desc) {
            ESP_GOTO_ON_ERROR(
                usb_host_interface_claim(
                    p_usbh_cdc_obj->cdc_client_hdl,
                    cdc_port->cdc->dev_hdl,
                    cdc_port->notif.intf_desc->bInterfaceNumber,
                    cdc_port->notif.intf_desc->bAlternateSetting),
                err, TAG, "Could not claim interface");
        }
    }

    CDC_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&(cdc->cdc_devices_port_list), cdc_port, list_entry);
    CDC_EXIT_CRITICAL();

    /* When the device unplugged, the usb_event_cb will close all ports and free the memory,
     * so we need to use the signiatures to check if the port is valid.
     * it not very elegant, but it works.
     */
    cdc_port->signature1 = CDC_PORT_SIGNATURE1;
    cdc_port->signature2 = CDC_PORT_SIGNATURE2;

    // Start polling for data
    if (cdc_port->data.in_xfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        ESP_ERROR_CHECK(usb_host_transfer_submit(cdc_port->data.in_xfer));
    }
    if (cdc_port->notif.xfer) {
        ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
        ESP_ERROR_CHECK(usb_host_transfer_submit(cdc_port->notif.xfer));
    }

    *cdc_port_out = (usbh_cdc_port_handle_t)cdc_port;
    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    return ESP_OK;

err:
    if (cdc_port->in_ringbuf_handle) {
        vRingbufferDelete(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        vRingbufferDelete(cdc_port->out_ringbuf_handle);
    }
    if (cdc_port->data.out_xfer_free_sem) {
        vSemaphoreDelete(cdc_port->data.out_xfer_free_sem);
    }

    if (cdc_port) {
        _cdc_transfers_free(cdc_port);
        free(cdc_port);
    }
    if (cdc->dev_hdl) {
        usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_hdl);
        cdc->dev_hdl = NULL;
    }
err_exit:
    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    return ret;
}

esp_err_t usbh_cdc_port_close(usbh_cdc_port_handle_t cdc_port_handle)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    usbh_cdc_t *cdc = cdc_port->cdc;
    ESP_RETURN_ON_FALSE(cdc != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc is NULL");
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");

    xSemaphoreTake(p_usbh_cdc_obj->open_close_mutex, portMAX_DELAY);

    CDC_ENTER_CRITICAL();
    cdc_port->cbs.notif_cb = NULL;
    cdc_port->cbs.recv_data = NULL;
    CDC_EXIT_CRITICAL();

    xSemaphoreTake(cdc_port->data.out_mux, portMAX_DELAY); // wait for all the writers to finish

    if (cdc_port->data.in_xfer) {
        ESP_ERROR_CHECK(_cdc_reset_transfer_endpoint(cdc->dev_hdl, cdc_port->data.in_xfer));
    }
    if (cdc_port->data.out_xfer) {
        ESP_ERROR_CHECK(_cdc_reset_transfer_endpoint(cdc->dev_hdl, cdc_port->data.out_xfer));
    }
    if (cdc_port->notif.xfer) {
        ESP_ERROR_CHECK(_cdc_reset_transfer_endpoint(cdc->dev_hdl, cdc_port->notif.xfer));
    }
    // handling client's events to make sure all states are updated
    usb_host_client_handle_events(p_usbh_cdc_obj->cdc_client_hdl, pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_hdl, cdc_port->data.intf_desc->bInterfaceNumber));
    if (cdc_port->notif.intf_desc && cdc_port->notif.intf_desc != cdc_port->data.intf_desc) {
        ESP_ERROR_CHECK(usb_host_interface_release(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_hdl, cdc_port->notif.intf_desc->bInterfaceNumber));
    }

    _cdc_transfers_free(cdc_port);

    if (cdc_port->in_ringbuf_handle) {
        _ringbuf_flush(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        _ringbuf_flush(cdc_port->out_ringbuf_handle);
    }

    CDC_ENTER_CRITICAL();
    SLIST_REMOVE(&(cdc->cdc_devices_port_list), cdc_port, usbh_cdc_port_s, list_entry);
    CDC_EXIT_CRITICAL();

    // if no ports left, close the device
    if (cdc->dev_hdl && SLIST_EMPTY(&cdc->cdc_devices_port_list)) {
        ESP_ERROR_CHECK(usb_host_device_close(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_hdl));
        cdc->dev_hdl = NULL;
    }

    if (cdc_port->in_ringbuf_handle) {
        vRingbufferDelete(cdc_port->in_ringbuf_handle);
    }
    if (cdc_port->out_ringbuf_handle) {
        vRingbufferDelete(cdc_port->out_ringbuf_handle);
    }
    if (cdc_port->data.out_xfer_free_sem) {
        vSemaphoreDelete(cdc_port->data.out_xfer_free_sem);
    }
    if (cdc_port->data.out_mux) {
        vSemaphoreDelete(cdc_port->data.out_mux);
    }

    if (cdc_port->cbs.closed) {
        cdc_port->cbs.closed((usbh_cdc_port_handle_t)cdc_port, cdc_port->cbs.user_data);
    }

    cdc_port->signature1 = 0; // invalidate the handle
    cdc_port->signature2 = 0;
    free(cdc_port);

    xSemaphoreGive(p_usbh_cdc_obj->open_close_mutex);
    return ret;
}

esp_err_t usbh_cdc_create(const usbh_cdc_device_config_t *config, usbh_cdc_handle_t *cdc_handle)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usbh_cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->dev_match_id_list != NULL, ESP_ERR_INVALID_ARG, TAG, "dev_match_id_list is NULL");

    usbh_cdc_t *cdc = (usbh_cdc_t *) calloc(1, sizeof(usbh_cdc_t));
    ESP_RETURN_ON_FALSE(cdc != NULL, ESP_ERR_NO_MEM, TAG, "calloc failed");

    cdc->dev_event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(cdc->dev_event_group != NULL, ESP_ERR_NO_MEM, err, TAG, "xEventGroupCreate failed");

    cdc->match_id_list = config->dev_match_id_list;
    cdc->cbs = config->cbs;
    cdc->dev_hdl = NULL;

    SLIST_INIT(&(cdc->cdc_devices_port_list));

    CDC_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&p_usbh_cdc_obj->cdc_devices_list, cdc, list_entry);
    CDC_EXIT_CRITICAL();

    *cdc_handle = (usbh_cdc_handle_t)cdc;
    return ret;

err:
    free(cdc);
    return ret;
}

esp_err_t usbh_cdc_delete(usbh_cdc_handle_t cdc_handle)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;

    cdc->to_disconnect = true; // signal to stop submitting new transfers
    // notify user about disconnection if device was connected
    if (cdc->dev_hdl != NULL && cdc->cbs.disconnect) {
        cdc->cbs.disconnect((usbh_cdc_handle_t)cdc, cdc->cbs.user_data);
    }

    _try_close_all_port(cdc);
    _cdc_dev_ctrl_free(cdc);

    CDC_ENTER_CRITICAL();
    SLIST_REMOVE(&p_usbh_cdc_obj->cdc_devices_list, cdc, usbh_cdc_s, list_entry);
    CDC_EXIT_CRITICAL();

    vEventGroupDelete(cdc->dev_event_group);
    free(cdc);
    return ESP_OK;
}

esp_err_t usbh_cdc_send_custom_request(usbh_cdc_handle_t cdc_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");

    esp_err_t ret = ESP_OK;
    if (wLength > 0) {
        ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    }
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;

    if (NULL == cdc->dev_hdl) {
        usb_device_handle_t current_device = NULL;
        ESP_RETURN_ON_ERROR(usb_host_device_open(p_usbh_cdc_obj->cdc_client_hdl, cdc->dev_addr, &current_device), TAG, "Could not open device %d", cdc->dev_addr);
        cdc->dev_hdl = current_device;
    }

    // Try to allocate ctrl
    if (cdc->ctrl.xfer == NULL) {
        ESP_RETURN_ON_ERROR(_cdc_dev_ctrl_allocate(cdc), TAG, "Failed to allocate ctrl");
    }
    ESP_RETURN_ON_FALSE(cdc->ctrl.xfer->data_buffer_size >= wLength, ESP_ERR_INVALID_ARG, TAG, "data buffer size is too small");

    BaseType_t taken = xSemaphoreTake(cdc->ctrl.mux, pdMS_TO_TICKS(CDC_CTRL_TIMEOUT_MS));
    if (taken == pdFALSE) {
        return ESP_ERR_TIMEOUT;
    }

    usb_setup_packet_t *req = (usb_setup_packet_t *) cdc->ctrl.xfer->data_buffer;
    uint8_t *start_of_data = (uint8_t *)req + sizeof(usb_setup_packet_t);
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    // For IN transfers we must transfer data ownership to CDC driver
    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer) {
        memcpy(start_of_data, data, wLength);
    }

    cdc->ctrl.xfer->num_bytes = wLength + sizeof(usb_setup_packet_t);
    ret = usb_host_transfer_submit_control(p_usbh_cdc_obj->cdc_client_hdl, cdc->ctrl.xfer);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "CTRL transfer failed");
        goto unblock;
    }

    taken = xSemaphoreTake((SemaphoreHandle_t)cdc->ctrl.xfer->context, pdMS_TO_TICKS(CDC_CTRL_TIMEOUT_MS));
    if (!taken) {
        // Transfer was not finished, error in USB LIB. Reset the endpoint
        _cdc_reset_transfer_endpoint(cdc->dev_hdl, cdc->ctrl.xfer);
        ret = ESP_ERR_TIMEOUT;
        goto unblock;
    }

    ESP_LOGD(TAG, "cdc->ctrl.xfer->actual_num_bytes = %d\n", cdc->ctrl.xfer->actual_num_bytes);
    if (cdc->ctrl.xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGD(TAG, "Control transfer error");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto unblock;
    }

    // For OUT transfers, we must transfer data ownership to user
    if (in_transfer) {
        memcpy(data, start_of_data, wLength);
    }
    ret = ESP_OK;
unblock:
    xSemaphoreGive(cdc->ctrl.mux);
    return ret;
}

esp_err_t usbh_cdc_write_bytes(usbh_cdc_port_handle_t cdc_port_handle, const uint8_t *buf, size_t length, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(length > 0, ESP_ERR_INVALID_ARG, TAG, "length is 0");

    esp_err_t ret = ESP_OK;
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");

    if (xSemaphoreTake(cdc_port->data.out_mux, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // if ringbuf is created, push data to ringbuf
    if (cdc_port->out_ringbuf_handle) {
        ret = _ringbuf_push(cdc_port->out_ringbuf_handle, buf, length, ticks_to_wait);
        ESP_GOTO_ON_ERROR(ret, fail, TAG, "cdc write ringbuf failed, buf = %p, len = %d", buf, length);

        if (xSemaphoreTake(cdc_port->data.out_xfer_free_sem, 0) == pdTRUE) {
            // here need a critical
            _cdc_tx_xfer_submit(cdc_port->data.out_xfer);
        }
    } else {
        ESP_RETURN_ON_FALSE(length <= cdc_port->data.out_xfer->data_buffer_size, ESP_ERR_INVALID_SIZE, TAG, "data length larger than transfer buffer size");
        // if ringbuf is not created, write data to usb
        BaseType_t taken = xSemaphoreTake(cdc_port->data.out_xfer_free_sem, ticks_to_wait);
        if (taken != pdTRUE) {
            ret = ESP_ERR_TIMEOUT;
            goto fail;
        }

        memcpy(cdc_port->data.out_xfer->data_buffer, buf, length);
        cdc_port->data.out_xfer->num_bytes = length;
        _cdc_tx_xfer_submit(cdc_port->data.out_xfer);
    }
fail:
    xSemaphoreGive(cdc_port->data.out_mux);
    return ret;
}

esp_err_t usbh_cdc_read_bytes(usbh_cdc_port_handle_t cdc_port_handle, uint8_t *buf, size_t *length, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(p_usbh_cdc_obj != NULL, ESP_ERR_INVALID_STATE, TAG, "usb cdc is not installed");
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "buf is NULL");
    ESP_GOTO_ON_FALSE(length != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "length is NULL");

    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    if (cdc_port->in_ringbuf_handle == NULL && ticks_to_wait != 0) {
        ESP_LOGW(TAG, "ticks_to_wait is invalid, in_ringbuf_handle is NULL");
    }

    if (cdc_port->in_ringbuf_handle) {
        size_t data_len = *length;
        if (data_len > cdc_port->in_ringbuf_size) {
            data_len = cdc_port->in_ringbuf_size;
        }

        ret = _ringbuf_pop(cdc_port->in_ringbuf_handle, buf, data_len, length, ticks_to_wait);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "cdc read failed");
            *length = 0;
            return ret;
        }
    } else {
        if (*length != cdc_port->data.in_xfer->actual_num_bytes) {
            // the length must be equal to actual_num_bytes, because we always read all the data in the buffer.
            ESP_LOGE(TAG, "length is invalid, length = %d, actual_num_bytes = %d", *length, cdc_port->data.in_xfer->actual_num_bytes);
            *length = cdc_port->data.in_xfer->actual_num_bytes;
            return ESP_ERR_INVALID_ARG;
        }
        memcpy((void *)buf, cdc_port->data.in_xfer->data_buffer, cdc_port->data.in_xfer->actual_num_bytes);
        *length = cdc_port->data.in_xfer->actual_num_bytes;
        cdc_port->data.in_xfer->actual_num_bytes = 0; // reset actual_num_bytes
    }
    return ESP_OK;
fail:
    *length = 0;
    return ret;
}

esp_err_t usbh_cdc_flush_rx_buffer(usbh_cdc_port_handle_t cdc_port_handle)
{
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    ESP_RETURN_ON_FALSE(cdc_port->in_ringbuf_handle != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "rx ringbuf is not created");
    _ringbuf_flush(cdc_port->in_ringbuf_handle);
    return ESP_OK;
}

esp_err_t usbh_cdc_flush_tx_buffer(usbh_cdc_port_handle_t cdc_port_handle)
{
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    ESP_RETURN_ON_FALSE(cdc_port->out_ringbuf_handle != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "tx ringbuf is not created");
    _ringbuf_flush(cdc_port->out_ringbuf_handle);
    return ESP_OK;
}

esp_err_t usbh_cdc_get_rx_buffer_size(usbh_cdc_port_handle_t cdc_port_handle, size_t *size)
{
    ESP_RETURN_ON_FALSE(cdc_port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) cdc_port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    if (cdc_port->in_ringbuf_handle) {
        *size = _get_ringbuf_len(cdc_port->in_ringbuf_handle);
    } else {
        *size = cdc_port->data.in_xfer->actual_num_bytes;
    }
    return ESP_OK;
}

esp_err_t usbh_cdc_is_connected(usbh_cdc_handle_t cdc_handle, bool *is_connected)
{
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    ESP_RETURN_ON_FALSE(is_connected != NULL, ESP_ERR_INVALID_ARG, TAG, "is_connected is NULL");
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;
    EventBits_t bits = xEventGroupGetBits(cdc->dev_event_group);
    *is_connected = ((bits & CDC_DEV_CONNECTED) != 0);
    return ESP_OK;
}

esp_err_t usbh_cdc_desc_print(usbh_cdc_handle_t cdc_handle)
{
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;
    ESP_RETURN_ON_FALSE(cdc->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");

    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_device_descriptor(cdc->dev_hdl, &device_desc));
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_active_config_descriptor(cdc->dev_hdl, &config_desc));
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, NULL);
    return ESP_OK;
}

esp_err_t usbh_cdc_get_dev_handle(usbh_cdc_handle_t cdc_handle, usb_device_handle_t *dev_handle)
{
    ESP_RETURN_ON_FALSE(cdc_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "cdc_handle is NULL");
    usbh_cdc_t *cdc = (usbh_cdc_t *) cdc_handle;
    ESP_RETURN_ON_FALSE(cdc->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");
    *dev_handle = cdc->dev_hdl;
    return ESP_OK;
}

esp_err_t usbh_cdc_port_get_cdc_handle(usbh_cdc_port_handle_t port_handle, usbh_cdc_handle_t *cdc_handle)
{
    ESP_RETURN_ON_FALSE(port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    *cdc_handle = (usbh_cdc_handle_t)cdc_port->cdc;
    return ESP_OK;
}

esp_err_t usbh_cdc_port_get_intf_desc(usbh_cdc_port_handle_t port_handle, const usb_intf_desc_t **notif_intf,  const usb_intf_desc_t **data_intf)
{
    ESP_RETURN_ON_FALSE(port_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "port_handle is NULL");
    usbh_cdc_port_t *cdc_port = (usbh_cdc_port_t *) port_handle;
    ESP_RETURN_ON_FALSE(cdc_port->cdc->dev_hdl != NULL, ESP_ERR_INVALID_STATE, TAG, "Device is not open yet");
    ESP_RETURN_ON_FALSE(cdc_port->signature1 == CDC_PORT_SIGNATURE1 && cdc_port->signature2 == CDC_PORT_SIGNATURE2, ESP_ERR_INVALID_ARG, TAG, "Invalid cdc port handle");
    if (notif_intf) {
        *notif_intf = cdc_port->notif.intf_desc;
    }
    if (data_intf) {
        *data_intf = cdc_port->data.intf_desc;
    }
    return ESP_OK;
}
