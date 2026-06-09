/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CONTACT_MANAGER_H
#define CONTACT_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// 姓名最大长度
#define MAX_NAME_LENGTH 32
// 电话号码最大长度
#define MAX_PHONE_LENGTH 16
// 默认联系人数量
#define DEFAULT_CONTACT_COUNT 10

/**
 * @brief 联系人结构体
 */
typedef struct {
  char name[MAX_NAME_LENGTH];   // 姓名
  char phone[MAX_PHONE_LENGTH]; // 电话号码
} contact_t;

/**
 * @brief 初始化联系人管理器
 *
 * 分配PSRAM内存并初始化默认联系人数据
 *
 * @return esp_err_t
 *         - ESP_OK: 初始化成功
 *         - ESP_ERR_NO_MEM: 内存分配失败
 */
esp_err_t contact_manager_init(void);

/**
 * @brief 通过姓名查找电话号码
 *
 * @param name 要查找的姓名
 * @param phone 存储找到的电话号码的缓冲区，缓冲区大小应至少为MAX_PHONE_LENGTH
 * @return esp_err_t
 *         - ESP_OK: 查找成功
 *         - ESP_ERR_NOT_FOUND: 未找到指定姓名的联系人
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t contact_get_phone_by_name(const char *name, char *phone);

/**
 * @brief 释放联系人管理器内存
 */
void contact_manager_deinit(void);

/**
 * @brief 从Flash加载通讯录
 *
 * 从NVS Flash中读取通讯录数据并加载到PSRAM
 * 如果Flash中没有数据，则使用默认联系人数据
 *
 * @return esp_err_t
 *         - ESP_OK: 加载成功
 *         - ESP_ERR_NO_MEM: 内存分配失败
 *         - ESP_FAIL: 加载失败
 */
esp_err_t contact_manager_load_from_flash(void);

/**
 * @brief 保存通讯录到Flash
 *
 * 将PSRAM中的通讯录数据保存到NVS Flash
 *
 * @return esp_err_t
 *         - ESP_OK: 保存成功
 *         - ESP_FAIL: 保存失败
 */
esp_err_t contact_manager_save_to_flash(void);

/**
 * @brief 更新通讯录数据
 *
 * 使用新的联系人数据更新PSRAM中的通讯录
 * 并自动保存到Flash
 *
 * @param contacts 新的联系人数组
 * @param count 联系人数量
 * @return esp_err_t
 *         - ESP_OK: 更新成功
 *         - ESP_ERR_NO_MEM: 内存分配失败
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t contact_manager_update(const contact_t *contacts, size_t count);

/**
 * @brief 获取联系人数量
 *
 * @return size_t 当前联系人数量
 */
size_t contact_manager_get_count(void);

/**
 * @brief 获取所有联系人
 *
 * @param contacts 输出参数，存储联系人数组的指针
 * @param count 输出参数，存储联系人数量
 * @return esp_err_t
 *         - ESP_OK: 获取成功
 *         - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t contact_manager_get_all(const contact_t **contacts, size_t *count);

/**
 * @brief 添加或更新单个联系人
 *
 * 如果联系人不存在，则添加到通讯录
 * 如果联系人已存在，则更新电话号码
 * 更新后会自动保存到Flash
 *
 * @param name 联系人姓名
 * @param phone 电话号码
 * @return esp_err_t
 *         - ESP_OK: 添加或更新成功
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t contact_manager_add_or_update(const char *name, const char *phone);

#ifdef __cplusplus
}
#endif

#endif // CONTACT_MANAGER_H