/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX_INTERCEPT_RULES 32
#define MAX_PHONE_NUMBER_LENGTH 32

typedef struct {
    char phone_number[MAX_PHONE_NUMBER_LENGTH];
    bool is_intercepted;
} intercept_rule_t;

typedef enum {
    CALL_STATE_IDLE = 0,
    CALL_STATE_RINGING,
    CALL_STATE_IN_CALL,
    CALL_STATE_HANGING_UP
} call_state_t;

typedef struct {
    char caller_number[MAX_PHONE_NUMBER_LENGTH];
    call_state_t state;
    TimerHandle_t auto_hangup_timer;
} incoming_call_info_t;

esp_err_t call_manager_init(void);
void call_manager_deinit(void);

esp_err_t call_manager_add_intercept_rule(const char *phone_number, bool is_intercepted);
esp_err_t call_manager_clear_intercept_rules(void);
bool call_manager_check_intercept(const char *phone_number);

esp_err_t call_manager_handle_incoming_call(const char *caller_number);
esp_err_t call_manager_answer_call(void);
esp_err_t call_manager_reject_call(void);
esp_err_t call_manager_hangup_call(void);
void call_manager_handle_call_ended(void);
void call_manager_process_events(void);

call_state_t call_manager_get_state(void);
const char *call_manager_get_caller_number(void);

// 全局变量，用于指示是否正在播放铃声
extern bool g_is_playing_ringtone;

#ifdef __cplusplus
}
#endif
