#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define LED_NUM 9 // 级联的芯片数量

extern uint8_t level;
extern uint8_t Mode;

void led_static_display(uint8_t mode, uint8_t level);
void led_display(uint8_t mode);
void led_init(void);

void timer_callback(TimerHandle_t xTimer);

#endif // LED_CONTROL_H