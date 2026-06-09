/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "osi/allocator.h"

#include "btc_blufi_prf.h"
#include "btc/btc_task.h"
#include "btc/btc_manage.h"

#include "blufi_int.h"

#include "esp_log.h"
#include "esp_blufi_api.h"
#include "esp_blufi.h"

#include "cJSON.h"
#include "bta/bta_gatt_api.h"

#if (BLUFI_INCLUDED == TRUE)

#if GATT_DYNAMIC_MEMORY == FALSE
tBLUFI_ENV blufi_env;
#else
tBLUFI_ENV *blufi_env_ptr;
#endif

// static functions declare
static void btc_blufi_send_raw_data(uint8_t *data, int data_len)
{
    if (blufi_env.is_connected == false) {
        BTC_TRACE_ERROR("blufi connection has been disconnected \n");
        return;
    }

    if (data == NULL || data_len <= 0) {
        BTC_TRACE_ERROR("invalid data or data_len\n");
        return;
    }

    // 直接通过GATT发送原始数据，不添加BLUFI协议头
    btc_blufi_send_notify(data, data_len);
    
    BTC_TRACE_DEBUG("发送纯JSON数据: %.*s", data_len, data);
}

static void btc_blufi_send_ack(uint8_t seq);

inline void btc_blufi_cb_to_app(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    esp_blufi_event_cb_t btc_blufi_cb = (esp_blufi_event_cb_t)btc_profile_cb_get(BTC_PID_BLUFI);
    if (btc_blufi_cb) {
	btc_blufi_cb(event, param);
    }
}

static uint8_t btc_blufi_profile_init(void)
{
    esp_blufi_callbacks_t *store_p = blufi_env.cbs;

    uint8_t rc;
    if (blufi_env.enabled) {
        BLUFI_TRACE_ERROR("BLUFI already initialized");
        return ESP_BLUFI_ERROR;
    }

    memset(&blufi_env, 0x0, sizeof(blufi_env));
    blufi_env.cbs = store_p;        /* if set callback prior, restore the point */
    blufi_env.frag_size = BLUFI_FRAG_DATA_DEFAULT_LEN;
    rc = esp_blufi_init();
    if(rc != 0 ){
       return rc;
    }

    return ESP_BLUFI_SUCCESS;
}

static uint8_t btc_blufi_profile_deinit(void)
{
    if (!blufi_env.enabled) {
        BTC_TRACE_ERROR("BLUFI already de-initialized");
        return ESP_BLUFI_ERROR;
    }

    esp_blufi_deinit();
    return ESP_BLUFI_SUCCESS;
}

void btc_blufi_send_notify(uint8_t *pkt, int pkt_len)
{
   // 直接发送原始数据，不经过BLUFI协议封装
   // 检查数据是否为WiFi列表JSON格式：改进识别逻辑，避免分片数据误判
   bool is_json_data = false;
   
   // 改进的识别逻辑：检查数据是否来自btc_blufi_send_wifi_list函数
   // 通过检查数据特征来识别WiFi列表JSON，而不是依赖严格的格式要求
   
   // 方法1：检查数据是否包含WiFi列表JSON特征
   if (pkt_len > 20) { // 确保数据长度足够包含JSON特征
       char *data_str = (char *)pkt;
       
       // 检查是否包含WiFi列表JSON特征
       // 放宽条件：不要求以0x0A结尾，因为分片数据可能不包含完整结尾
       if (strstr(data_str, "\"wifis\"") != NULL) {
           is_json_data = true;
           BTC_TRACE_DEBUG("识别为WiFi列表JSON数据（包含wifis字段）");
       }
       
       // 方法2：检查JSON格式特征
       if (!is_json_data && pkt_len > 10 && pkt[0] == '{') {
           // 检查是否包含JSON数组特征
           for (int i = 0; i < pkt_len - 5; i++) {
               if (pkt[i] == '"' && 
                   pkt[i+1] == 'w' && pkt[i+2] == 'i' && pkt[i+3] == 'f' && pkt[i+4] == 'i' && pkt[i+5] == 's') {
                   is_json_data = true;
                   BTC_TRACE_DEBUG("识别为WiFi列表JSON数据（包含wifis字符串）");
                   break;
               }
           }
       }
       
       // 方法3：检查是否来自btc_blufi_send_wifi_list的数据
       // 通过调用栈或数据特征识别
       if (!is_json_data) {
           // 检查数据是否以JSON格式开头且包含SSID字段
           if (pkt[0] == '{' && strstr(data_str, "\"ssid\"") != NULL) {
               is_json_data = true;
               BTC_TRACE_DEBUG("识别为WiFi列表JSON数据（包含ssid字段）");
           }
       }
   }
   
   // 特殊处理：如果数据长度较小但包含JSON特征，也识别为JSON数据
   // 这可能是分片后的数据
   if (!is_json_data && pkt_len > 5 && pkt_len <= 20) {
       char *data_str = (char *)pkt;
       // 检查是否包含JSON片段特征
       if (strstr(data_str, "wifis") != NULL || strstr(data_str, "ssid") != NULL) {
           is_json_data = true;
           BTC_TRACE_DEBUG("识别为WiFi列表JSON数据片段");
       }
   }
   
   if (is_json_data) {
       // 对于WiFi列表JSON数据，直接通过GATT发送，不经过协议封装
       BTC_TRACE_DEBUG("=== Direct WiFi List JSON Transmission ===\n");
       BTC_TRACE_DEBUG("Sending raw WiFi list JSON data, length: %d\n", pkt_len);
       BTC_TRACE_DEBUG("JSON content: %.*s\n", pkt_len, pkt);
       
       // 直接调用GATT发送函数
       if (blufi_env.is_connected) {
           BTA_GATTS_HandleValueIndication(blufi_env.conn_id, blufi_env.handle_char_e2p, 
                                         pkt_len, pkt, false);
       }
   } else {
       // 对于普通BLUFI数据，使用原来的协议封装
       BTC_TRACE_DEBUG("=== BLUFI Protocol Data Transmission ===\n");
       BTC_TRACE_DEBUG("Sending BLUFI protocol data, length: %d\n", pkt_len);
       struct pkt_info pkts;
       pkts.pkt = pkt;
       pkts.pkt_len = pkt_len;
       esp_blufi_send_notify(&pkts);
   }
}

void btc_blufi_report_error(esp_blufi_error_state_t state)
{
    btc_msg_t msg;
    msg.sig = BTC_SIG_API_CB;
    msg.pid = BTC_PID_BLUFI;
    msg.act = ESP_BLUFI_EVENT_REPORT_ERROR;
    esp_blufi_cb_param_t param;
    param.report_error.state = state;
    btc_transfer_context(&msg, &param, sizeof(esp_blufi_cb_param_t), NULL, NULL);
}

void btc_blufi_recv_handler(uint8_t *data, int len)
{
    // 蓝牙数据接收处理函数 - 处理从APP接收到的蓝牙数据
    // 这是BLUFI协议数据接收的入口点，负责解析APP发送的请求
    
    // 打印所有接收到的数据 - 使用ESP_LOGI确保始终输出
    ESP_LOGI("BLUFI_DEBUG", "=== BLUFI Received Data ===");
    ESP_LOGI("BLUFI_DEBUG", "Total length: %d bytes", len);
    
    // 打印16进制格式
    char hex_str[len * 3 + 1];
    hex_str[0] = '\0';
    for (int i = 0; i < len; i++) {
        char temp[4];
        sprintf(temp, "%02X ", data[i]);
        strcat(hex_str, temp);
    }
    ESP_LOGI("BLUFI_DEBUG", "Hex format: %s", hex_str);
    
    // 尝试转换为可打印文本
    char text_str[len + 1];
    text_str[0] = '\0';
    for (int i = 0; i < len; i++) {
        // 只打印可打印ASCII字符
        if (data[i] >= 32 && data[i] <= 126) {
            char temp[2] = {data[i], '\0'};
            strcat(text_str, temp);
        } else {
            strcat(text_str, ".");
        }
    }
    ESP_LOGI("BLUFI_DEBUG", "Text format: %s", text_str);
    ESP_LOGI("BLUFI_DEBUG", "===========================");
    
    // 同时使用BTC_TRACE_DEBUG输出
    BTC_TRACE_DEBUG("=== BLUFI Received Data ===\n");
    BTC_TRACE_DEBUG("Total length: %d bytes\n", len);
    
    // 打印16进制格式
    BTC_TRACE_DEBUG("Hex format: ");
    for (int i = 0; i < len; i++) {
        BTC_TRACE_DEBUG("%02X ", data[i]);
    }
    BTC_TRACE_DEBUG("\n");
    
    // 尝试转换为可打印文本
    BTC_TRACE_DEBUG("Text format: ");
    for (int i = 0; i < len; i++) {
        // 只打印可打印ASCII字符
        if (data[i] >= 32 && data[i] <= 126) {
            BTC_TRACE_DEBUG("%c", data[i]);
        } else {
            BTC_TRACE_DEBUG(".");
        }
    }
    BTC_TRACE_DEBUG("\n");
    BTC_TRACE_DEBUG("===========================\n");
    
    if (len < sizeof(struct blufi_hdr)) {
        BTC_TRACE_ERROR("%s invalid data length: %d", __func__, len);
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    struct blufi_hdr *hdr = (struct blufi_hdr *)data;

    // 检查是否是0x0A结尾的JSON格式数据，如果是则跳过标准BLUFI协议验证
    bool is_json_data = false;
    // 检查整个接收数据是否以0x0A结尾
    if (len > 0 && data[len-1] == 0x0A) {
        // 检查是否包含JSON特征字符，跳过协议头部分
        for (int i = sizeof(struct blufi_hdr); i < len; i++) {
            if (data[i] == '{' || data[i] == '}') {
                is_json_data = true;
                break;
            }
        }
    }

    // 如果是JSON格式数据，则跳过标准BLUFI协议验证和序列号检查
    if (is_json_data) {
        BTC_TRACE_DEBUG("=== JSON Data Processing ===\n");
        ESP_LOGI("BLUFI_DEBUG", "=== JSON Data Processing ===");
        BTC_TRACE_DEBUG("Data ends with 0x0A, trying to parse as JSON\n");
        ESP_LOGI("BLUFI_DEBUG", "Data ends with 0x0A, trying to parse as JSON");
        BTC_TRACE_DEBUG("Data length: %d\n", len);
        ESP_LOGI("BLUFI_DEBUG", "Data length: %d", len);
        
        // 解析JSON格式数据 - 使用整个接收数据，而不是协议头之后的数据
        char *json_str = (char *)malloc(len + 1);
        if (json_str) {
            memcpy(json_str, data, len);
            json_str[len] = '\0'; // 确保字符串以null结尾
            
            BTC_TRACE_DEBUG("Data as string: %s\n", json_str);
            ESP_LOGI("BLUFI_DEBUG", "Data as string: %s", json_str);
            
            // 尝试解析JSON
            cJSON *json_obj = cJSON_Parse(json_str);
            if (json_obj) {
                BTC_TRACE_DEBUG("JSON parsing successful\n");
                ESP_LOGI("BLUFI_DEBUG", "JSON parsing successful");
                BTC_TRACE_DEBUG("Received JSON data: %s\n", json_str);
                ESP_LOGI("BLUFI_DEBUG", "Received JSON data: %s", json_str);
                
                // 检查是否是请求WiFi列表的JSON
                cJSON *getwifilist = cJSON_GetObjectItem(json_obj, "getwifilist");
                if (getwifilist && cJSON_IsString(getwifilist)) {
                    if (strcmp(getwifilist->valuestring, "1") == 0) {
                        BTC_TRACE_DEBUG("JSON request for WiFi list detected\n");
                        ESP_LOGI("BLUFI_DEBUG", "JSON request for WiFi list detected");
                        
                        // 触发WiFi列表请求事件
                        btc_msg_t msg;
                        msg.sig = BTC_SIG_API_CB;
                        msg.pid = BTC_PID_BLUFI;
                        msg.act = ESP_BLUFI_EVENT_GET_WIFI_LIST;
                        btc_transfer_context(&msg, NULL, 0, NULL, NULL);
                        
                        // 释放JSON对象和字符串
                        cJSON_Delete(json_obj);
                        free(json_str);
                        return; // 处理完成，直接返回
                    }
                }
                
                // 检查是否是WiFi连接请求的JSON
                cJSON *ssid = cJSON_GetObjectItem(json_obj, "ssid");
                cJSON *password = cJSON_GetObjectItem(json_obj, "password");
                if (ssid && password && cJSON_IsString(ssid) && cJSON_IsString(password)) {
                    BTC_TRACE_DEBUG("JSON WiFi connection request detected\n");
                    ESP_LOGI("BLUFI_DEBUG", "JSON WiFi connection request detected");
                    BTC_TRACE_DEBUG("SSID: %s, Password: %s\n", ssid->valuestring, password->valuestring);
                    ESP_LOGI("BLUFI_DEBUG", "SSID: %s, Password: %s", ssid->valuestring, password->valuestring);
                    
                    // 触发WiFi连接事件
                    btc_msg_t msg;
                    esp_blufi_cb_param_t param;
                    
                    // 为SSID分配内存并复制内容
                    uint8_t *ssid_buf = (uint8_t *)malloc(strlen(ssid->valuestring) + 1);
                    if (ssid_buf) {
                        strcpy((char *)ssid_buf, ssid->valuestring);
                        
                        // 发送SSID
                        msg.sig = BTC_SIG_API_CB;
                        msg.pid = BTC_PID_BLUFI;
                        msg.act = ESP_BLUFI_EVENT_RECV_STA_SSID;
                        param.sta_ssid.ssid_len = strlen(ssid->valuestring);
                        param.sta_ssid.ssid = ssid_buf;
                        btc_transfer_context(&msg, &param, sizeof(esp_blufi_cb_param_t), btc_blufi_cb_deep_copy, btc_blufi_cb_deep_free);
                    }
                    
                    // 为密码分配内存并复制内容
                    uint8_t *passwd_buf = (uint8_t *)malloc(strlen(password->valuestring) + 1);
                    if (passwd_buf) {
                        strcpy((char *)passwd_buf, password->valuestring);
                        
                        // 发送密码
                        msg.act = ESP_BLUFI_EVENT_RECV_STA_PASSWD;
                        param.sta_passwd.passwd_len = strlen(password->valuestring);
                        param.sta_passwd.passwd = passwd_buf;
                        btc_transfer_context(&msg, &param, sizeof(esp_blufi_cb_param_t), btc_blufi_cb_deep_copy, btc_blufi_cb_deep_free);
                    }
                    
                    // 重要：在接收完SSID和密码后，自动触发WiFi连接请求
                    // 这是修复WiFi连接不继续执行的关键
                    BTC_TRACE_DEBUG("JSON processing: Triggering WiFi connection request after receiving SSID and password\n");
                    ESP_LOGI("BLUFI_DEBUG", "JSON processing: Auto-triggering WiFi connection request");
                    
                    // 发送WiFi连接请求事件
                    msg.sig = BTC_SIG_API_CB;
                    msg.pid = BTC_PID_BLUFI;
                    msg.act = ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP;
                    btc_transfer_context(&msg, NULL, 0, NULL, NULL);
                    
                    // 释放JSON对象和字符串
                    cJSON_Delete(json_obj);
                    free(json_str);
                    return; // 处理完成，直接返回
                }
                
                // 释放JSON对象
                cJSON_Delete(json_obj);
            } else {
                BTC_TRACE_DEBUG("JSON parsing failed\n");
                ESP_LOGI("BLUFI_DEBUG", "JSON parsing failed");
            }
            free(json_str);
        }
        return; // JSON数据处理完成，直接返回
    }

    // 如果不是JSON格式数据，则进行标准的BLUFI协议长度验证
    int target_data_len;

    if (BLUFI_FC_IS_CHECK(hdr->fc)) {
        target_data_len = hdr->data_len + 4 + 2; // Data + (Type + Frame Control + Sequence Number + Data Length) + Checksum
    } else {
        target_data_len = hdr->data_len + 4; // Data + (Type + Frame Control + Sequence Number + Data Length)
    }

    if (len != target_data_len) {
        BTC_TRACE_ERROR("%s: Invalid data length: %d, expected: %d", __func__, len, target_data_len);
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint16_t checksum, checksum_pkt;
    int ret;

    // 检查序列号是否正确，防止数据包乱序或重复
    if (hdr->seq != blufi_env.recv_seq) {
        BTC_TRACE_ERROR("%s seq %d is not expect %d\n", __func__, hdr->seq, blufi_env.recv_seq + 1);
        btc_blufi_report_error(ESP_BLUFI_SEQUENCE_ERROR);
        return;
    }

    blufi_env.recv_seq++;

    // 第一步：解密数据（如果数据被加密）
    if (BLUFI_FC_IS_ENC(hdr->fc)
            && (blufi_env.cbs && blufi_env.cbs->decrypt_func)) {
        ret = blufi_env.cbs->decrypt_func(hdr->seq, hdr->data, hdr->data_len);
        if (ret != hdr->data_len) { /* enc must be success and enc len must equal to plain len */
            BTC_TRACE_ERROR("%s decrypt error %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_DECRYPT_ERROR);
            return;
        }
    }

    // 第二步：验证校验和（如果启用了校验和功能）
    if (BLUFI_FC_IS_CHECK(hdr->fc)
            && (blufi_env.cbs && blufi_env.cbs->checksum_func)) {
        checksum = blufi_env.cbs->checksum_func(hdr->seq, &hdr->seq, hdr->data_len + 2);
        checksum_pkt = hdr->data[hdr->data_len] | (((uint16_t) hdr->data[hdr->data_len + 1]) << 8);
        if (checksum != checksum_pkt) {
            BTC_TRACE_ERROR("%s checksum error %04x, pkt %04x\n", __func__, checksum, checksum_pkt);
            btc_blufi_report_error(ESP_BLUFI_CHECKSUM_ERROR);
            return;
        }
    }

    // 如果对方请求ACK确认，则发送ACK响应
    if (BLUFI_FC_IS_REQ_ACK(hdr->fc)) {
        btc_blufi_send_ack(hdr->seq);
    }

    // 处理分片数据包（大数据会被分成多个小包传输）
    if (BLUFI_FC_IS_FRAG(hdr->fc)) {
        if (blufi_env.offset == 0) {
            /*
            blufi_env.aggr_buf should be NULL if blufi_env.offset is 0.
            It is possible that the process of sending fragment packet
            has not been completed
            */
            // 初始化分片数据缓冲区，用于重组分片数据
            if (blufi_env.aggr_buf) {
                BTC_TRACE_ERROR("%s msg error, blufi_env.aggr_buf is not freed\n", __func__);
                btc_blufi_report_error(ESP_BLUFI_MSG_STATE_ERROR);
                return;
            }
            // 从数据包中获取总数据长度
            blufi_env.total_len = hdr->data[0] | (((uint16_t) hdr->data[1]) << 8);
            blufi_env.aggr_buf = osi_malloc(blufi_env.total_len);
            if (blufi_env.aggr_buf == NULL) {
                BTC_TRACE_ERROR("%s no mem, len %d\n", __func__, blufi_env.total_len);
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
        }
        // 将当前分片数据复制到聚合缓冲区
        if (blufi_env.offset + hdr->data_len  - 2 <= blufi_env.total_len){
            memcpy(blufi_env.aggr_buf + blufi_env.offset, hdr->data + 2, hdr->data_len  - 2);
            blufi_env.offset += (hdr->data_len - 2);
        } else {
            BTC_TRACE_ERROR("%s payload is longer than packet length, len %d \n", __func__, blufi_env.total_len);
            btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
            return;
        }

    } else {
        // 处理非分片数据包或最后一个分片数据包
        if (blufi_env.offset > 0) {   /* if previous pkt is frag */
            /* blufi_env.aggr_buf should not be NULL */
            if (blufi_env.aggr_buf == NULL) {
                BTC_TRACE_ERROR("%s buffer is NULL\n", __func__);
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
            /* payload length should be equal to total_len */
            if ((blufi_env.offset + hdr->data_len) != blufi_env.total_len) {
                BTC_TRACE_ERROR("%s payload is longer than packet length, len %d \n", __func__, blufi_env.total_len);
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }
            // 复制最后一个分片的数据到聚合缓冲区
            memcpy(blufi_env.aggr_buf + blufi_env.offset, hdr->data, hdr->data_len);

            // 将完整数据传递给协议处理器进行解析
            btc_blufi_protocol_handler(hdr->type, blufi_env.aggr_buf, blufi_env.total_len);
            blufi_env.offset = 0;
            osi_free(blufi_env.aggr_buf);
            blufi_env.aggr_buf = NULL;
        } else {
            // 处理单个完整数据包（非分片）
            btc_blufi_protocol_handler(hdr->type, hdr->data, hdr->data_len);
            blufi_env.offset = 0;
        }
    }
}

/**
 * BLUFI协议封装函数 - 添加协议头并发送数据
 * 这是WiFi列表JSON数据发送时添加协议头的地方
 * 协议头结构：
 *   type: 数据类型 (1字节) - 0x11表示WiFi列表数据
 *   fc: 帧控制 (1字节) - 包含加密、校验、方向、分片等信息
 *   seq: 序列号 (1字节) - 用于数据包排序
 *   data_len: 数据长度 (1字节) - 实际数据长度
 *   data[0]: 数据内容 - 可变长度
 * 
 * 对于WiFi列表JSON数据，协议头会添加在JSON数据前面
 * 你看到的乱码 "M" 就是协议头部分：
 *   M (0x4D) - 可能是type字段
 *    (0x14) - fc字段
 *    (0x0E) - seq字段  
 *    (0x01) - data_len字段
 *    (0x03) - 数据内容开始
 */
void btc_blufi_send_encap(uint8_t type, uint8_t *data, int total_data_len)
{
    // 修改：直接发送原始数据，不添加任何协议头
    // 通过增加延迟确保APP按顺序接收数据
    
    if (blufi_env.is_connected == false) {
        BTC_TRACE_ERROR("blufi connection has been disconnected \n");
        return;
    }

    if (data == NULL || total_data_len <= 0) {
        BTC_TRACE_ERROR("invalid data or data_len\n");
        return;
    }

    // 添加调试信息，显示发送的数据类型和长度
    BTC_TRACE_WARNING("BLUFI直接发送原始数据: type=0x%02x, len=%d, 无协议头\n", type, total_data_len);
    
    // 获取当前连接的MTU大小（默认20字节，但可以协商更大的MTU）
    uint16_t mtu_size = blufi_env.frag_size > 0 ? blufi_env.frag_size : 20;
    
    // 如果数据长度小于等于MTU，直接发送
    if (total_data_len <= mtu_size) {
        BTC_TRACE_DEBUG("发送单包数据，无需分片\n");
        btc_blufi_send_notify(data, total_data_len);
    } else {
        // 数据分片发送
    int total_frags = (total_data_len + mtu_size - 1) / mtu_size;
    BTC_TRACE_DEBUG("数据需要分片发送: 总分片数=%d, 每片大小=%d\n", total_frags, mtu_size);
    BTC_TRACE_DEBUG("原始数据总长度: %d字节\n", total_data_len);
    
    int offset = 0;
    int remaining = total_data_len;
    
    for (int frag_index = 0; frag_index < total_frags; frag_index++) {
        int chunk_size = (remaining > mtu_size) ? mtu_size : remaining;
        
        // 检查最后一个分片是否包含换行符
        if (frag_index == total_frags - 1) {
            // 最后一个分片，检查是否包含换行符
            if (chunk_size > 0 && data[offset + chunk_size - 1] != '\n') {
                BTC_TRACE_WARNING("警告: 最后一个分片不包含换行符! 数据结尾: 0x%02x\n", data[offset + chunk_size - 1]);
            } else {
                BTC_TRACE_DEBUG("最后一个分片包含换行符\n");
            }
        }
        
        // 直接发送原始数据分片，不添加任何协议头
        BTC_TRACE_DEBUG("发送分片 %d/%d, 大小=%d\n", frag_index + 1, total_frags, chunk_size);
        btc_blufi_send_notify(data + offset, chunk_size);
        
        offset += chunk_size;
        remaining -= chunk_size;
        
        // 增加延迟确保APP按顺序接收
        // 根据分片索引增加延迟：前几个分片延迟较小，后续分片延迟逐渐增加
        int delay_ms = 260 + (frag_index * 5); // 基础160ms + 每片增加20ms
        if (delay_ms > 280) delay_ms = 280; // 最大延迟190ms
        
        BTC_TRACE_DEBUG("分片 %d/%d 发送完成，等待 %dms 后发送下一片\n", 
                      frag_index + 1, total_frags, delay_ms);
        
        if (frag_index < total_frags - 1) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
        
        BTC_TRACE_DEBUG("所有分片发送完成，总分片数: %d\n", total_frags);
    }
}

static void btc_blufi_wifi_conn_report(uint8_t opmode, uint8_t sta_conn_state, uint8_t softap_conn_num, esp_blufi_extra_info_t *info, int info_len)
{
    uint8_t type;
    uint8_t *data;
    int data_len;
    uint8_t *p;

    data_len = info_len + 3;
    p = data = osi_malloc(data_len);
    if (data == NULL) {
        BTC_TRACE_ERROR("%s no mem\n", __func__);
        return;
    }

    type = BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_TYPE_DATA_SUBTYPE_WIFI_REP);
    *p++ = opmode;
    *p++ = sta_conn_state;
    *p++ = softap_conn_num;

    if (info) {
        if (info->sta_bssid_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_BSSID;
            *p++ = 6;
            memcpy(p, info->sta_bssid, 6);
            p += 6;
        }
        if (info->sta_ssid) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_SSID;
            *p++ = info->sta_ssid_len;
            memcpy(p, info->sta_ssid, info->sta_ssid_len);
            p += info->sta_ssid_len;
        }
        if (info->sta_passwd) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_PASSWD;
            *p++ = info->sta_passwd_len;
            memcpy(p, info->sta_passwd, info->sta_passwd_len);
            p += info->sta_passwd_len;
        }
        if (info->softap_ssid) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_SOFTAP_SSID;
            *p++ = info->softap_ssid_len;
            memcpy(p, info->softap_ssid, info->softap_ssid_len);
            p += info->softap_ssid_len;
        }
        if (info->softap_passwd) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_SOFTAP_PASSWD;
            *p++ = info->softap_passwd_len;
            memcpy(p, info->softap_passwd, info->softap_passwd_len);
            p += info->softap_passwd_len;
        }
        if (info->softap_authmode_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_SOFTAP_AUTH_MODE;
            *p++ = 1;
            *p++ = info->softap_authmode;
        }
        if (info->softap_max_conn_num_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_SOFTAP_MAX_CONN_NUM;
            *p++ = 1;
            *p++ = info->softap_max_conn_num;
        }
        if (info->softap_channel_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_SOFTAP_CHANNEL;
            *p++ = 1;
            *p++ = info->softap_channel;
        }
        if (info->sta_max_conn_retry_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_MAX_CONN_RETRY;
            *p++ = 1;
            *p++ = info->sta_max_conn_retry;
        }
        if (info->sta_conn_end_reason_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_CONN_END_REASON;
            *p++ = 1;
            *p++ = info->sta_conn_end_reason;
        }
        if (info->sta_conn_rssi_set) {
            *p++ = BLUFI_TYPE_DATA_SUBTYPE_STA_CONN_RSSI;
            *p++ = 1;
            *p++ = info->sta_conn_rssi;
        }
    }
    if (p - data > data_len) {
        BTC_TRACE_ERROR("%s len error %d %d\n", __func__, (int)(p - data), data_len);
    }

    btc_blufi_send_encap(type, data, data_len);
    osi_free(data);
}

/**
 * WiFi列表发送函数 - 创建JSON格式的WiFi列表并直接发送（无协议头）
 * 这个函数负责将WiFi扫描结果转换为JSON格式并直接发送给手机APP
 * 
 * 修改说明：取消协议头包裹，直接发送纯JSON数据
 * 数据发送流程：
 * 1. 创建JSON对象 {"wifis": [...]}
 * 2. 为每个WiFi AP创建 {"ssid": "名称", "rssi": 信号强度}
 * 3. 将JSON转换为字符串
 * 4. 直接调用btc_blufi_send_raw_data()发送纯JSON数据
 * 5. 通过蓝牙GATT发送给手机
 * 
 * 修改后APP将收到干净的JSON数据，不再有协议头乱码
 */
void btc_blufi_send_wifi_list(uint16_t apCount, esp_blufi_ap_record_t *list)
{
    uint8_t *data;
    int data_len;
    uint8_t *p;
    
    // 创建JSON格式的WiFi列表数据
    // 根对象包含一个"wifis"数组
    cJSON *root = cJSON_CreateObject();
    cJSON *wifis = cJSON_CreateArray();
    
    if (root == NULL || wifis == NULL) {
        BTC_TRACE_ERROR("cJSON create error\n");
        if (root) cJSON_Delete(root);
        if (wifis) cJSON_Delete(wifis);
        return;
    }
    
    cJSON_AddItemToObject(root, "wifis", wifis);
    
    // 添加每个AP的信息到JSON数组
    // 每个WiFi AP表示为：{"ssid": "名称", "rssi": 信号强度}
    for (int i = 0; i < apCount; ++i)
    {
        cJSON *ap = cJSON_CreateObject();
        if (ap == NULL) {
            BTC_TRACE_ERROR("cJSON create ap error\n");
            continue;
        }
        
        cJSON_AddStringToObject(ap, "ssid", (const char *)list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", list[i].rssi);
        cJSON_AddItemToArray(wifis, ap);
    }
    
    // 将JSON对象转换为字符串
    // 使用无格式化的JSON字符串以减少数据量
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        BTC_TRACE_ERROR("cJSON print error\n");
        cJSON_Delete(root);
        return;
    }
    
    // 添加换行符作为结束标记
    uint32_t json_len = strlen(json_str);
    uint32_t malloc_size = json_len + 2; // 字符串 + 换行符 + 空字符
    
    p = data = osi_malloc(malloc_size);
    if (data == NULL) {
        BTC_TRACE_ERROR("malloc error\n");
        cJSON_free(json_str);
        cJSON_Delete(root);
        return;
    }
    
    // 复制JSON字符串并添加换行符
    memcpy(data, json_str, json_len);
    data[json_len] = '\n'; // 添加换行符作为结束标记
    data[json_len + 1] = '\0'; // 字符串结束
    
    data_len = json_len + 1; // 实际数据长度（包含换行符）
    
    BTC_TRACE_DEBUG("Sending WiFi list as JSON (no header): %s", json_str);
    BTC_TRACE_DEBUG("JSON数据总长度: %d字节 (包含换行符0x0A)", data_len);
    
    // 直接发送纯JSON数据，不添加协议头
    // 修改：取消协议头包裹，直接发送原始数据
    BTC_TRACE_DEBUG("=== 开始发送WiFi列表JSON数据 ===");
    BTC_TRACE_DEBUG("数据长度: %d字节", data_len);
    BTC_TRACE_DEBUG("数据结尾字节: 0x%02x", data[data_len-1]);
    BTC_TRACE_DEBUG("预期结尾: 0x0A (换行符)");
    
    // 重要：检查数据是否以换行符结尾
    if (data_len > 0 && data[data_len-1] == 0x0A) {
        BTC_TRACE_DEBUG("数据正确以换行符0x0A结尾");
    } else {
        BTC_TRACE_ERROR("错误：数据不以换行符结尾！实际结尾: 0x%02x", data_len > 0 ? data[data_len-1] : 0xFF);
        BTC_TRACE_ERROR("这可能是内存分配或数据复制错误");
        
        // 重新添加换行符，而不是强制替换
        if (data_len > 0) {
            // 重新分配内存以包含换行符
            uint32_t new_data_len = data_len + 1;
            uint8_t *new_data = osi_malloc(new_data_len + 1); // 包含空字符
            if (new_data) {
                memcpy(new_data, data, data_len);
                new_data[data_len] = '\n'; // 添加换行符
                new_data[data_len + 1] = '\0'; // 字符串结束
                
                // 释放旧数据，使用新数据
                osi_free(data);
                data = new_data;
                data_len = new_data_len;
                
                BTC_TRACE_DEBUG("已重新添加换行符0x0A，新数据长度: %d字节", data_len);
            } else {
                BTC_TRACE_ERROR("内存分配失败，使用原始数据发送");
            }
        }
    }
    
    btc_blufi_send_raw_data(data, data_len);
    
    // 清理资源
    osi_free(data);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

static void btc_blufi_send_ack(uint8_t seq)
{
    uint8_t type;
    uint8_t data;

    type = BLUFI_BUILD_TYPE(BLUFI_TYPE_CTRL, BLUFI_TYPE_CTRL_SUBTYPE_ACK);
    data = seq;

    btc_blufi_send_encap(type, &data, 1);
}
static void btc_blufi_send_error_info(uint8_t state)
{
    uint8_t type;
    uint8_t *data;
    int data_len;
    uint8_t *p;

    data_len = 1;
    p = data = osi_malloc(data_len);
    if (data == NULL) {
        BTC_TRACE_ERROR("%s no mem\n", __func__);
        return;
    }

    type = BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_TYPE_DATA_SUBTYPE_ERROR_INFO);
    *p++ = state;
    if (p - data > data_len) {
        BTC_TRACE_ERROR("%s len error %d %d\n", __func__, (int)(p - data), data_len);
    }

    btc_blufi_send_encap(type, data, data_len);
    osi_free(data);
}

static void btc_blufi_send_custom_data(uint8_t *value, uint32_t value_len)
{
    if(value == NULL || value_len == 0) {
        BTC_TRACE_ERROR("%s value or value len error", __func__);
        return;
    }
    uint8_t *data = osi_malloc(value_len);
    if (data == NULL) {
        BTC_TRACE_ERROR("%s mem malloc error", __func__);
        return;
    }
    uint8_t type = BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_TYPE_DATA_SUBTYPE_CUSTOM_DATA);
    memcpy(data, value, value_len);
    btc_blufi_send_encap(type, data, value_len);
    osi_free(data);
}

void btc_blufi_cb_deep_copy(btc_msg_t *msg, void *p_dest, void *p_src)
{
    esp_blufi_cb_param_t *dst = (esp_blufi_cb_param_t *) p_dest;
    esp_blufi_cb_param_t *src = (esp_blufi_cb_param_t *) p_src;

    switch (msg->act) {
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        dst->sta_ssid.ssid = osi_malloc(src->sta_ssid.ssid_len);
        if (dst->sta_ssid.ssid == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->sta_ssid.ssid, src->sta_ssid.ssid, src->sta_ssid.ssid_len);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        dst->sta_passwd.passwd = osi_malloc(src->sta_passwd.passwd_len);
        if (dst->sta_passwd.passwd == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->sta_passwd.passwd, src->sta_passwd.passwd, src->sta_passwd.passwd_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        dst->softap_ssid.ssid = osi_malloc(src->softap_ssid.ssid_len);
        if (dst->softap_ssid.ssid == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->softap_ssid.ssid, src->softap_ssid.ssid, src->softap_ssid.ssid_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        dst->softap_passwd.passwd = osi_malloc(src->softap_passwd.passwd_len);
        if (dst->softap_passwd.passwd == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->softap_passwd.passwd, src->softap_passwd.passwd, src->softap_passwd.passwd_len);
        break;
    case ESP_BLUFI_EVENT_RECV_USERNAME:
        dst->username.name = osi_malloc(src->username.name_len);
        if (dst->username.name == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->username.name, src->username.name, src->username.name_len);
        break;
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
        dst->ca.cert = osi_malloc(src->ca.cert_len);
        if (dst->ca.cert == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->ca.cert, src->ca.cert, src->ca.cert_len);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        dst->client_cert.cert = osi_malloc(src->client_cert.cert_len);
        if (dst->client_cert.cert == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->client_cert.cert, src->client_cert.cert, src->client_cert.cert_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        dst->server_cert.cert = osi_malloc(src->server_cert.cert_len);
        if (dst->server_cert.cert == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->server_cert.cert, src->server_cert.cert, src->server_cert.cert_len);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
         dst->client_pkey.pkey = osi_malloc(src->client_pkey.pkey_len);
        if (dst->client_pkey.pkey == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->client_pkey.pkey, src->client_pkey.pkey, src->client_pkey.pkey_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
         dst->server_pkey.pkey = osi_malloc(src->server_pkey.pkey_len);
        if (dst->server_pkey.pkey == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
        }
        memcpy(dst->server_pkey.pkey, src->server_pkey.pkey, src->server_pkey.pkey_len);
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
         dst->custom_data.data = osi_malloc(src->custom_data.data_len);
        if (dst->custom_data.data == NULL) {
            BTC_TRACE_ERROR("%s %d no mem\n", __func__, msg->act);
            break;
        }
        memcpy(dst->custom_data.data, src->custom_data.data, src->custom_data.data_len);
        break;
    default:
        break;
    }
}

void btc_blufi_cb_deep_free(btc_msg_t *msg)
{
    esp_blufi_cb_param_t *param = (esp_blufi_cb_param_t *)msg->arg;

    switch (msg->act) {
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        // Note: Don't free memory here as application layer is responsible for freeing it
        // osi_free(param->sta_ssid.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        // Note: Don't free memory here as application layer is responsible for freeing it
        // osi_free(param->sta_passwd.passwd);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        osi_free(param->softap_ssid.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        osi_free(param->softap_passwd.passwd);
        break;
    case ESP_BLUFI_EVENT_RECV_USERNAME:
        osi_free(param->username.name);
        break;
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
        osi_free(param->ca.cert);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        osi_free(param->client_cert.cert);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        osi_free(param->server_cert.cert);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        osi_free(param->client_pkey.pkey);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        osi_free(param->server_pkey.pkey);
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        osi_free(param->custom_data.data);
        break;
    default:
        break;
    }
}

void btc_blufi_cb_handler(btc_msg_t *msg)
{
    esp_blufi_cb_param_t *param = (esp_blufi_cb_param_t *)msg->arg;

    switch (msg->act) {
    case ESP_BLUFI_EVENT_INIT_FINISH: {
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_INIT_FINISH, param);
        break;
    }
    case ESP_BLUFI_EVENT_DEINIT_FINISH: {
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_DEINIT_FINISH, param);
        break;
    }
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_BLE_CONNECT, param);
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_BLE_DISCONNECT, param);
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_SET_WIFI_OPMODE, param);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP, NULL);
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP, NULL);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_GET_WIFI_STATUS, NULL);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_GET_WIFI_LIST, NULL);
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_DEAUTHENTICATE_STA, NULL);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_STA_BSSID, param);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_STA_SSID, param);
        // 注意：不要在这里释放内存，因为应用层可能还在使用
        // 应用层在处理完事件后应该负责释放内存
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_STA_PASSWD, param);
        // 注意：不要在这里释放内存，因为应用层可能还在使用
        // 应用层在处理完事件后应该负责释放内存
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SOFTAP_SSID, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL, param);
        break;
    case ESP_BLUFI_EVENT_RECV_USERNAME:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_USERNAME, param);
        break;
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_CA_CERT, param);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_CLIENT_CERT, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SERVER_CERT, param);
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY, param);
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE, param);
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_REPORT_ERROR, param);
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        btc_blufi_cb_to_app(ESP_BLUFI_EVENT_RECV_CUSTOM_DATA, param);
        break;
    default:
        BTC_TRACE_ERROR("%s UNKNOWN %d\n", __func__, msg->act);
        break;
    }

    // Note: Don't free memory here as application layer may still be using it
    // Memory should be freed by application layer after processing events
    // btc_blufi_cb_deep_free(msg);
}

void btc_blufi_call_deep_copy(btc_msg_t *msg, void *p_dest, void *p_src)
{
    btc_blufi_args_t *dst = (btc_blufi_args_t *) p_dest;
    btc_blufi_args_t *src = (btc_blufi_args_t *) p_src;

    switch (msg->act) {
    case BTC_BLUFI_ACT_SEND_CFG_REPORT: {
        esp_blufi_extra_info_t *src_info = src->wifi_conn_report.extra_info;
        dst->wifi_conn_report.extra_info_len = 0;
        dst->wifi_conn_report.extra_info = NULL;

        if (src_info == NULL) {
            return;
        }

        dst->wifi_conn_report.extra_info = osi_calloc(sizeof(esp_blufi_extra_info_t));
        if (dst->wifi_conn_report.extra_info == NULL) {
            BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
            return;
        }

        if (src_info->sta_bssid_set) {
            memcpy(dst->wifi_conn_report.extra_info->sta_bssid, src_info->sta_bssid, 6);
            dst->wifi_conn_report.extra_info->sta_bssid_set = src_info->sta_bssid_set;
            dst->wifi_conn_report.extra_info_len += (6 + 2);
        }
        if (src_info->sta_ssid) {
            dst->wifi_conn_report.extra_info->sta_ssid = osi_malloc(src_info->sta_ssid_len);
            if (dst->wifi_conn_report.extra_info->sta_ssid) {
                memcpy(dst->wifi_conn_report.extra_info->sta_ssid, src_info->sta_ssid, src_info->sta_ssid_len);
                dst->wifi_conn_report.extra_info->sta_ssid_len = src_info->sta_ssid_len;
                dst->wifi_conn_report.extra_info_len += (dst->wifi_conn_report.extra_info->sta_ssid_len + 2);
            } else {
                BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
                return;
            }
        }
        if (src_info->sta_passwd) {
            dst->wifi_conn_report.extra_info->sta_passwd = osi_malloc(src_info->sta_passwd_len);
            if (dst->wifi_conn_report.extra_info->sta_passwd) {
                memcpy(dst->wifi_conn_report.extra_info->sta_passwd, src_info->sta_passwd, src_info->sta_passwd_len);
                dst->wifi_conn_report.extra_info->sta_passwd_len = src_info->sta_passwd_len;
                dst->wifi_conn_report.extra_info_len += (dst->wifi_conn_report.extra_info->sta_passwd_len + 2);
            } else {
                 BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
                 return;
            }
        }
        if (src_info->softap_ssid) {
            dst->wifi_conn_report.extra_info->softap_ssid = osi_malloc(src_info->softap_ssid_len);
            if (dst->wifi_conn_report.extra_info->softap_ssid) {
                memcpy(dst->wifi_conn_report.extra_info->softap_ssid, src_info->softap_ssid, src_info->softap_ssid_len);
                dst->wifi_conn_report.extra_info->softap_ssid_len = src_info->softap_ssid_len;
                dst->wifi_conn_report.extra_info_len += (dst->wifi_conn_report.extra_info->softap_ssid_len + 2);
            } else {
                 BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
                 return;
            }
        }
        if (src_info->softap_passwd) {
            dst->wifi_conn_report.extra_info->softap_passwd = osi_malloc(src_info->softap_passwd_len);
            if (dst->wifi_conn_report.extra_info->softap_passwd) {
                memcpy(dst->wifi_conn_report.extra_info->softap_passwd, src_info->softap_passwd, src_info->softap_passwd_len);
                dst->wifi_conn_report.extra_info->softap_passwd_len = src_info->softap_passwd_len;
                dst->wifi_conn_report.extra_info_len += (dst->wifi_conn_report.extra_info->softap_passwd_len + 2);
            } else {
                 BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
                 return;
            }
        }
        if (src_info->softap_authmode_set) {
            dst->wifi_conn_report.extra_info->softap_authmode_set = src_info->softap_authmode_set;
            dst->wifi_conn_report.extra_info->softap_authmode = src_info->softap_authmode;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        if (src_info->softap_max_conn_num_set) {
            dst->wifi_conn_report.extra_info->softap_max_conn_num_set = src_info->softap_max_conn_num_set;
            dst->wifi_conn_report.extra_info->softap_max_conn_num = src_info->softap_max_conn_num;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        if (src_info->softap_channel_set) {
            dst->wifi_conn_report.extra_info->softap_channel_set = src_info->softap_channel_set;
            dst->wifi_conn_report.extra_info->softap_channel = src_info->softap_channel;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        if (src_info->sta_max_conn_retry_set) {
            dst->wifi_conn_report.extra_info->sta_max_conn_retry_set = src_info->sta_max_conn_retry_set;
            dst->wifi_conn_report.extra_info->sta_max_conn_retry = src_info->sta_max_conn_retry;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        if (src_info->sta_conn_end_reason_set) {
            dst->wifi_conn_report.extra_info->sta_conn_end_reason_set = src_info->sta_conn_end_reason_set;
            dst->wifi_conn_report.extra_info->sta_conn_end_reason = src_info->sta_conn_end_reason;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        if (src_info->sta_conn_rssi_set) {
            dst->wifi_conn_report.extra_info->sta_conn_rssi_set = src_info->sta_conn_rssi_set;
            dst->wifi_conn_report.extra_info->sta_conn_rssi = src_info->sta_conn_rssi;
            dst->wifi_conn_report.extra_info_len += (1 + 2);
        }
        break;
    }
    case BTC_BLUFI_ACT_SEND_WIFI_LIST:{
        esp_blufi_ap_record_t *list = src->wifi_list.list;
        src->wifi_list.list = NULL;
        if (list == NULL || src->wifi_list.apCount <= 0) {
            break;
        }
        dst->wifi_list.list = (esp_blufi_ap_record_t *)osi_malloc(sizeof(esp_blufi_ap_record_t) * src->wifi_list.apCount);
        if (dst->wifi_list.list == NULL) {
            BTC_TRACE_ERROR("%s no mem line %d\n", __func__, __LINE__);
            break;
        }
        memcpy(dst->wifi_list.list, list, sizeof(esp_blufi_ap_record_t) * src->wifi_list.apCount);
        break;
    }
    case BTC_BLUFI_ACT_SEND_CUSTOM_DATA:{
        uint8_t *data = src->custom_data.data;
        if(data == NULL) {
            BTC_TRACE_ERROR("custom data is NULL\n");
            break;
        }
        dst->custom_data.data = osi_malloc(src->custom_data.data_len);
        if(dst->custom_data.data == NULL) {
            BTC_TRACE_ERROR("custom data malloc error\n");
            break;
        }
        memcpy(dst->custom_data.data, src->custom_data.data, src->custom_data.data_len);
        break;
    }
    default:
        break;
    }
}

void btc_blufi_call_deep_free(btc_msg_t *msg)
{
    btc_blufi_args_t *arg = (btc_blufi_args_t *)msg->arg;

    switch (msg->act) {
    case BTC_BLUFI_ACT_SEND_CFG_REPORT: {
        esp_blufi_extra_info_t *info = (esp_blufi_extra_info_t *)arg->wifi_conn_report.extra_info;

        if (info == NULL) {
            return;
        }
        if (info->sta_ssid) {
            osi_free(info->sta_ssid);
        }
        if (info->sta_passwd) {
            osi_free(info->sta_passwd);
        }
        if (info->softap_ssid) {
            osi_free(info->softap_ssid);
        }
        if (info->softap_passwd) {
            osi_free(info->softap_passwd);
        }
        osi_free(info);
        break;
    }
    case BTC_BLUFI_ACT_SEND_WIFI_LIST:{
        esp_blufi_ap_record_t *list = (esp_blufi_ap_record_t *)arg->wifi_list.list;
        if (list){
            osi_free(list);
        }
        break;
    }
    case BTC_BLUFI_ACT_SEND_CUSTOM_DATA:{
        uint8_t *data = arg->custom_data.data;
        if(data) {
            osi_free(data);
        }
        break;
    }
    default:
        break;
    }
}

void btc_blufi_call_handler(btc_msg_t *msg)
{
    btc_blufi_args_t *arg = (btc_blufi_args_t *)msg->arg;

    switch (msg->act) {
    case BTC_BLUFI_ACT_INIT:
        btc_blufi_profile_init();
        break;
    case BTC_BLUFI_ACT_DEINIT:
        btc_blufi_profile_deinit();
        break;
    case BTC_BLUFI_ACT_SEND_CFG_REPORT:
        btc_blufi_wifi_conn_report(arg->wifi_conn_report.opmode,
                                   arg->wifi_conn_report.sta_conn_state,
                                   arg->wifi_conn_report.softap_conn_num,
                                   arg->wifi_conn_report.extra_info,
                                   arg->wifi_conn_report.extra_info_len);
        break;
    case BTC_BLUFI_ACT_SEND_WIFI_LIST:{
        btc_blufi_send_wifi_list(arg->wifi_list.apCount, arg->wifi_list.list);
        break;
    }
    case BTC_BLUFI_ACT_SEND_ERR_INFO:
        btc_blufi_send_error_info(arg->blufi_err_infor.state);
        break;
    case BTC_BLUFI_ACT_SEND_CUSTOM_DATA:
        btc_blufi_send_custom_data(arg->custom_data.data, arg->custom_data.data_len);
        break;
    default:
        BTC_TRACE_ERROR("%s UNKNOWN %d\n", __func__, msg->act);
        break;
    }
    btc_blufi_call_deep_free(msg);
}

void btc_blufi_set_callbacks(esp_blufi_callbacks_t *callbacks)
{
    blufi_env.cbs = callbacks;
}

uint16_t btc_blufi_get_version(void)
{
    return BTC_BLUFI_VERSION;
}

#endif  ///BLUFI_INCLUDED == TRUE
