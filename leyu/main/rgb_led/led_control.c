#include "led_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "xgb-1815.h"

uint8_t Mode;
const char led_off_array[LED_NUM] = {'0', '0', '0', '0', '0',
                                     '0', '0', '0', '0'};
const char led_White_array[LED_NUM] = {'W', 'W', 'W', 'W', 'W',
                                       'W', 'W', 'W', 'W'};
const char led_red_array[LED_NUM] = {'R', 'R', 'R', 'R', 'R',
                                     'R', 'R', 'R', 'R'};
const char led_green_array[LED_NUM] = {'G', 'G', 'G', 'G', 'G',
                                       'G', 'G', 'G', 'G'};
const char led_blue_array[LED_NUM] = {'B', 'B', 'B', 'B', 'B',
                                      'B', 'B', 'B', 'B'};
const char led_yellow_array[LED_NUM] = {'Y', 'Y', 'Y', 'Y', 'Y',
                                        'Y', 'Y', 'Y', 'Y'};
const char led_cyan_array[LED_NUM] = {'C', 'C', 'C', 'C', 'C',
                                      'C', 'C', 'C', 'C'};
const char led_purple_array[LED_NUM] = {'P', 'P', 'P', 'P', 'P',
                                        'P', 'P', 'P', 'P'};
const char led_orange_array[LED_NUM] = {'O', 'O', 'O', 'O', 'O',
                                        'O', 'O', 'O', 'O'};
const char led_mixed_array[LED_NUM] = {'R', 'G', 'B', 'Y', 'C',
                                       'P', 'O', 'W', 'R'};
const char led_14_1[LED_NUM] = "R00000000";
const char led_14_2[LED_NUM] = "GR0000000";
const char led_14_3[LED_NUM] = "BGR000000";
const char led_14_4[LED_NUM] = "YBGR00000";
const char led_14_5[LED_NUM] = "CYBGR0000";
const char led_14_6[LED_NUM] = "PCYBGR000";
const char led_14_7[LED_NUM] = "OPCYBGR00";
const char led_14_8[LED_NUM] = "0OPCYBGR0";
const char led_14_9[LED_NUM] = "00OPCYBGR";
const char led_14_10[LED_NUM] = "R00OPCYBG";
const char led_14_11[LED_NUM] = "GR00OPCYB";
const char led_14_12[LED_NUM] = "BGR00OPCY";
const char led_14_13[LED_NUM] = "YBGR00OPC";
const char led_14_14[LED_NUM] = "CYBGR00OP";
const char led_14_15[LED_NUM] = "PCYBGR00O";

void led_static_display(uint8_t mode, uint8_t level) {
  switch (mode) {
  case 0: // 全部关闭
    xgb_1815_set_colour_array(led_off_array, LED_NUM, level);
    break;
  case 1: // 全部白色
    xgb_1815_set_colour_array(led_White_array, LED_NUM, level);
    break;
  case 2:
    xgb_1815_set_colour_array(led_green_array, LED_NUM, level);
    break;
  case 3:
    xgb_1815_set_colour_array(led_yellow_array, LED_NUM, level);
    break;
  case 4:
    xgb_1815_set_colour_array(led_red_array, LED_NUM, level);
    break;
  case 5:
    xgb_1815_set_colour_array(led_mixed_array, LED_NUM, level);
    break;
  case 6: // 全部白色
    xgb_1815_set_colour_array(led_White_array, LED_NUM, level);
    break;
  case 7:
    xgb_1815_set_colour_array(led_blue_array, LED_NUM, level);
    break;
  case 8:
    xgb_1815_set_colour_array(led_purple_array, LED_NUM, level);
    break;
  case 9:
    xgb_1815_set_colour_array(led_orange_array, LED_NUM, level);
    break;
  case 10:
    xgb_1815_set_colour_array(led_cyan_array, LED_NUM, level);
    break;
  case 17:
    xgb_1815_set_colour_array(led_14_1, LED_NUM, level);
    break;
  case 18:
    xgb_1815_set_colour_array(led_14_2, LED_NUM, level);
    break;
  case 19:
    xgb_1815_set_colour_array(led_14_3, LED_NUM, level);
    break;
  case 20:
    xgb_1815_set_colour_array(led_14_4, LED_NUM, level);
    break;
  case 21:
    xgb_1815_set_colour_array(led_14_5, LED_NUM, level);
    break;
  case 22:
    xgb_1815_set_colour_array(led_14_6, LED_NUM, level);
    break;
  case 23:
    xgb_1815_set_colour_array(led_14_7, LED_NUM, level);
    break;
  case 24:
    xgb_1815_set_colour_array(led_14_8, LED_NUM, level);
    break;
  case 25:
    xgb_1815_set_colour_array(led_14_9, LED_NUM, level);
    break;
  case 26:
    xgb_1815_set_colour_array(led_14_10, LED_NUM, level);
    break;
  case 27:
    xgb_1815_set_colour_array(led_14_11, LED_NUM, level);
    break;
  case 28:
    xgb_1815_set_colour_array(led_14_12, LED_NUM, level);
    break;
  case 29:
    xgb_1815_set_colour_array(led_14_13, LED_NUM, level);
    break;
  case 30:
    xgb_1815_set_colour_array(led_14_14, LED_NUM, level);
    break;
  case 31:
    xgb_1815_set_colour_array(led_14_15, LED_NUM, level);
    break;
  default:
    break;
  }
}

uint8_t i_11 = 0;
uint8_t i_12 = 0;
uint8_t i_13 = 0;
uint8_t i_14 = 0;

uint8_t flag_12 = 1;
uint8_t count_12 = 1;

uint8_t level = 2;
void led_display(uint8_t mode) {
  static uint8_t old_mode = 0;

  switch (mode) {
  case 11:
    i_11 = 0;
    break;
  case 12:
    i_12 = 0;
    count_12 = 1;
    flag_12 = 1;
    break;
  case 13:
    i_13 = 0;
    break;
  case 14:
    i_14 = 0;
    break;
  case 15:
    level++;
    if (level >= 5) {
      level = 5;
    }
    mode = old_mode;
    break;
  case 16:
    level--;
    if (level <= 1) {
      level = 1;
    }
    mode = old_mode;
    break;
  default:
    break;
  }

  Mode = mode;
  led_static_display(mode, level);
  old_mode = mode;
}

uint8_t led_list_11[7] = {4, 2, 7, 3, 8, 10, 9};
uint8_t led_list_13[3] = {255, 0, 0};
uint8_t led_list_14_1[6] = {17, 18, 19, 20, 21, 22};
uint8_t led_list_14_2[9] = {23, 24, 25, 26, 27, 28, 29, 30, 31};

void timer_callback(TimerHandle_t xTimer) {
  switch (Mode) {
  case 11:
    led_static_display(led_list_11[i_11 == 6 ? i_11 = 0 : i_11++], level);
    break;
  case 12:
    switch (i_12) {
    case 0:
      if (count_12 <= 6) {
        led_static_display(flag_12 == 1 ? 4 : 0, 5);
        flag_12 == 1 ? flag_12 = 0 : flag_12++;
        count_12++;
      } else if (count_12 <= 9) {
        count_12++;
      } else {
        count_12 = 1;
        i_12 = 1;
        flag_12 = 1;
      }
      break;
    case 1:
      if (count_12 <= 12) {
        led_static_display(flag_12 <= 3 ? 4 : 0, 5);
        flag_12 == 4 ? flag_12 = 1 : flag_12++;
        count_12++;
      } else if (count_12 <= 15) {
        count_12++;
      } else {
        count_12 = 1;
        i_12 = 2;
        flag_12 = 1;
      }
      break;
    case 2:
      if (count_12 <= 6) {
        led_static_display(flag_12 == 1 ? 4 : 0, 5);
        flag_12 == 1 ? flag_12 = 0 : flag_12++;
        count_12++;
      } else if (count_12 <= 13) {
        count_12++;
      } else {
        count_12 = 1;
        i_12 = 0;
        flag_12 = 1;
      }
      break;
    default:
      break;
    }
    break;
  case 13:
    xgb_1815_set_colour_change(led_list_13, LED_NUM, level);
    switch (i_13) {
    case 0:
      if (led_list_13[1] + 10 <= 255) {
        led_list_13[1] += 10;
      } else {
        led_list_13[1] = 255;
        i_13 = 1;
      }
      break;
    case 1:
      if (led_list_13[0] - 10 >= 0) {
        led_list_13[0] -= 10;
      } else {
        led_list_13[0] = 0;
        i_13 = 2;
      }
      break;
    case 2:
      if (led_list_13[2] + 10 <= 255) {
        led_list_13[2] += 10;
      } else {
        led_list_13[2] = 255;
        i_13 = 3;
      }
      break;
    case 3:
      if (led_list_13[1] - 10 >= 0) {
        led_list_13[1] -= 10;
      } else {
        led_list_13[1] = 0;
        i_13 = 4;
      }
      break;
    case 4:
      if (led_list_13[0] + 10 <= 255) {
        led_list_13[0] += 10;
      } else {
        led_list_13[0] = 255;
        i_13 = 5;
      }
      break;
    case 5:
      if (led_list_13[2] - 10 >= 0) {
        led_list_13[2] -= 10;
      } else {
        led_list_13[2] = 0;
        i_13 = 0;
      }
      break;
    default:
      break;
    }
    break;
  case 14:
    if (i_14 <= 5) {
      led_static_display(led_list_14_1[i_14], level);
    } else if (i_14 > 5 && i_14 <= 14) {
      led_static_display(led_list_14_2[i_14 - 6], level);
    }
    i_14 == 14 ? i_14 = 6 : i_14++;
    break;
  default:
    break;
  }
}

void led_init(void) { xgb_1815_init(); }