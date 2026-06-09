#include "brtc_app.h"
#include "esp_codec_dev.h"
#include "esp_console.h"
#include "esp_log.h"

#include "console.h"

static const char *TAG = "console";

/**
 * @brief  gain 命令：设置麦克风/参考通道的输入增益
 * @param  argc 参数个数
 * @param  argv 参数列表
 * @return 0=成功 1=失败
 */
static int cmd_gain(int argc, char **argv) {
  // 检查参数是否正确（必须输入 ch 和 db 两个参数）
  if (argc < 3) {
    ESP_LOGI("cmd_gain", "用法: gain <通道号> <增益dB>");
    ESP_LOGI("cmd_gain", "示例: gain 0 24.0  -> 设置通道0增益为24dB");
    return 1;
  }

  // 把字符串参数转成数字
  int ch = atoi(argv[1]);   // 通道号 0/1
  float db = atof(argv[2]); // 增益值

  // 获取录音设备句柄（外部提供）
  esp_codec_dev_handle_t rec_dev = get_rec_dev_handle();

  // 检查设备是否初始化成功
  if (!rec_dev) {
    ESP_LOGE("cmd_gain", "录音设备未初始化！");
    return 1;
  }

  // 根据通道号设置增益
  if (ch < 9) {
    // ch0 = 主麦克风
    esp_codec_dev_set_in_channel_gain(rec_dev, ch, db);
    ESP_LOGI("cmd_gain", "已设置 ch%d 增益: %.1f dB", ch, db);
  } else if (ch == 9) {
    // 全局增益
    esp_codec_dev_set_in_gain(rec_dev, db);
    ESP_LOGI("cmd_gain", "已设置 全局增益: %.1f dB", db);
  } else {
    // 无效通道
    ESP_LOGE("cmd_gain", "无效通道！仅支持 0 / 1 / 2 / 3 / 9");
    return 1;
  }

  return 0;
}

/**
 * @brief  注册 gain 命令到控制台
 */
static void register_gain_cmd(void) {
  // 定义命令结构体
  const esp_console_cmd_t cmd = {
      .command = "gain",              // 命令名称
      .help = "设置音频输入通道增益", // 帮助说明
      .hint = "gain <ch> <db>",       // 用法提示
      .func = &cmd_gain,              // 命令执行函数
  };

  // 注册命令
  esp_console_cmd_register(&cmd);
}

static esp_console_repl_t *s_repl;
/**
 * @brief  控制台初始化
 */
void console_init(void) {
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_uart_config_t uart_config =
      ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  repl_config.prompt = "leyu>";
  repl_config.max_cmdline_length = 256;

  ESP_ERROR_CHECK(
      esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl));
  esp_console_register_help_command();
  register_gain_cmd();
  ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
  ESP_LOGI(TAG, "控制台初始化完成 ✅");
}