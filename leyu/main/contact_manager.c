/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "contact_manager.h"

static const char *TAG = "contact_manager";

// NVS命名空间和键名
#define CONTACT_NVS_NAMESPACE "contact_store"
#define CONTACT_NVS_KEY "contacts"
#define CONTACT_NVS_COUNT_KEY "count"

// 联系人数组指针，使用PSRAM分配
static contact_t *g_contacts = NULL;
// 联系人数量
static size_t g_contact_count = 0;
// 联系人数组容量
static size_t g_contact_capacity = 0;

esp_err_t contact_manager_init(void)
{
    // 如果已经初始化，先释放旧的内存
    if (g_contacts != NULL) {
        contact_manager_deinit();
    }
    
    // 分配PSRAM内存用于存储联系人数据
    g_contacts = (contact_t *)heap_caps_malloc(DEFAULT_CONTACT_COUNT * sizeof(contact_t), MALLOC_CAP_SPIRAM);
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "分配联系人内存失败");
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化联系人容量和数量
    g_contact_capacity = DEFAULT_CONTACT_COUNT;
    g_contact_count = 0;
    
    // 硬编码一些默认联系人数据（后续可改为从Flash读取）
    // 这些数据将存储在PSRAM中
    strcpy(g_contacts[0].name, "乐语客服");
    strcpy(g_contacts[0].phone, "10028");
    g_contact_count++;
    /*
    strcpy(g_contacts[1].name, "孙子");
    strcpy(g_contacts[1].phone, "19330031339");
    g_contact_count++;
    
    strcpy(g_contacts[2].name, "王五");
    strcpy(g_contacts[2].phone, "13700137000");
    g_contact_count++;
    
    strcpy(g_contacts[3].name, "赵六");
    strcpy(g_contacts[3].phone, "13600136000");
    g_contact_count++;
    
    strcpy(g_contacts[4].name, "钱七");
    strcpy(g_contacts[4].phone, "13500135000");*/
    g_contact_count++;
    
    ESP_LOGI(TAG, "联系人管理器初始化完成，共加载了 %" PRIu32 " 个联系人", (uint32_t)g_contact_count);
    return ESP_OK;
}

esp_err_t contact_get_phone_by_name(const char *name, char *phone)
{
    // 检查参数有效性
    if (name == NULL || phone == NULL) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查是否已初始化
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "联系人管理器未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 遍历联系人数组查找匹配的姓名
    for (size_t i = 0; i < g_contact_count; i++) {
        if (strcmp(g_contacts[i].name, name) == 0) {
            // 找到匹配的联系人，复制电话号码
            strcpy(phone, g_contacts[i].phone);
            ESP_LOGI(TAG, "找到联系人: %s, 电话: %s", name, phone);
            return ESP_OK;
        }
    }
    
    // 未找到匹配的联系人
    ESP_LOGW(TAG, "未找到姓名为 %s 的联系人", name);
    return ESP_ERR_NOT_FOUND;
}

void contact_manager_deinit(void)
{
    // 释放PSRAM内存
    if (g_contacts != NULL) {
        heap_caps_free(g_contacts);
        g_contacts = NULL;
    }
    
    // 重置变量
    g_contact_count = 0;
    g_contact_capacity = 0;
    
    ESP_LOGI(TAG, "联系人管理器已释放资源");
}

esp_err_t contact_manager_load_from_flash(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    ESP_LOGI(TAG, "从Flash加载通讯录");
    
    // 打开NVS命名空间
    err = nvs_open(CONTACT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开NVS命名空间失败，使用默认联系人数据");
        return contact_manager_init();
    }
    
    // 读取联系人数量
    size_t count = 0;
    err = nvs_get_u32(nvs_handle, CONTACT_NVS_COUNT_KEY, (uint32_t*)&count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取联系人数量失败，使用默认联系人数据");
        nvs_close(nvs_handle);
        return contact_manager_init();
    }
    
    if (count == 0 || count > 1000) {
        ESP_LOGW(TAG, "联系人数量无效: %zu，使用默认联系人数据", count);
        nvs_close(nvs_handle);
        return contact_manager_init();
    }
    
    ESP_LOGI(TAG, "Flash中存储了 %zu 个联系人", count);
    
    // 如果已经初始化，先释放旧的内存
    if (g_contacts != NULL) {
        contact_manager_deinit();
    }
    
    // 分配PSRAM内存用于存储联系人数据
    g_contacts = (contact_t *)heap_caps_malloc(count * sizeof(contact_t), MALLOC_CAP_SPIRAM);
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "分配联系人内存失败");
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }
    
    // 初始化联系人容量和数量
    g_contact_capacity = count;
    g_contact_count = 0;
    
    // 读取联系人数据
    size_t required_size = count * sizeof(contact_t);
    err = nvs_get_blob(nvs_handle, CONTACT_NVS_KEY, g_contacts, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取联系人数据失败: %s", esp_err_to_name(err));
        heap_caps_free(g_contacts);
        g_contacts = NULL;
        g_contact_count = 0;
        g_contact_capacity = 0;
        nvs_close(nvs_handle);
        return contact_manager_init();
    }
    
    g_contact_count = count;
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "从Flash加载通讯录成功，共加载了 %" PRIu32 " 个联系人", (uint32_t)g_contact_count);
    
    // 打印前5个联系人用于调试
    for (size_t i = 0; i < g_contact_count && i < 9; i++) {
        ESP_LOGI(TAG, "联系人 %zu: %s - %s", i + 1, g_contacts[i].name, g_contacts[i].phone);
    }
    
    return ESP_OK;
}

esp_err_t contact_manager_save_to_flash(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    ESP_LOGI(TAG, "保存通讯录到Flash");
    
    // 检查是否已初始化
    if (g_contacts == NULL || g_contact_count == 0) {
        ESP_LOGE(TAG, "联系人管理器未初始化或无联系人数据");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 打开NVS命名空间
    err = nvs_open(CONTACT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS命名空间失败");
        return err;
    }
    
    // 保存联系人数量
    err = nvs_set_u32(nvs_handle, CONTACT_NVS_COUNT_KEY, (uint32_t)g_contact_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存联系人数量失败");
        nvs_close(nvs_handle);
        return err;
    }
    
    // 保存联系人数据
    err = nvs_set_blob(nvs_handle, CONTACT_NVS_KEY, g_contacts, g_contact_count * sizeof(contact_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存联系人数据失败");
        nvs_close(nvs_handle);
        return err;
    }
    
    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "提交NVS更改失败");
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "保存通讯录到Flash成功，共保存了 %" PRIu32 " 个联系人", (uint32_t)g_contact_count);
    return ESP_OK;
}

esp_err_t contact_manager_update(const contact_t *contacts, size_t count)
{
    if (contacts == NULL || count == 0) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "更新通讯录，新联系人数量: %zu", count);
    
    // 释放旧的内存
    if (g_contacts != NULL) {
        heap_caps_free(g_contacts);
        g_contacts = NULL;
    }
    
    // 分配新的PSRAM内存
    g_contacts = (contact_t *)heap_caps_malloc(count * sizeof(contact_t), MALLOC_CAP_SPIRAM);
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "分配联系人内存失败");
        g_contact_count = 0;
        g_contact_capacity = 0;
        return ESP_ERR_NO_MEM;
    }
    
    // 复制联系人数据
    memcpy(g_contacts, contacts, count * sizeof(contact_t));
    g_contact_count = count;
    g_contact_capacity = count;
    
    ESP_LOGI(TAG, "PSRAM通讯录更新成功，共 %" PRIu32 " 个联系人", (uint32_t)g_contact_count);
    
    // 保存到Flash
    esp_err_t err = contact_manager_save_to_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存通讯录到Flash失败");
        return err;
    }
    
    return ESP_OK;
}

size_t contact_manager_get_count(void)
{
    return g_contact_count;
}

esp_err_t contact_manager_get_all(const contact_t **contacts, size_t *count)
{
    if (contacts == NULL || count == NULL) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查是否已初始化
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "联系人管理器未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    *contacts = g_contacts;
    *count = g_contact_count;
    
    return ESP_OK;
}

esp_err_t contact_manager_add_or_update(const char *name, const char *phone)
{
    if (name == NULL || phone == NULL) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查是否已初始化
    if (g_contacts == NULL) {
        ESP_LOGE(TAG, "联系人管理器未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 遍历联系人数组查找匹配的姓名
    for (size_t i = 0; i < g_contact_count; i++) {
        if (strcmp(g_contacts[i].name, name) == 0) {
            // 找到匹配的联系人，更新电话号码
            strncpy(g_contacts[i].phone, phone, MAX_PHONE_LENGTH - 1);
            g_contacts[i].phone[MAX_PHONE_LENGTH - 1] = '\0';
            ESP_LOGI(TAG, "更新联系人: %s, 新电话: %s", name, phone);
            
            // 保存到Flash
            esp_err_t err = contact_manager_save_to_flash();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "保存通讯录到Flash失败");
                return err;
            }
            
            return ESP_OK;
        }
    }
    
    // 未找到匹配的联系人，需要添加新联系人
    ESP_LOGI(TAG, "未找到联系人 %s，准备添加新联系人", name);
    
    // 检查是否需要扩容
    if (g_contact_count >= g_contact_capacity) {
        size_t new_capacity = g_contact_capacity + 10;
        contact_t *new_contacts = (contact_t *)heap_caps_realloc(g_contacts, new_capacity * sizeof(contact_t), MALLOC_CAP_SPIRAM);
        if (new_contacts == NULL) {
            ESP_LOGE(TAG, "扩容联系人内存失败");
            return ESP_ERR_NO_MEM;
        }
        g_contacts = new_contacts;
        g_contact_capacity = new_capacity;
        ESP_LOGI(TAG, "联系人容量扩容到: %zu", new_capacity);
    }
    
    // 添加新联系人
    strncpy(g_contacts[g_contact_count].name, name, MAX_NAME_LENGTH - 1);
    g_contacts[g_contact_count].name[MAX_NAME_LENGTH - 1] = '\0';
    strncpy(g_contacts[g_contact_count].phone, phone, MAX_PHONE_LENGTH - 1);
    g_contacts[g_contact_count].phone[MAX_PHONE_LENGTH - 1] = '\0';
    g_contact_count++;
    
    ESP_LOGI(TAG, "添加新联系人: %s, 电话: %s, 当前联系人总数: %zu", name, phone, g_contact_count);
    
    // 保存到Flash
    esp_err_t err = contact_manager_save_to_flash();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存通讯录到Flash失败");
        return err;
    }
    
    return ESP_OK;
}