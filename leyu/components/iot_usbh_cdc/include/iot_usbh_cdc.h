/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"
#include "iot_usbh_cdc_type.h"
#include "usbh_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handle to a CDC device, used for communication with the USB device
typedef struct usbh_cdc_t *usbh_cdc_handle_t;
typedef struct usbh_cdc_port_t *usbh_cdc_port_handle_t;

/**
 * @brief CDC driver configuration
 *
 */
typedef struct {
    size_t task_stack_size;                    /*!< Stack size of the driver's task */
    unsigned task_priority;                    /*!< Priority of the driver's task */
    int task_coreid;                           /*!< Core of the driver's task, Set it to -1 to not specify the core. */
    bool skip_init_usb_host_driver;            /*!< Skip initialization of USB host driver */
} usbh_cdc_driver_config_t;

/**
 * @brief Callback function for CDC device notifications
 *
 * This callback is invoked when a CDC device sends a notification event. The notification
 * data is passed through the notif parameter.
 *
 * @param[in] cdc_handle Handle to the CDC device that sent the notification
 * @param[in] notif Pointer to the notification data structure containing notification details
 * @param[in] user_data User data pointer that was passed during callback registration
 */
typedef void (*usbh_cdc_notif_cb_t)(usbh_cdc_port_handle_t cdc_handle, iot_cdc_notification_t *notif, void *user_data);

/**
 * @brief Callback structure for CDC device events
 *
 */
typedef struct {
    /**
     * @brief Callback function for when the CDC device is matched and connected, set NULL if not use
     * This function is called when a CDC device is connected and matches the specified criteria.
     * It allows the user to perform actions such as opening ports.
     *
     * @param[in] cdc_handle Handle to the CDC device that sent the notification
     * @param[in] user_data User data pointer that was passed during callback registration
     * @param[in] matched_intf_num Number of matched interface, -1 is invalid
     * */
    void (*connect)(usbh_cdc_handle_t cdc_handle, void *user_data, int matched_intf_num);

    /**
     * @brief Callback function for when the CDC device is disconnected, set NULL if not use
     * This function is called when the CDC device is disconnected.
     * It allows the user to perform actions such as closing ports. If user does not close ports, the driver will close them automatically.
     *
     * @param[in] cdc_handle Handle to the CDC device that sent the notification
     * @param[in] user_data User data pointer that was passed during callback registration
     * */
    void (*disconnect)(usbh_cdc_handle_t cdc_handle, void *user_data);

    void *user_data;                        /*!< Pointer to user data that will be passed to the callbacks */
} usbh_cdc_event_callbacks_t;

/**
 * @brief Callback structure for CDC port events
 *
 */
typedef struct {
    /**
     * @brief Callback function for when the CDC port is opened, set NULL if not use
     *
     * @param[in] port_handle Handle to the CDC port that was opened
     * @param[in] user_data User data pointer that was passed during callback registration
     */
    void (*closed)(usbh_cdc_port_handle_t port_handle, void *user_data);

    /**
     * @brief Callback function for when data is received on the CDC port, set NULL if not use
     *
     * @param[in] port_handle Handle to the CDC port that received data
     * @param[in] user_data User data pointer that was passed during callback registration
     */
    void (*recv_data)(usbh_cdc_port_handle_t port_handle, void *user_data);

    /**
     * @brief Callback function for when a notification is received on the CDC port, set NULL if not use
     *
     * @param[in] port_handle Handle to the CDC port that received a notification
     * @param[in] notif Pointer to the notification data structure containing notification details
     * @param[in] user_data User data pointer that was passed during callback registration
     */
    void (*notif_cb)(usbh_cdc_port_handle_t port_handle, iot_cdc_notification_t *notif, void *user_data);

    void *user_data;                       /*!< Pointer to user data that will be passed to the callbacks */
} usbh_cdc_port_event_callbacks_t;

/**
 * @brief Configuration structure for initializing a USB CDC device
 */
typedef struct usbh_cdc_config {
    const usb_device_match_id_t *dev_match_id_list;       /*!< Device match ID for the USB CDC device, must be end with empty entry */
    usbh_cdc_event_callbacks_t cbs;                       /*!< Event callbacks for the CDC device */
} usbh_cdc_device_config_t;

/**
 * @brief Configuration structure for initializing a USB CDC port
 *
 */
typedef struct {
    uint8_t itf_num;                         /*!< Interface number for the CDC port */
    size_t in_ringbuf_size;                  /*!< Size of the receive ringbuffer size, if set to 0, not use ringbuffer */
    size_t out_ringbuf_size;                 /*!< Size of the transmit ringbuffer size, if set to 0, not use ringbuffer */
    size_t in_transfer_buffer_size;          /*!< Size of the IN transfer buffer size */
    size_t out_transfer_buffer_size;         /*!< Size of the OUT transfer buffer size */
    usbh_cdc_port_event_callbacks_t cbs;     /*!< Event callbacks for the CDC port */
} usbh_cdc_port_config_t;

/**
 * @brief Install the USB CDC driver
 *
 * This function installs and initializes the USB CDC driver. It sets up internal data structures, creates necessary tasks,
 * and registers the USB host client. If the USB host driver is not already initialized, it will create and initialize it.
 *
 * @param config Pointer to a configuration structure (`usbh_cdc_driver_config_t`) that specifies the driver's settings such as task stack size, priority, core affinity, and new device callback.
 *
 * @return
 *     - ESP_OK: Driver installed successfully
 *     - ESP_ERR_INVALID_ARG: Null configuration parameter or invalid arguments
 *     - ESP_ERR_INVALID_STATE: USB CDC driver already installed
 *     - ESP_ERR_NO_MEM: Memory allocation failed
 *     - ESP_FAIL: Failed to create necessary tasks or USB host installation failed
 *
 * @note If the `skip_init_usb_host_driver` flag in the config is set to true, the function will skip the USB host driver initialization.
 *
 * The function performs the following steps:
 * 1. Verifies input arguments and state.
 * 2. Optionally initializes the USB host driver.
 * 3. Allocates memory for the USB CDC object.
 * 4. Creates necessary synchronization primitives (event group, mutex).
 * 5. Creates the driver task.
 * 6. Registers the USB host client with the provided configuration.
 * 7. Initializes the CDC driver structure and resumes the driver task.
 *
 * In case of an error, the function will clean up any resources that were allocated before the failure occurred.
 */
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config);

/**
 * @brief Uninstall the USB CDC driver
 *
 * This function uninstalls the USB CDC driver, releasing any allocated resources and stopping all driver-related tasks.
 * It ensures that all CDC devices are closed before proceeding with the uninstallation.
 *
 * @return
 *     - ESP_OK: Driver uninstalled successfully
 *     - ESP_ERR_INVALID_STATE: Driver is not installed or devices are still active
 *     - ESP_ERR_NOT_FINISHED: Timeout occurred while waiting for the CDC task to finish
 */
esp_err_t usbh_cdc_driver_uninstall(void);

/**
 * @brief Create a new USB CDC device handle
 *
 * This function allocates and initializes a new USB CDC device based on the provided configuration, including setting up the ring buffers
 * for data transmission and reception.
 *
 * @param[in] config Pointer to a `usbh_cdc_device_config_t` structure containing the configuration for the new device
 * @param[out] cdc_handle Pointer to a variable that will hold the created CDC device handle upon successful creation
 *
 * @return
 *     - ESP_OK: CDC device successfully created
 *     - ESP_ERR_INVALID_STATE: USB CDC driver is not installed
 *     - ESP_ERR_INVALID_ARG: Null `cdc_handle` or `config` parameter
 *     - ESP_ERR_NO_MEM: Memory allocation failed or ring buffer creation failed
 *     - ESP_FAIL: Failed to open the CDC device
 */
esp_err_t usbh_cdc_create(const usbh_cdc_device_config_t *config, usbh_cdc_handle_t *cdc_handle);

/**
 * @brief Delete a USB CDC device handle
 *
 * This function deletes the specified USB CDC device handle, freeing its associated resources, including ring buffers and semaphores.
 * It will close any open ports associated with the device.
 *
 * @param[in] cdc_handle The CDC device handle to delete
 *
 * @return
 *     - ESP_OK: CDC device deleted successfully
 *     - ESP_ERR_INVALID_STATE: USB CDC driver is not installed
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 */
esp_err_t usbh_cdc_delete(usbh_cdc_handle_t cdc_handle);

/**
 * @brief Open a USB CDC port on a USB CDC device
 * * This function opens a USB CDC port on the specified USB CDC device, allowing communication through the port.
 *
 * @param[in] cdc_handle The CDC device handle
 * @param[in] port_config Pointer to a `usb_cdc_port_config_t` structure
 * @param[in] ticks_to_wait The maximum time to wait for the port to be opened
 * @param[out] cdc_port_out Pointer to a variable that will hold the opened CDC port handle upon successful opening
 *
 * @return
 *     - ESP_OK: Port opened successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments (NULL handle or port_config)
 *     - ESP_ERR_INVALID_STATE: Device is not connected
 *     - ESP_ERR_TIMEOUT: Port open timed out
 */
esp_err_t usbh_cdc_port_open(usbh_cdc_handle_t cdc_handle, const usbh_cdc_port_config_t *port_config, TickType_t ticks_to_wait, usbh_cdc_port_handle_t *cdc_port_out);

/**
 * @brief Close a USB CDC port
 *
 * @param[in] cdc_port_handle The CDC port handle to close
 *
 * @return
 *    - ESP_OK: Port closed successfully
 *    - ESP_ERR_INVALID_ARG: Invalid CDC port handle provided
 *    - ESP_ERR_INVALID_STATE: USB CDC driver is not installed or port is not open
 */
esp_err_t usbh_cdc_port_close(usbh_cdc_port_handle_t cdc_port_handle);

/**
 * @brief Send a custom control request to the USB CDC port
 *
 * This function sends a custom control request to the USB CDC device with the specified parameters.
 * It can be used for both IN and OUT transfers.
 *
 * @param[in] cdc_handle The CDC handle to send the request to
 * @param[in] bmRequestType The request type bitmask
 * @param[in] bRequest The request ID
 * @param[in] wValue The request value
 * @param[in] wIndex The request index
 * @param[in] wLength The length of data to transfer
 * @param[in,out] data Pointer to the data buffer:
 *                     - For OUT transfers: data to be sent to device
 *                     - For IN transfers: buffer to store received data
 *
 * @return
 *     - ESP_OK: Request sent successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments (NULL handle/data buffer)
 *     - ESP_ERR_INVALID_STATE: Device is not connected
 *     - ESP_ERR_TIMEOUT: Control transfer timed out
 *     - ESP_ERR_INVALID_RESPONSE: Control transfer failed
 */
esp_err_t usbh_cdc_send_custom_request(usbh_cdc_handle_t cdc_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data);

/**
 * @brief Write data to the USB CDC port
 *
 * With internal ring buffer, this function writes data to the specified USB CDC device by pushing the data into the output ring buffer.
 * If the buffer is full or the device is not connected, the write will fail.
 *
 * Without internal ring buffer, this function writes data to the specified USB CDC device by sending data directly to the device.
 * Please set the ticks_to_wait for blocking write.
 *
 * @param[in] cdc_port_handle The CDC port handle
 * @param[in] buf Pointer to the data buffer to write
 * @param[in] length Pointer to the length of data to write. On success, it remains unchanged, otherwise set to 0.
 * @param[in] ticks_to_wait The maximum amount of time to wait for the write operation to complete
 *
 * @return
 *     - ESP_OK: Data written successfully
 *     - ESP_ERR_INVALID_ARG: Invalid argument (NULL handle, buffer, or length)
 *     - ESP_ERR_INVALID_STATE: Device is not connected
 */
esp_err_t usbh_cdc_write_bytes(usbh_cdc_port_handle_t cdc_port_handle, const uint8_t *buf, size_t length, TickType_t ticks_to_wait);

/**
 * @brief Read data from the USB CDC port
 *
 * With internal ring buffer, this function reads data from the specified USB CDC device by popping data from the input ring buffer.
 * If no data is available or the device is not connected, the read will fail.
 *
 * Without internal ring buffer, this function reads data from the specified USB CDC device by receiving data directly from the device. You can call this function in the recv_data callback function.
 * Please set ticks_to_wait to zero for non-blocking read.
 *
 * @param[in] cdc_port_handle The CDC port handle
 * @param[out] buf Pointer to the buffer where the read data will be stored
 * @param[inout] length Pointer to the length of data to read. On success, it is updated with the actual bytes read.
 * @param[in] ticks_to_wait The maximum amount of time to wait for the read operation to complete
 *
 * @note If the port is not opened with ring buffer, the length must be equal to the internal transfer buffer size, otherwise the function will return ESP_ERR_INVALID_ARG.
 *
 * @return
 *     - ESP_OK: Data read successfully
 *     - ESP_FAIL: Failed to read data
 *     - ESP_ERR_INVALID_ARG: Invalid argument (NULL handle, buffer, or length)
 *     - ESP_ERR_INVALID_STATE: Device is not connected
 */
esp_err_t usbh_cdc_read_bytes(usbh_cdc_port_handle_t cdc_port_handle, uint8_t *buf, size_t *length, TickType_t ticks_to_wait);

/**
 * @brief Flush the receive buffer of the USB CDC port
 *
 * This function clears the receive buffer, discarding any data currently in the buffer.
 *
 * Not supported for no internal ring buffer mode.
 *
 * @param[in] cdc_port_handle The CDC port handle
 *
 * @return
 *     - ESP_OK: Receive buffer flushed successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 *     - ESP_ERR_NOT_SUPPORTED: Not supported for no internal ring buffer mode
 */
esp_err_t usbh_cdc_flush_rx_buffer(usbh_cdc_port_handle_t cdc_port_handle);

/**
 * @brief Flush the transmit buffer of the USB CDC port
 *
 * This function clears the transmit buffer, discarding any data currently in the buffer.
 *
 * Not supported for no internal ring buffer mode.
 *
 * @param[in] cdc_port_handle The CDC port handle
 *
 * @return
 *     - ESP_OK: Transmit buffer flushed successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 *     - ESP_ERR_NOT_SUPPORTED: Not supported for no internal ring buffer mode
 */
esp_err_t usbh_cdc_flush_tx_buffer(usbh_cdc_port_handle_t cdc_port_handle);

/**
 * @brief Get the size of the receive buffer of the USB CDC port
 *
 * This function retrieves the current size of the receive buffer in bytes.
 *
 * For no internal ring buffer, you can call this function in the recv_data callback function.
 *
 * @param[in] cdc_port_handle The CDC port handle
 * @param[out] size Pointer to store the size of the receive buffer
 *
 * @return
 *     - ESP_OK: Size retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 */
esp_err_t usbh_cdc_get_rx_buffer_size(usbh_cdc_port_handle_t cdc_port_handle, size_t *size);

/**
 * @brief Get the connect state of given interface
 *
 * @param[in] cdc_handle The CDC device handle
 * @param[out] is_connected Pointer to store the connect state
 *
 * @return
 *     - ESP_OK: Connect state retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 */
esp_err_t usbh_cdc_is_connected(usbh_cdc_handle_t cdc_handle, bool *is_connected);

/**
 * @brief Print the USB CDC device descriptors
 *
 * This function retrieves and prints the USB device and configuration descriptors
 * for the specified USB CDC device. It checks that the device is properly connected
 * and open before retrieving and printing the descriptors.
 *
 * @param[in] cdc_handle The CDC device handle
 *
 * @return
 *     - ESP_OK: Descriptors printed successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 *     - ESP_ERR_INVALID_STATE: Device is not connected or not yet open
 */
esp_err_t usbh_cdc_desc_print(usbh_cdc_handle_t cdc_handle);

/**
 * @brief Get the device handle of the USB CDC device
 *
 * This function retrieves the device handle associated with the specified USB CDC device.
 *
 * @param[in] cdc_handle The CDC device handle
 * @param[out] dev_handle Pointer to store the device handle
 *
 * @return
 *     - ESP_OK: Device handle retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 *     - ESP_ERR_INVALID_STATE: Device is not connected or not yet open
 */
esp_err_t usbh_cdc_get_dev_handle(usbh_cdc_handle_t cdc_handle, usb_device_handle_t *dev_handle);

/**
 * @brief Get the CDC device handle of the USB CDC port
 *
 * @param[in] port_handle The CDC port handle
 * @param[out] cdc_handle Pointer to store the CDC device handle
 *
 * @return
 *   - ESP_OK: CDC handle retrieved successfully
 *   - ESP_ERR_INVALID_ARG: Invalid port handle provided
 */
esp_err_t usbh_cdc_port_get_cdc_handle(usbh_cdc_port_handle_t port_handle, usbh_cdc_handle_t *cdc_handle);

/**
 * @brief Get the parsed information of the USB CDC device
 *
 * @param[in] port_handle The CDC port handle
 * @param[out] notif_intf Pointer to store the notification interface descriptor
 * @param[out] data_intf Pointer to store the data interface descriptor
 *
 * @return
 *     - ESP_OK: Parsed information retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid CDC handle provided
 *     - ESP_ERR_INVALID_STATE: Device is not connected or not yet open
 */
esp_err_t usbh_cdc_port_get_intf_desc(usbh_cdc_port_handle_t port_handle, const usb_intf_desc_t **notif_intf,  const usb_intf_desc_t **data_intf);

#ifdef __cplusplus
}
#endif
