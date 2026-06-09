/* HTTP OTA 静态库头文件
 * 对外暴露 http_get, http_post, ota_check_g 三个函数
 * 基于mbedTLS实现HTTPS通信
 */

#ifndef HTTP_OTA_H
#define HTTP_OTA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP GET请求函数
 * @param url 请求URL
 * @param response_data 响应数据存储缓冲区
 * @param response_size 响应数据大小指针，输入时为缓冲区大小，输出时为实际数据大小
 * @return 0成功，负数失败
 */
int http_get(const char *url, char *response_data, size_t *response_size);

/**
 * HTTP POST请求函数
 * @param url 请求URL
 * @param post_data POST请求数据，如果为NULL则使用URL中的查询参数作为POST数据
 * @param response_data 响应数据存储缓冲区
 * @param response_size 响应数据大小指针，输入时为缓冲区大小，输出时为实际数据大小
 * @return 0成功，负数失败
 */
int http_post(const char *url, const char *post_data, char *response_data, size_t *response_size);

/**
 * OTA检查全局函数（封装版）
 * 使用PSRAM启动任务执行OTA检查，阻塞等待结果
 * @param sn 设备序列号
 * @param version 当前固件版本号
 * @param response_data 响应数据存储缓冲区（由调用者提供）
 * @param response_size 响应数据大小指针，输入时为缓冲区大小，输出时为实际数据大小
 * @return 0成功，负数失败
 */
int ota_check_g(const char *sn, const char *version, char *response_data, size_t *response_size);

#ifdef __cplusplus
}
#endif

#endif // HTTP_OTA_H
