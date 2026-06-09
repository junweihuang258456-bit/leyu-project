#ifndef XGB_1815_H
#define XGB_1815_H

#include "driver/gptimer.h"
#include "driver/gpio.h"

#define XGB_1815_DATA_GPIO GPIO_NUM_12
#define XGB_1815_EN_GPIO  GPIO_NUM_5
#define XGB_1815_NUM_LEDS 9 // 级联的芯片数量

void xgb_1815_set_colour_array(char *colour_array, int length, uint8_t level);
void xgb_1815_set_colour_change(uint8_t colour[], int length, uint8_t level);
void xgb_1815_flush(void);
void xgb_1815_init(void);

#endif // XGB_1815_H