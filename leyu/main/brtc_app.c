/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in
 * which case, it is free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "brtc_app.h"

#define STATS_TASK_PRIO (10)
#define JOIN_EVENT_BIT (1 << 1)
// #define WAIT_DESTORY_EVENT_BIT (1 << 0)
// #define WAKEUP_REC_READING (1 << 0)
#define BUTTON_WAKEUP_BIT (1 << 0) // 按键唤醒事件位

#define AUDIO_BUFFER_SIZE 640
#define AUDIO_QUEUE_SIZE 2
#define THREAD_STACK_SIZE 4096// RTC任务堆栈大小，确保稳定运行
#define THREAD_PRIORITY 5
#define RTC_AUDIO_BUFFER_SIZE 320 // RTC模式下8kHz 20ms的音频缓冲区大小

static const char *TAG = "BRTC_APP";
bool is_sip_flag = false;
bool is_sip_mode = false;

#include "sip_service.h"
extern esp_rtc_handle_t s_esp_sip;
// 关机请求标志，0：无请求，1：有关机请求
int g_shutdown_request = 0;

// 拨打电话相关标志和号码存储
static bool s_pending_phone_call = false;
static char s_phone_number[32] = {0};
static TimerHandle_t s_phone_call_timer = NULL;

typedef struct {
  char *url;
  bool start;
} audio_player_ctrl_event_t;

struct baidu_rtc_t s_bdrtc;

// BRTC客户端实例
static void *s_rtc_client = NULL;

// 音量控制相关函数
esp_err_t brtc_set_volume_db(float volume_db) {
  if (volume_db < 10.0f) {
    volume_db = 10.0f;
  } else if (volume_db > 80.0f) {
    volume_db = 80.0f;
  }

  s_bdrtc.current_volume = volume_db;

  if (s_bdrtc.play_dev_handle) {
    int ret =
        esp_codec_dev_set_out_vol(s_bdrtc.play_dev_handle, (int)volume_db);
    if (ret != 0) {
      ESP_LOGE(TAG, "设置音量失败: %d", ret);
      return ESP_FAIL;
    }
  }

  ESP_LOGI(TAG, "音量设置为: %.1f dB", volume_db);
  return ESP_OK;
}

// 声明外部函数
void byte_rtc_engine_destroy(void);
int brtc_sdk_init(void);
int brtc_sdk_deinit(void);
void *brtc_create_client(void);
void brtc_destroy_client(void *rtc_client);
int brtc_login_room(void *rtc_client, const char *room_name,
                    const char *user_id, const char *display_name,
                    const char *token);
int brtc_logout_room(void *rtc_client);
void brtc_set_appid(void *rtc_client, const char *app_id);
void brtc_register_message_listener(void *rtc_client,
                                    IRtcMessageListener msgListener);
void brtc_send_audio(void *rtc_client, const void *data, int len);
void brtc_set_audiocodec(void *rtc_client, const char *codec);
void brtc_set_usingvideo(void *rtc_client, int enable);
void brtc_set_receivingvideo(void *rtc_client, int enable);

// 声明回调函数
static void onErrorCallback(int errCode, const char *errMsg);
static void onCallStateChangeCallback(AGentCallState state);
static void onConnectionStateChangeCallback(AGentConnectState state);
static void recorder_event_callback_fn(void *event, void *ctx);

// BRTC消息监听器回调函数
static void on_rtc_message_callback(RtcMessage *msg);
static void onUserAsrSubtitleCallback(const char *text, bool isFinal);
static void onAIAgentSubtitle(const char *text, bool isFinal);
static void onAIAgentSpeaking(bool speaking);
static void onFunctionCall(const char *functionName, const char *args);
static void onAudioPlayerOp(const char *path, bool start);
static void onMediaSetup(void);
static void onAudioData(const uint8_t *data, size_t len);
static void onVideoData(const uint8_t *data, size_t len, int width, int height);
static void onLicenseResult(bool result);
static void setUserParameters(AgentEngineParams *params);

/**
 * @brief 使用新的房间参数重新初始化brtc
 *
 * @param appid 百度appid
 * @param token 鉴权使用串
 * @param userid 用户id
 * @param room_name 房间号
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t brtc_reinit_with_new_room(const char *appid, const char *token,
                                    const char *userid, const char *room_name) {
  if (!appid || !token || !userid || !room_name) {
    ESP_LOGE(TAG, "参数不能为空");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "开始使用新房间参数重新初始化brtc");
  ESP_LOGI(TAG, "appid: %s, userid: %s, roomName: %s", appid, userid,
           room_name);

  // 检查brtc引擎是否正在运行
  if (s_bdrtc.byte_rtc_running) {
    ESP_LOGW(TAG, "brtc引擎正在运行，先停止当前连接");
    // 等待一段时间，确保当前操作完成
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // 1. 销毁当前的brtc引擎
  if (s_bdrtc.engine) {
    ESP_LOGI(TAG, "销毁当前brtc引擎");
    byte_rtc_engine_destroy();

    // 等待一段时间，确保引擎完全销毁
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // 2. 初始化BRTC SDK（如果尚未初始化）
  static bool sdk_initialized = false;
  if (!sdk_initialized) {
    ESP_LOGI(TAG, "初始化BRTC SDK");
    int ret = brtc_sdk_init();
    if (ret != 0) {
      ESP_LOGE(TAG, "BRTC SDK初始化失败，错误代码: %d", ret);
      return ESP_FAIL;
    }
    sdk_initialized = true;
  }

  // 3. 销毁旧的BRTC客户端（如果存在）
  if (s_rtc_client) {
    ESP_LOGI(TAG, "销毁旧的BRTC客户端");
    brtc_logout_room(s_rtc_client);
    brtc_destroy_client(s_rtc_client);
    s_rtc_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // 4. 创建BRTC客户端
  ESP_LOGI(TAG, "创建BRTC客户端");
  s_rtc_client = brtc_create_client();
  if (!s_rtc_client) {
    ESP_LOGE(TAG, "创建BRTC客户端失败");
    return ESP_FAIL;
  }

  // 5. 设置BRTC参数
  ESP_LOGI(TAG, "设置BRTC参数");
  brtc_set_appid(s_rtc_client, appid);

  // 设置音频编码为PCMU，并禁用视频（仅语音通话）
  ESP_LOGI(TAG, "设置语音通话参数");
  brtc_set_audiocodec(s_rtc_client, "pcmu");
  brtc_set_usingvideo(s_rtc_client, 0);
  brtc_set_receivingvideo(s_rtc_client, 0);

  // 注册消息监听器
  ESP_LOGI(TAG, "注册RTC消息监听器");
  brtc_register_message_listener(s_rtc_client, on_rtc_message_callback);

  // 6. 登录房间
  ESP_LOGI(TAG, "使用token直接登录房间: %s", room_name);
  int ret = brtc_login_room(s_rtc_client, room_name, userid, userid, token);
  if (ret != 1) {
    ESP_LOGE(TAG, "登录房间失败，错误代码: %d", ret);
    brtc_destroy_client(s_rtc_client);
    s_rtc_client = NULL;
    return ESP_FAIL;
  }

  // 6. 更新配置
  ESP_LOGI(TAG, "更新brtc配置");

  // 释放旧的配置内存
  if (s_bdrtc.config.appid &&
      strcmp(s_bdrtc.config.appid, CONFIG_BAIDU_APP_ID) != 0) {
    free((void *)s_bdrtc.config.appid);
    s_bdrtc.config.appid = NULL;
  }
  if (s_bdrtc.config.userId &&
      strcmp(s_bdrtc.config.userId, CONFIG_BAIDU_USER_ID) != 0) {
    free((void *)s_bdrtc.config.userId);
    s_bdrtc.config.userId = NULL;
  }

  // 设置新的配置，使用安全的内存分配方式
  s_bdrtc.config.appid = malloc(strlen(appid) + 1);
  if (s_bdrtc.config.appid) {
    strcpy((char *)s_bdrtc.config.appid, appid);
  } else {
    ESP_LOGE(TAG, "内存分配失败：appid");
    if (s_rtc_client) {
      brtc_destroy_client(s_rtc_client);
      s_rtc_client = NULL;
    }
    return ESP_ERR_NO_MEM;
  }

  s_bdrtc.config.userId = malloc(strlen(userid) + 1);
  if (s_bdrtc.config.userId) {
    strcpy((char *)s_bdrtc.config.userId, userid);
  } else {
    ESP_LOGE(TAG, "内存分配失败：userid");
    free((void *)s_bdrtc.config.appid);
    s_bdrtc.config.appid = NULL;
    if (s_rtc_client) {
      brtc_destroy_client(s_rtc_client);
      s_rtc_client = NULL;
    }
    return ESP_ERR_NO_MEM;
  }

  // 7. 创建新的brtc引擎（用于兼容现有代码）
  ESP_LOGI(TAG, "创建新的brtc引擎");

  BaiduChatAgentEvent events = {
      .onError = onErrorCallback,
      .onCallStateChange = onCallStateChangeCallback,
      .onConnectionStateChange = onConnectionStateChangeCallback,
      .onUserAsrSubtitle = onUserAsrSubtitleCallback,
      .onAIAgentSubtitle = onAIAgentSubtitle,
      .onAIAgentSpeaking = onAIAgentSpeaking,
      .onFunctionCall = onFunctionCall,
      .onAudioPlayerOp = onAudioPlayerOp,
      .onMediaSetup = onMediaSetup,
      .onAudioData = onAudioData,
      .onVideoData = onVideoData,
      .onLicenseResult = onLicenseResult,
  };

  s_bdrtc.engine = baidu_create_chat_agent_engine(&events);
  if (s_bdrtc.engine == NULL) {
    ESP_LOGE(TAG, "创建新的brtc引擎失败");
    free((void *)s_bdrtc.config.appid);
    free((void *)s_bdrtc.config.userId);
    s_bdrtc.config.appid = NULL;
    s_bdrtc.config.userId = NULL;
    if (s_rtc_client) {
      brtc_destroy_client(s_rtc_client);
      s_rtc_client = NULL;
    }
    return ESP_FAIL;
  }

  // 8. 设置参数并初始化
  ESP_LOGI(TAG, "设置参数并初始化新的brtc引擎");

  AgentEngineParams agentParams;
  memset(&agentParams, 0, sizeof(AgentEngineParams));

  // 使用安全的字符串复制方式
  strlcpy(agentParams.agent_platform_url, CONFIG_SERVER_URL,
          sizeof(agentParams.agent_platform_url));
  strlcpy(agentParams.appid, s_bdrtc.config.appid, sizeof(agentParams.appid));
  strlcpy(agentParams.userId, s_bdrtc.config.userId,
          sizeof(agentParams.userId));
  strlcpy(agentParams.cer, "./a.cer", sizeof(agentParams.cer));

  if (s_bdrtc.config.workflow) {
    strlcpy(agentParams.workflow, s_bdrtc.config.workflow,
            sizeof(agentParams.workflow));
  }

  if (s_bdrtc.config.license_key) {
    strlcpy(agentParams.license_key, s_bdrtc.config.license_key,
            sizeof(agentParams.license_key));
  }

  // 使用配置字符串模板
  extern const char *config;
  strlcpy(agentParams.config, config, sizeof(agentParams.config));

  agentParams.instance_id = s_bdrtc.config.instance_id;
  agentParams.verbose = false;
  agentParams.enable_local_agent = true;
  agentParams.enable_voice_interrupt = true;
  agentParams.level_voice_interrupt = 80;
  agentParams.AudioInChannel = 1;
  agentParams.AudioInFrequency = 16000;

  // 使用传入的token
  strlcpy(agentParams.token, token, sizeof(agentParams.token));

  // 设置remote_params，使用传入的token和其他参数，避免HTTP认证
  snprintf(agentParams.remote_params, sizeof(agentParams.remote_params),
           "{\"token\":\"%s\",\"appid\":\"%s\",\"userid\":\"%s\",\"roomName\":"
           "\"%s\"}",
           token, appid, userid, room_name);

  // 9. 初始化并加入房间
  ESP_LOGI(TAG, "使用token直接加入新房间，跳过HTTP认证");
  ret = baidu_chat_agent_engine_init(s_bdrtc.engine, &agentParams);
  if (ret != 200) {
    ESP_LOGE(TAG, "初始化新的brtc引擎失败，错误代码: %d", ret);
    baidu_chat_agent_engine_destroy(s_bdrtc.engine);
    s_bdrtc.engine = NULL;
    free((void *)s_bdrtc.config.appid);
    free((void *)s_bdrtc.config.userId);
    s_bdrtc.config.appid = NULL;
    s_bdrtc.config.userId = NULL;
    if (s_rtc_client) {
      brtc_destroy_client(s_rtc_client);
      s_rtc_client = NULL;
    }
    return ESP_FAIL;
  }

  // 10. 开始通话
  baidu_chat_agent_engine_call(s_bdrtc.engine);
  xEventGroupWaitBits(s_bdrtc.join_event, JOIN_EVENT_BIT, pdTRUE, pdTRUE,
                      portMAX_DELAY);
  ESP_LOGI(TAG, "重新加入房间成功");

  return ESP_OK;
}

float brtc_get_volume(void) { return s_bdrtc.current_volume; }

void brtc_set_playing_state(bool is_playing) {
  s_bdrtc.is_playing = is_playing;
  ESP_LOGI(TAG, "播放状态: %s", is_playing ? "播放中" : "停止");
}

bool brtc_is_playing(void) { return s_bdrtc.is_playing; }

bool brtc_is_rtc_mode(void) { return s_bdrtc.is_rtc_mode; }

// 获取播放设备句柄
esp_codec_dev_handle_t get_play_dev_handle(void) {
  return s_bdrtc.play_dev_handle;
}

// 获取录音设备句柄
esp_codec_dev_handle_t get_rec_dev_handle(void) {
  return s_bdrtc.rec_dev_handle;
}

BaiduChatAgentEngine *brtc_get_engine_handle(void) { return s_bdrtc.engine; }

void brtc_force_reconnect(void) {
  if (s_bdrtc.is_connected) {
      ESP_LOGW(TAG, "Forcing BRTC reconnect due to network route change!");
      s_bdrtc.is_connected = false;
  }
}

void brtc_set_rtc_mode(bool enable, void *rtc_client) {
  s_bdrtc.is_rtc_mode = enable;
  s_bdrtc.rtc_client = rtc_client;
  s_bdrtc.is_rtc_stream_up = false;
  s_bdrtc.is_connected = enable;

  if (enable) {
    // 关闭当前的recorder和feeder
    audio_recorder_close();
    audio_feeder_close();

    if (s_bdrtc.play_dev_handle) {
      esp_codec_dev_close(s_bdrtc.play_dev_handle);
    }
    if (s_bdrtc.rec_dev_handle) {
      esp_codec_dev_close(s_bdrtc.rec_dev_handle);
    }

    esp_codec_dev_sample_info_t rec_fs = {
        .sample_rate = 8000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_codec_dev_sample_info_t play_fs = {
        .sample_rate = 8000,
        .channel = 1,
        .bits_per_sample = 16,
    };

    if (s_bdrtc.rec_dev_handle) {
      esp_codec_dev_open(s_bdrtc.rec_dev_handle, &rec_fs);
      ESP_LOGI(TAG, "RTC模式：录音设备采样率设置为8000Hz");
    }
    if (s_bdrtc.play_dev_handle) {
      esp_codec_dev_open(s_bdrtc.play_dev_handle, &play_fs);
      ESP_LOGI(TAG, "RTC模式：播放设备采样率设置为8000Hz");
    }

    // 重新配置编码器为PCMU 8000Hz
    av_processor_encoder_config_t encoder_cfg = {0};
    encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711U;
    encoder_cfg.params.g711.audio_info.sample_rate = 8000;
    encoder_cfg.params.g711.audio_info.sample_bits = 16;
    encoder_cfg.params.g711.audio_info.channels = 1;
    encoder_cfg.params.g711.audio_info.frame_duration = 20;

    audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    memcpy((void *)&recorder_cfg.encoder_cfg, &encoder_cfg,
           sizeof(av_processor_encoder_config_t));
    recorder_cfg.recorder_event_cb = recorder_event_callback_fn;
    audio_recorder_open(&recorder_cfg);

    // 重新配置解码器为PCMU 8000Hz
    av_processor_decoder_config_t feeder_cfg = {0};
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711U;
    feeder_cfg.params.g711.audio_info.sample_rate = 8000;
    feeder_cfg.params.g711.audio_info.sample_bits = 16;
    feeder_cfg.params.g711.audio_info.channels = 1;
    feeder_cfg.params.g711.audio_info.frame_duration = 20;

    audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
    memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
           sizeof(av_processor_decoder_config_t));
    audio_feeder_open(&feeder_config);
    audio_feeder_run();

    audio_processor_mixer_close();
    ESP_LOGI(TAG, "RTC模式：关闭audio_mixer，重新配置编码器为PCMU 8000Hz");
  } else {
    // 关闭当前的recorder和feeder
    audio_recorder_close();
    audio_feeder_close();

    if (s_bdrtc.play_dev_handle) {
      esp_codec_dev_close(s_bdrtc.play_dev_handle);
    }
    if (s_bdrtc.rec_dev_handle) {
      esp_codec_dev_close(s_bdrtc.rec_dev_handle);
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };

    if (s_bdrtc.rec_dev_handle) {
      esp_codec_dev_open(s_bdrtc.rec_dev_handle, &fs);
      ESP_LOGI(TAG, "AI模式：录音设备采样率恢复为16000Hz");
    }
    if (s_bdrtc.play_dev_handle) {
      esp_codec_dev_open(s_bdrtc.play_dev_handle, &fs);
      ESP_LOGI(TAG, "AI模式：播放设备采样率恢复为16000Hz");
    }

    // 恢复编码器为PCM 16000Hz
    av_processor_encoder_config_t encoder_cfg = {0};
    encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    encoder_cfg.params.pcm.audio_info.sample_rate = 16000;
    encoder_cfg.params.pcm.audio_info.sample_bits = 16;
    encoder_cfg.params.pcm.audio_info.channels = 1;
    encoder_cfg.params.pcm.audio_info.frame_duration = 20;

    audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    memcpy((void *)&recorder_cfg.encoder_cfg, &encoder_cfg,
           sizeof(av_processor_encoder_config_t));
    recorder_cfg.recorder_event_cb = recorder_event_callback_fn;
    audio_recorder_open(&recorder_cfg);

    // 恢复解码器为PCM 16000Hz
    av_processor_decoder_config_t feeder_cfg = {0};
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    feeder_cfg.params.pcm.audio_info.sample_rate = 16000;
    feeder_cfg.params.pcm.audio_info.sample_bits = 16;
    feeder_cfg.params.pcm.audio_info.channels = 1;
    feeder_cfg.params.pcm.audio_info.frame_duration = 20;

    audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
    memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
           sizeof(av_processor_decoder_config_t));
    audio_feeder_open(&feeder_config);
    audio_feeder_run();

    audio_processor_mixer_open();
    ESP_LOGI(TAG, "AI模式：重新打开audio_mixer，恢复编码器为PCM 16000Hz");

    audio_processor_ramp_control(AUDIO_MIXER_FOCUS_PLAYBACK);
    ESP_LOGI(TAG, "RTC模式禁用，音频混音器优先级设置为PLAYBACK_BOOST");
  }

  ESP_LOGI(TAG, "RTC模式: %s, rtc_client: %p, is_connected: %d",
           enable ? "启用" : "禁用", rtc_client, s_bdrtc.is_connected);
}

void brtc_set_rtc_stream_up(bool is_stream_up) {
  s_bdrtc.is_rtc_stream_up = is_stream_up;
  ESP_LOGI(TAG, "RTC流状态: %s", is_stream_up ? "已建立" : "未建立");
}

esp_err_t brtc_switch_to_ai_mode(void) {
  ESP_LOGI(TAG, "开始切换回AI对话模式");

  // 1. 禁用RTC模式，恢复AI模式
  brtc_set_rtc_mode(false, NULL);

  // 3. 退出RTC房间并销毁RTC客户端（如果存在）
  if (s_bdrtc.rtc_client) {
    ESP_LOGI(TAG, "退出RTC房间");
    brtc_logout_room(s_bdrtc.rtc_client);
    brtc_destroy_client(s_bdrtc.rtc_client);
    s_bdrtc.rtc_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // 4. 重新初始化BRTC SDK
  ESP_LOGI(TAG, "重新初始化BRTC SDK");
  int ret = brtc_sdk_init();
  if (ret < 0 && ret != 1) {
    ESP_LOGE(TAG, "BRTC SDK初始化失败，错误代码: %d", ret);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "BRTC SDK初始化成功");

  // 4. 创建新的brtc引擎用于AI对话
  ESP_LOGI(TAG, "创建新的brtc引擎");

  BaiduChatAgentEvent events = {
      .onError = onErrorCallback,
      .onCallStateChange = onCallStateChangeCallback,
      .onConnectionStateChange = onConnectionStateChangeCallback,
      .onUserAsrSubtitle = onUserAsrSubtitleCallback,
      .onAIAgentSubtitle = onAIAgentSubtitle,
      .onAIAgentSpeaking = onAIAgentSpeaking,
      .onFunctionCall = onFunctionCall,
      .onAudioPlayerOp = onAudioPlayerOp,
      .onMediaSetup = onMediaSetup,
      .onAudioData = onAudioData,
      .onVideoData = onVideoData,
      .onLicenseResult = onLicenseResult,
  };

  s_bdrtc.engine = baidu_create_chat_agent_engine(&events);
  if (s_bdrtc.engine == NULL) {
    ESP_LOGE(TAG, "Engine initialization failed.");
    return ESP_FAIL;
  }

  AgentEngineParams agentParams;
  setUserParameters(&agentParams);

  ESP_LOGI(TAG, "使用AI对话配置登录...");
  ret = baidu_chat_agent_engine_init(s_bdrtc.engine, &agentParams);
  if (ret != 200) {
    ESP_LOGE(TAG, "Failed to log in. Error code: %d", ret);
    baidu_chat_agent_engine_destroy(s_bdrtc.engine);
    s_bdrtc.engine = NULL;
    return ESP_FAIL;
  }

  // 5. 开始AI对话
  baidu_chat_agent_engine_call(s_bdrtc.engine);
  xEventGroupWaitBits(s_bdrtc.join_event, JOIN_EVENT_BIT, pdTRUE, pdTRUE,
                      portMAX_DELAY);
  ESP_LOGI(TAG, "AI对话房间登录成功");

  ESP_LOGI(TAG, "切换回AI对话模式完成");
  return ESP_OK;
}

// 音量增加函数
void brtc_volume_up(void) {
  float current_vol = brtc_get_volume();
  float new_vol = current_vol + 5.0f; // 每次增加5dB

  if (new_vol >= 80.0f) {
    new_vol = 80.0f;
    audio_playback_play("file:///spiffs/maxvolume.mp3");
  } else {
    audio_playback_play("file:///spiffs/Volume+.mp3");
  }

  ESP_LOGI(TAG, "音量增加: %.1f -> %.1f dB", current_vol, new_vol);
  brtc_set_volume_db(new_vol);
}
void shutdown_V(void) { audio_prompt_play("file:///spiffs/Shutdown.mp3"); }
// 音量减少函数
void brtc_volume_down(void) {
  float current_vol = brtc_get_volume();
  float new_vol = current_vol - 5.0f; // 每次减少5dB

  if (new_vol <= 10.0f) {
    new_vol = 10.0f;
    audio_playback_play("file:///spiffs/minivolume.mp3"); // 播放最低音量
  } else {
    audio_playback_play("file:///spiffs/Volume-.mp3"); // 播放音量减小
  }

  ESP_LOGI(TAG, "音量减少: %.1f -> %.1f dB", current_vol, new_vol);
  brtc_set_volume_db(new_vol);
}

static void onErrorCallback(int errCode, const char *errMsg) {
  ESP_LOGE(TAG, "Error occurred. Code: %d, Message: %s", errCode, errMsg);
}

// 定时器回调函数，用于拨打电话
static void phone_call_timer_callback(TimerHandle_t xTimer) {
  // 检查是否有待拨打的电话
  if (s_pending_phone_call && strlen(s_phone_number) > 0) {
    ESP_LOGI(TAG, "TTS播放完成，开始拨打电话: %s", s_phone_number);

    // 调用拨打电话函数
    esp_err_t call_ret = at_make_phone_call(s_phone_number);
    if (call_ret != ESP_OK) {
      ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
    }

    // 清除标志
    s_pending_phone_call = false;
    memset(s_phone_number, 0, sizeof(s_phone_number));
  }

  // 删除定时器
  if (s_phone_call_timer != NULL) {
    xTimerDelete(s_phone_call_timer, 0);
    s_phone_call_timer = NULL;
  }
}

static void onCallStateChangeCallback(AGentCallState state) {
  ESP_LOGI(TAG, "Call state changed: %d", state);
  if (s_bdrtc.is_destroying) {
    ESP_LOGW(TAG, "引擎正在销毁中，忽略回调");
    return;
  }
  if (state == AGENT_LOGIN_SUCCESS) {
    ESP_LOGI(TAG, "Login success");
    s_bdrtc.is_connected = true;
    xEventGroupSetBits(s_bdrtc.join_event, JOIN_EVENT_BIT);
  } else if (state == AGENT_LOGIN_FAIL) {
    ESP_LOGE(TAG, "Login failed");
    s_bdrtc.is_connected = false;
  }
}

static void onConnectionStateChangeCallback(AGentConnectState state) {
  ESP_LOGI(TAG, "Connection state changed: %d", state);
  switch (state) {
  case AGENT_CONNECTION_STATE_CONNECTED:
    ESP_LOGI(TAG, "Connection state changed: connected");
    s_bdrtc.is_connected = true;
    break;
  case AGENT_CONNECTION_STATE_DISCONNECTED:
    ESP_LOGE(TAG, "Connection state changed: disconnected");
    s_bdrtc.is_connected = false;
    break;
  default:
    break;
  }
}

// BRTC消息监听器回调函数
static void on_rtc_message_callback(RtcMessage *msg) {
  if (!msg) {
    ESP_LOGE(TAG, "收到空的RTC消息");
    return;
  }

  switch (msg->msgType) {
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_OK:
    ESP_LOGI(TAG, "RTC房间登录成功");
    break;
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_TIMEOUT:
    ESP_LOGE(TAG, "RTC房间登录超时");
    break;
  case RTC_MESSAGE_ROOM_EVENT_LOGIN_ERROR:
    ESP_LOGE(TAG, "RTC房间登录错误: %s",
             msg->extra_info ? msg->extra_info : "未知错误");
    break;
  case RTC_MESSAGE_ROOM_EVENT_CONNECTION_LOST:
    ESP_LOGW(TAG, "RTC连接丢失");
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_COMING:
    ESP_LOGI(TAG, "RTC远程用户进入: feedId=%" PRIi64, msg->data.feedId);
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_LEAVING:
    ESP_LOGI(TAG, "RTC远程用户离开: feedId=%" PRIi64, msg->data.feedId);
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_RENDERING:
    ESP_LOGI(TAG, "RTC远程用户渲染: feedId=%" PRIi64, msg->data.feedId);
    break;
  case RTC_MESSAGE_ROOM_EVENT_REMOTE_GONE:
    ESP_LOGI(TAG, "RTC远程用户消失: feedId=%" PRIi64, msg->data.feedId);
    break;
  case RTC_MESSAGE_ROOM_EVENT_SERVER_ERROR:
    ESP_LOGE(TAG, "RTC服务器错误: %s",
             msg->extra_info ? msg->extra_info : "未知错误");
    break;
  case RTC_ROOM_EVENT_ON_USER_JOINED_ROOM:
    ESP_LOGI(TAG, "RTC用户加入房间事件");
    break;
  case RTC_ROOM_EVENT_ON_USER_LEAVING_ROOM:
    ESP_LOGI(TAG, "RTC用户离开房间事件");
    break;
  case RTC_ROOM_EVENT_ON_USER_MESSAGE:
    ESP_LOGI(TAG, "RTC用户消息: %s", msg->extra_info ? msg->extra_info : "");
    break;
  case RTC_ROOM_EVENT_ON_USER_ATTRIBUTE:
    ESP_LOGI(TAG, "RTC用户属性变更");
    break;
  case RTC_MESSAGE_STATE_STREAM_UP:
    ESP_LOGI(TAG, "RTC流状态: 上线");
    break;
  case RTC_MESSAGE_STATE_SENDING_MEDIA_OK:
    ESP_LOGI(TAG, "RTC媒体发送成功");
    break;
  case RTC_MESSAGE_STATE_SENDING_MEDIA_FAILED:
    ESP_LOGE(TAG, "RTC媒体发送失败");
    break;
  case RTC_MESSAGE_STATE_STREAM_DOWN:
    ESP_LOGI(TAG, "RTC流状态: 下线");
    break;
  case RTC_STATE_STREAM_SLOW_LINK_NACKS:
    ESP_LOGW(TAG, "RTC网络慢连接，NACK数量增加");
    break;
  default:
    ESP_LOGW(TAG, "RTC未知消息类型: %d", msg->msgType);
    break;
  }
}

static void onUserAsrSubtitleCallback(const char *text, bool isFinal) {
  ESP_LOGI(TAG, "User ASR subtitle: %s (isFinal: %d)", text, isFinal);
}

#include "led_control.h"

static void onFunctionCall(const char *id, const char *params) {
  ESP_LOGI(TAG, "onFunctionCall, id: %s, params: %s", id, params);

  // 解析JSON参数
  cJSON *root = cJSON_Parse(params);
  if (!root) {
    ESP_LOGE(TAG, "解析JSON参数失败");
    return;
  }

  // 获取content对象
  cJSON *content = cJSON_GetObjectItem(root, "content");
  if (!cJSON_IsObject(content)) {
    ESP_LOGE(TAG, "JSON格式错误，找不到content对象");
    cJSON_Delete(root);
    return;
  }

  // 获取function_name
  cJSON *function_name = cJSON_GetObjectItem(content, "function_name");
  if (!cJSON_IsString(function_name)) {
    ESP_LOGE(TAG, "JSON格式错误，找不到function_name");
    cJSON_Delete(root);
    return;
  }

  // 获取parameter_list数组
  cJSON *parameter_list = cJSON_GetObjectItem(content, "parameter_list");
  if (!cJSON_IsArray(parameter_list)) {
    ESP_LOGE(TAG, "JSON格式错误，找不到parameter_list数组");
    cJSON_Delete(root);
    return;
  }
  // 调用baidu_chat_agent_engine_send_text_to_TTS进行语音合成播放"好的，没问题"
  // baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, "好的，没问题");
  ESP_LOGI(TAG, "函数名称: %s", function_name->valuestring);

  // 全局拦截：在执行具体指令的语音播报之前，先打断大模型云端返回的普通对话语音
  bool was_playing = false;
  if (brtc_is_playing() && s_bdrtc.engine) {
    was_playing = true;
    baidu_chat_agent_engine_interrupt(s_bdrtc.engine);
    vTaskDelay(pdMS_TO_TICKS(100)); // 稍作延时，确保底层播放器已停止并清理干净
  }

  // 根据不同的function_name执行不同的操作
  if (strcmp(function_name->valuestring, "yinliangtiaozheng") == 0) {
    ESP_LOGI(TAG, "执行音量调整操作");

    // 解析参数
    int param_count = cJSON_GetArraySize(parameter_list);
    int mode = 0;       // 调整模式：1调大 2减小 3直接设置
    float value = 0.0f; // 调整值

    // 遍历参数列表，查找p1和p2参数
    for (int i = 0; i < param_count; i++) {
      cJSON *param = cJSON_GetArrayItem(parameter_list, i);
      if (cJSON_IsObject(param)) {
        // 检查是否包含p1参数
        cJSON *p1 = cJSON_GetObjectItem(param, "p1");
        if (p1) {
          if (cJSON_IsNumber(p1)) {
            mode = p1->valueint;
            ESP_LOGI(TAG, "调整模式(p1): %d", mode);
          } else if (cJSON_IsString(p1)) {
            // 如果p1是字符串，尝试转换为数字
            mode = atoi(p1->valuestring);
            ESP_LOGI(TAG, "调整模式(p1): %d (从字符串转换)", mode);
          }
        }

        // 检查是否包含p2参数
        cJSON *p2 = cJSON_GetObjectItem(param, "p2");
        if (p2) {
          if (cJSON_IsNumber(p2)) {
            value = (float)p2->valuedouble;
            ESP_LOGI(TAG, "调整值(p2): %.1f", value);
          } else if (cJSON_IsString(p2)) {
            // 如果p2是字符串，尝试转换为数字
            value = (float)atof(p2->valuestring);
            ESP_LOGI(TAG, "调整值(p2): %.1f (从字符串转换)", value);
          }
        }
      }
    }

    // 根据模式执行音量调整
    if (mode == 1) {
      // 调大音量
      ESP_LOGI(TAG, "调大音量");
      brtc_volume_up();
    } else if (mode == 2) {
      // 调小音量
      ESP_LOGI(TAG, "调小音量");
      brtc_volume_down();
    } else if (mode == 3) {
      // 直接设置音量
      if (value > 100.0f) {
        value = 100.0f;
      } else if (value < 0.0f) {
        value = 0.0f;
      }
      ESP_LOGI(TAG, "直接设置音量为: %.1f", value);
      esp_err_t ret = brtc_set_volume_db(value);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "音量已设置为: %.1f dB", value);
      } else {
        ESP_LOGE(TAG, "设置音量失败");
      }
    } else {
      ESP_LOGW(TAG, "未知的调整模式: %d", mode);
    }
    // 调用baidu_chat_agent_engine_send_text_to_TTS进行语音合成播放
    baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, "音量已调节");

  } else if (strcmp(function_name->valuestring, "TiaoJieDengGuang") == 0) {
    ESP_LOGI(TAG, "执行灯光调节操作");
    int param_count = cJSON_GetArraySize(parameter_list);
    uint8_t mode = 0;

    // 遍历参数列表，查找type参数
    for (int i = 0; i < param_count; i++) {
      cJSON *param = cJSON_GetArrayItem(parameter_list, i);
      if (cJSON_IsObject(param)) {
        // 检查是否包含type参数
        cJSON *type = cJSON_GetObjectItem(param, "type");
        if (type) {
          if (cJSON_IsNumber(type)) {
            mode = type->valueint;
          } else if (cJSON_IsString(type)) {
            mode = atoi(type->valuestring);
            ESP_LOGI(TAG, "灯光模式：%s", type->valuestring);
          }
          ESP_LOGI(TAG, "灯光模式：%d", mode);
        }

        char buffer[100];
        switch (mode) {
        case 0:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "正在关闭灯光");
          break;
        case 1:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "正在打开灯光");
          break;
        case 11:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "灯光模式为多色闪烁模式");
          break;
        case 12:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "灯光模式为SOS闪烁模式");
          break;
        case 13:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "灯光模式为渐变模式");
          break;
        case 14:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "灯光模式为跑马灯模式");
          break;
        case 15:
          if (level >= 5) {
            snprintf(buffer, sizeof(buffer),
                     "当前灯光亮度等级为：%d,灯光亮度已最高", level);
          } else {
            snprintf(buffer, sizeof(buffer),
                     "当前灯光亮度等级为：%d,灯光亮度增加", level);
          }
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, buffer);
          break;
        case 16:
          if (level <= 1) {
            snprintf(buffer, sizeof(buffer),
                     "当前灯光亮度等级为：%d,灯光亮度已最低", level);
          } else {
            snprintf(buffer, sizeof(buffer),
                     "当前灯光亮度等级为：%d,灯光亮度减少", level);
          }
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, buffer);
          break;
        default:
          baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                   "灯光颜色已调节");
          break;
        }
        led_display(mode);
      }
    }

  } else if (strcmp(function_name->valuestring, "bodadianhua") == 0) {
    ESP_LOGI(TAG, "执行拨打电话操作");

    // 解析参数
    int param_count = cJSON_GetArraySize(parameter_list);
    for (int i = 0; i < param_count; i++) {
      cJSON *param = cJSON_GetArrayItem(parameter_list, i);
      if (cJSON_IsObject(param)) {
        // 获取p1和p2参数
        cJSON *p1 = cJSON_GetObjectItem(param, "p1");
        cJSON *p2 = cJSON_GetObjectItem(param, "p2");

        if (p1) {
          if (cJSON_IsNumber(p1)) {
            ESP_LOGI(TAG, "参数p1: %d", p1->valueint);
            // 将数字转换为字符串并存储到全局变量
            snprintf(s_phone_number, sizeof(s_phone_number), "%d",
                     p1->valueint);
            s_pending_phone_call = true;
          } else if (cJSON_IsString(p1)) {
            ESP_LOGI(TAG, "参数p1: %s", p1->valuestring);
            // 直接使用字符串存储到全局变量
            strncpy(s_phone_number, p1->valuestring,
                    sizeof(s_phone_number) - 1);
            s_phone_number[sizeof(s_phone_number) - 1] = '\0';
            s_pending_phone_call = true;
          }
        }

        if (p2) {
          if (cJSON_IsString(p2)) {
            ESP_LOGI(TAG, "参数p2: %s", p2->valuestring);
            // 将p2作为联系人姓名，查找对应的电话号码
            char phone[MAX_PHONE_LENGTH];
            esp_err_t contact_ret =
                contact_get_phone_by_name(p2->valuestring, phone);
            if (contact_ret == ESP_OK) {
              ESP_LOGI(TAG, "找到联系人 %s 的电话号码: %s", p2->valuestring,
                       phone);
              // 将电话号码存储到全局变量
              strncpy(s_phone_number, phone, sizeof(s_phone_number) - 1);
              s_phone_number[sizeof(s_phone_number) - 1] = '\0';
              s_pending_phone_call = true;
              ESP_LOGI(TAG, "已设置待拨打电话: %s", s_phone_number);
            } else {
              ESP_LOGE(TAG, "未找到联系人 %s 的电话号码", p2->valuestring);
            }
          }
        }
      }
    }

    // 播放TTS提示音，不阻塞当前回调
    if (s_pending_phone_call) {
      char tts_text[128];
      snprintf(tts_text, sizeof(tts_text), "小乐正在为您接通%s",
               s_phone_number);
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, tts_text);

      // 如果已有定时器，先删除
      if (s_phone_call_timer != NULL) {
        xTimerDelete(s_phone_call_timer, 0);
        s_phone_call_timer = NULL;
      }

      // 创建定时器来处理拨打电话，3秒后执行
      s_phone_call_timer =
          xTimerCreate("phone_call_timer", pdMS_TO_TICKS(10 * 1000), pdFALSE,
                       (void *)0, phone_call_timer_callback);

      if (s_phone_call_timer == NULL) {
        ESP_LOGE(TAG, "创建拨打电话定时器失败，内存可能不足");
        // 如果定时器创建失败，直接拨打电话
        ESP_LOGI(TAG, "直接拨打电话: %s", s_phone_number);
        esp_err_t call_ret = at_make_phone_call(s_phone_number);
        if (call_ret != ESP_OK) {
          ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
        }
        s_pending_phone_call = false;
        memset(s_phone_number, 0, sizeof(s_phone_number));
      } else {
        // 启动定时器
        if (xTimerStart(s_phone_call_timer, 0) != pdPASS) {
          ESP_LOGE(TAG, "启动拨打电话定时器失败");
          // 启动失败，直接拨打电话
          ESP_LOGI(TAG, "直接拨打电话: %s", s_phone_number);
          esp_err_t call_ret = at_make_phone_call(s_phone_number);
          if (call_ret != ESP_OK) {
            ESP_LOGE(TAG, "拨打电话失败 - 错误: %d", call_ret);
          }
          s_pending_phone_call = false;
          memset(s_phone_number, 0, sizeof(s_phone_number));
          xTimerDelete(s_phone_call_timer, 0);
          s_phone_call_timer = NULL;
        } else {
          ESP_LOGI(TAG, "拨打电话定时器创建并启动成功");
        }
      }
    }

    // 拨打电话操作已完成（异步处理）
    ESP_LOGI(TAG, "拨打电话操作已设置为异步处理");

  } else if (strcmp(function_name->valuestring, "guanji") == 0) {
    ESP_LOGI(TAG, "执行关机操作");
    baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                             "正在为您关机，再见！");
    ESP_LOGI(TAG, "TTS播放指令已发送，设置关机标志");

    // 设置关机标志，由系统监控任务处理实际关机操作
    g_shutdown_request = 1;

  } else if (strcmp(function_name->valuestring, "jiuming") == 0) {
    ESP_LOGI(TAG, "执行sos操作");
    // 灯光模式设置为SOS闪烁
    led_display(12);
    // 从PSRAM获取SOS绑定号码
    const char *sos_number = get_phone_number_by_key(2); // 2表示SOS
    if (sos_number != NULL && strlen(sos_number) > 0) {
      ESP_LOGI(TAG, "SOS绑定号码: %s", sos_number);

      // 使用封装的AT指令拨打电话函数
      esp_err_t call_ret = at_make_phone_call(sos_number);
      if (call_ret != ESP_OK) {
        ESP_LOGE(TAG, "拨打SOS号码失败 - 错误: %d", call_ret);
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "拨打SOS号码失败");
      }
    } else {
      ESP_LOGW(TAG, "SOS绑定号码为空或未设置，播放提示音");
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                               "SOS绑定号码为空或未设置");
    }

  } else if (strcmp(function_name->valuestring, "hujiaozongtai") == 0) {
    ESP_LOGI(TAG, "执行呼叫座席操作");
    if (s_esp_sip == NULL) {
      baidu_chat_agent_engine_send_text_to_TTS(
          s_bdrtc.engine, "SIP服务未启动，暂时不能为您呼叫总台");
    } else {
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                               "正在为您呼叫总台");
      brtc_set_playing_state(true);
      is_sip_flag = true;
    }
  } else if (strcmp(function_name->valuestring, "gengxintongxunlu") == 0) {
    ESP_LOGI(TAG, "执行更新通讯录操作");

    // 解析参数
    int param_count = cJSON_GetArraySize(parameter_list);
    char contact_name[MAX_NAME_LENGTH] = {0};
    char contact_phone[MAX_PHONE_LENGTH] = {0};

    // 遍历参数列表，查找phone和name参数
    for (int i = 0; i < param_count; i++) {
      cJSON *param = cJSON_GetArrayItem(parameter_list, i);
      if (cJSON_IsObject(param)) {
        // 检查是否包含phone参数
        cJSON *phone = cJSON_GetObjectItem(param, "phone");
        if (phone && cJSON_IsString(phone)) {
          ESP_LOGI(TAG, "参数phone: %s", phone->valuestring);
          strncpy(contact_phone, phone->valuestring, MAX_PHONE_LENGTH - 1);
          contact_phone[MAX_PHONE_LENGTH - 1] = '\0';
        }

        // 检查是否包含name参数
        cJSON *name = cJSON_GetObjectItem(param, "name");
        if (name && cJSON_IsString(name)) {
          ESP_LOGI(TAG, "参数name: %s", name->valuestring);
          strncpy(contact_name, name->valuestring, MAX_NAME_LENGTH - 1);
          contact_name[MAX_NAME_LENGTH - 1] = '\0';
        }
      }
    }

    // 检查参数是否完整
    if (strlen(contact_name) == 0 || strlen(contact_phone) == 0) {
      ESP_LOGE(TAG, "通讯录参数不完整，name=%s, phone=%s", contact_name,
               contact_phone);
      baidu_chat_agent_engine_send_text_to_TTS(
          s_bdrtc.engine, "通讯录信息不完整，请提供姓名和电话号码");
    } else {
      // 添加或更新联系人
      ESP_LOGI(TAG, "开始添加或更新联系人: %s - %s", contact_name,
               contact_phone);
      esp_err_t ret =
          contact_manager_add_or_update(contact_name, contact_phone);

      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "通讯录更新成功");
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "通讯录已更新");

        // 通过MQTT向服务器发送完整通讯录同步数据
        bool sync_ret = send_contact_sync_data();
        if (sync_ret) {
          ESP_LOGI(TAG, "完整通讯录同步数据发送成功");
        } else {
          ESP_LOGW(TAG, "完整通讯录同步数据发送失败");
        }
      } else {
        ESP_LOGE(TAG, "通讯录更新失败，错误代码: %d", ret);
        baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                                 "通讯录更新失败，请稍后再试");
      }
    }

  } else if (strcmp(function_name->valuestring, "tingzhiduihua") == 0) {
    ESP_LOGI(TAG, "执行暂停播放操作");
    if (was_playing) {
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                               "好的，马上闭嘴");
    } else {
      baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                               "小乐，没有说话呀！");
    }
  } else {
    ESP_LOGI(TAG, "未知函数名称: %s", function_name->valuestring);
  }

  // 释放JSON对象
  cJSON_Delete(root);
}

static void onMediaSetup(void) {
  ESP_LOGI(TAG, "Media setup completed.");
  baidu_chat_agent_engine_send_text_to_TTS(
      s_bdrtc.engine,
      "网络连接成功。你一出现，气氛就变得好软萌呀，快来和我说说话");
  mqtt_flash_load();
}

static void onAIAgentSubtitle(const char *text, bool isFinal) {
  ESP_LOGI(TAG, "onAIAgentSubtitle %s", text);
}

static void onAIAgentSpeaking(bool speeking) {
  ESP_LOGI(TAG, "onAIAgentSpeaking is %s", speeking ? "Start" : "Stop");

  // 更新播放状态标志
  brtc_set_playing_state(speeking);

  if (speeking) {
    audio_processor_ramp_control(AUDIO_MIXER_FOCUS_FEEDER);
  } else {
    audio_processor_ramp_control(AUDIO_MIXER_FOCUS_PLAYBACK);
  }
}

static void onAudioPlayerOp(const char *path, bool start) {
  ESP_LOGI(TAG, "onAudioPlayerOp. path:%s, start:%d", path, start);

  // 检查是否正在播放铃声，如果是，则忽略AI的音频播放请求
  if (start && g_is_playing_ringtone) {
    ESP_LOGW(TAG, "正在播放铃声，忽略AI的音频播放请求: %s", path);
    return;
  }

  audio_player_ctrl_event_t event;
  event.url = esp_gmf_oal_strdup(path);
  event.start = start;

  if (xQueueSend(s_bdrtc.audio_player_queue, &event, portMAX_DELAY) != pdPASS) {
    ESP_LOGE(TAG, "Failed to send audio player event");
    esp_gmf_oal_free(event.url);
  }
}

static void onAudioData(const uint8_t *data, size_t len) {
  // ESP_LOGW(TAG, "onAudioData, len: %d", len);
  if (!is_sip_mode) {
    audio_feeder_feed_data((uint8_t *)data, len);
  }
}

static void onVideoData(const uint8_t *data, size_t len, int width,
                        int height) {
  ESP_LOGD(TAG, "Received video data of length: %zu, width: %d, height: %d",
           len, width, height);
}

static void onLicenseResult(bool result) {
  ESP_LOGI(TAG, "onLicenseResult: %d", result);
  if (!result) {
    ESP_LOGE(TAG, "onLicenseResult: failed");
  }
}

void byte_rtc_engine_destroy() {
  s_bdrtc.is_destroying = true;

  if (s_bdrtc.engine) {
    baidu_chat_agent_engine_destroy(s_bdrtc.engine);
    s_bdrtc.engine = NULL;
  }

  if (s_rtc_client) {
    brtc_logout_room(s_rtc_client);
    brtc_destroy_client(s_rtc_client);
    s_rtc_client = NULL;
  }

  ESP_LOGI(TAG, "完全释放BRTC SDK");
  brtc_sdk_deinit();

  s_bdrtc.is_destroying = false;
}

const char *config =
    "{\"app_id\": \"%s\", \"config\" : \"{\\\"llm\\\" : \\\"%s\\\", "
    "\\\"llm_token\\\" : \\\"no\\\", \\\"rtc_ac\\\": \\\"%s\\\", "
    "\\\"lang\\\" "
    ": \\\"%s\\\",  \\\"enable_visual\\\" : \\\"%s\\\", \\\"dfda\\\" : "
    "\\\"%s\\\"}\", \"quick_start\": true}";

static void setUserParameters(AgentEngineParams *params) {
  memset(params, 0, sizeof(AgentEngineParams));

  if (s_bdrtc.config.config_server_url) {
    snprintf(params->agent_platform_url, sizeof(params->agent_platform_url),
             "%s", s_bdrtc.config.config_server_url);
  } else {
    snprintf(params->agent_platform_url, sizeof(params->agent_platform_url),
             "%s", CONFIG_SERVER_URL);
  }
  snprintf(params->appid, sizeof(params->appid), "%s", s_bdrtc.config.appid);
  snprintf(params->userId, sizeof(params->userId), "%s", s_bdrtc.config.userId);
  snprintf(params->cer, sizeof(params->cer), "%s", "./a.cer");
  snprintf(params->workflow, sizeof(params->workflow), "%s",
           s_bdrtc.config.workflow);
  snprintf(params->license_key, sizeof(params->license_key), "%s",
           s_bdrtc.config.license_key);
  snprintf(params->config, sizeof(params->config), "%s", config);
  params->instance_id = s_bdrtc.config.instance_id;
  params->verbose = false;
  params->enable_local_agent = true;
  params->enable_voice_interrupt = true;
  params->level_voice_interrupt = 80;
  params->AudioInChannel = 1;
  params->AudioInFrequency = 16000;
}

esp_err_t brtc_create(void) {
  if (s_bdrtc.config.appid) free((void *)s_bdrtc.config.appid);
  s_bdrtc.config.appid = strdup(CONFIG_BAIDU_APP_ID);

  // 从flash读取SN码作为用户ID
  char sn_buffer[128] = {0};
  esp_err_t sn_ret = read_sn_from_nvs(sn_buffer, sizeof(sn_buffer));
  if (s_bdrtc.config.userId) free((void *)s_bdrtc.config.userId);
  if (sn_ret == ESP_OK && strlen(sn_buffer) > 0) {
    ESP_LOGI(TAG, "从flash读取SN码作为用户ID: %s", sn_buffer);
    s_bdrtc.config.userId = strdup(sn_buffer); // 动态分配内存存储SN码
  } else {
    ESP_LOGW(TAG, "无法从flash读取SN码，使用默认用户ID");
    s_bdrtc.config.userId = strdup(CONFIG_BAIDU_USER_ID);
  }

  // 获取当前固件版本号
  const char *version = "100003";

  // 调用OTA检查
  char *ota_response = malloc(1024);
  if (!ota_response) {
      ESP_LOGE(TAG, "内存分配失败：ota_response");
      if (s_bdrtc.config.userId && strcmp(s_bdrtc.config.userId, CONFIG_BAIDU_USER_ID) != 0) {
          free((void *)s_bdrtc.config.userId);
          s_bdrtc.config.userId = NULL;
      }
      return ESP_ERR_NO_MEM;
  }
  memset(ota_response, 0, 1024);
  size_t ota_response_size = 1024;
  ESP_LOGI(TAG, "开始OTA检查，SN: %s, 版本: %s",
           strlen(sn_buffer) > 0 ? sn_buffer : "unknown", version);
  int ota_ret = ota_check_g(strlen(sn_buffer) > 0 ? sn_buffer : "unknown",
                            version, ota_response, &ota_response_size);
  if (ota_ret == 0) {
    //  解析JSON响应
    cJSON *root = cJSON_Parse(ota_response);
    if (root != NULL) {
      cJSON *code = cJSON_GetObjectItem(root, "code");
      if (code != NULL && cJSON_IsNumber(code)) {
        int response_code = code->valueint;
        if (response_code != 200) {
          cJSON *message = cJSON_GetObjectItem(root, "message");
          const char *msg = message != NULL && cJSON_IsString(message)
                                ? message->valuestring
                                : "未知错误";
          char error_msg[256];
          snprintf(error_msg, sizeof(error_msg), "返回错误码: %d, 消息: %s",
                   response_code, msg);
          ESP_LOGE(TAG, "%s", error_msg);
          cJSON_Delete(root);
        } else {
          // 获取server_info中的配置信息
          cJSON *data = cJSON_GetObjectItem(root, "data");
          if (data != NULL) {
            cJSON *server_info = cJSON_GetObjectItem(data, "server_info");
            if (server_info != NULL) {
              cJSON *id = cJSON_GetObjectItem(server_info, "id");
              cJSON *key = cJSON_GetObjectItem(server_info, "key");
              cJSON *ota = cJSON_GetObjectItem(server_info, "ota");
              cJSON *url = cJSON_GetObjectItem(server_info, "url");

              if (url != NULL && cJSON_IsString(url) &&
                  strlen(url->valuestring) > 0) {
                if (s_bdrtc.config.config_server_url) free((void *)s_bdrtc.config.config_server_url);
                s_bdrtc.config.config_server_url = strdup(url->valuestring);
                ESP_LOGI(TAG, "使用OTA返回的url: %s", url->valuestring);
              }

              if (id != NULL && cJSON_IsString(id)) {
                if (s_bdrtc.config.appid) free((void *)s_bdrtc.config.appid);
                s_bdrtc.config.appid = strdup(id->valuestring);
                ESP_LOGI(TAG, "使用OTA返回的appid: %s", id->valuestring);
              }

              if (key != NULL && cJSON_IsString(key)) {
                if (s_bdrtc.config.license_key) free((void *)s_bdrtc.config.license_key);
                s_bdrtc.config.license_key = strdup(key->valuestring);
                ESP_LOGI(TAG, "使用OTA返回的license_key: %s", key->valuestring);
              }

              // 检查是否存在ota字段，如果存在则启动OTA任务
              if (ota != NULL && cJSON_IsString(ota) &&
                  strlen(ota->valuestring) > 0) {
                ESP_LOGI(TAG, "检测到OTA升级包: %s", ota->valuestring);

                // 触发OTA升级
                esp_err_t ota_result = ota_start(ota->valuestring);
                if (ota_result == ESP_OK) {
                  ESP_LOGI(TAG, "OTA升级已触发，等待升级完成...");
                  // 原地等待OTA升级完成，升级成功后会自动重启
                  int wait_count = 0;
                  while (wait_count < 12) {
                    audio_playback_play("file:///spiffs/wait_upgrade.mp3");
                    vTaskDelay(9000 / portTICK_PERIOD_MS);
                    wait_count++;
                  }
                  ESP_LOGE(TAG, "OTA升级超时，继续启动");
                } else {
                  ESP_LOGE(TAG, "OTA升级触发失败: %s",
                           esp_err_to_name(ota_result));
                }
              }
            }
          }
          cJSON_Delete(root);
        }
      } else {
        cJSON_Delete(root);
      }
    }
  } else {
    ESP_LOGW(TAG, "OTA检查失败，使用默认配置");
    if (s_bdrtc.config.license_key) free((void *)s_bdrtc.config.license_key);
    s_bdrtc.config.license_key = strdup("");
  }
  free(ota_response);

  s_bdrtc.config.workflow = CONFIG_BAIDU_WORKFLOW;
  // s_bdrtc.config.license_key ="";
  s_bdrtc.config.instance_id = CONFIG_BAIDU_INSTANCE_ID;

  BaiduChatAgentEvent events = {
      .onError = onErrorCallback,
      .onCallStateChange = onCallStateChangeCallback,
      .onConnectionStateChange = onConnectionStateChangeCallback,
      .onUserAsrSubtitle = onUserAsrSubtitleCallback,
      .onAIAgentSubtitle = onAIAgentSubtitle,
      .onAIAgentSpeaking = onAIAgentSpeaking,
      .onFunctionCall = onFunctionCall,
      .onAudioPlayerOp = onAudioPlayerOp,
      .onMediaSetup = onMediaSetup,
      .onAudioData = onAudioData,
      .onVideoData = onVideoData,
      .onLicenseResult = onLicenseResult,
  };

  s_bdrtc.engine = baidu_create_chat_agent_engine(&events);
  if (s_bdrtc.engine == NULL) {
    ESP_LOGE(TAG, "Engine initialization failed.");
    return ESP_FAIL;
  }

  AgentEngineParams agentParams;
  setUserParameters(&agentParams);

  ESP_LOGI(TAG, "Logging in...");
  int ret = baidu_chat_agent_engine_init(s_bdrtc.engine, &agentParams);
  if (ret != 200) {
    ESP_LOGE(TAG, "Failed to log in. Error code: %d", ret);
    baidu_chat_agent_engine_destroy(s_bdrtc.engine);
    s_bdrtc.engine = NULL;
    return ESP_FAIL;
  }
  baidu_chat_agent_engine_call(s_bdrtc.engine);
  xEventGroupWaitBits(s_bdrtc.join_event, JOIN_EVENT_BIT, pdTRUE, pdTRUE,
                      portMAX_DELAY);
  ESP_LOGI(TAG, "join room success");

  return ESP_OK;
}

EventGroupHandle_t brtc_get_wakeup_event_handle(void) {
  return s_bdrtc.wakeup_event;
}

static void playback_event_callback_fn(audio_player_state_t state, void *ctx) {
  s_bdrtc.playback_state = state;
  ESP_LOGI(TAG, "Playback state changed: %d", state);
}

static void recorder_event_callback_fn(void *event, void *ctx) {
  esp_gmf_afe_evt_t *afe_evt = (esp_gmf_afe_evt_t *)event;
  switch (afe_evt->type) {
  case ESP_GMF_AFE_EVT_WAKEUP_START:
    ESP_LOGI(TAG, "wakeup start");
    break;
  case ESP_GMF_AFE_EVT_WAKEUP_END:
    ESP_LOGI(TAG, "wakeup end");
    break;
  case ESP_GMF_AFE_EVT_VAD_START:
    ESP_LOGD(TAG, "vad start");
    break;
  case ESP_GMF_AFE_EVT_VAD_END:
    ESP_LOGD(TAG, "vad end");
    break;
  case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT:
    ESP_LOGI(TAG, "vcmd detect timeout");
    break;
  default:
    break;
  }
}

void audio_pipe_open() {
  basic_board_periph_t periph = {0};
  audio_manager_config_t config = {0};

  basic_board_init(&periph);

  config.play_dev = periph.play_dev;
  config.rec_dev = periph.rec_dev;

  strcpy(config.mic_layout, periph.mic_layout);
  config.board_sample_rate = periph.sample_rate;
  config.board_bits = periph.sample_bits;
  config.board_channels = periph.channels;
  audio_manager_init(&config);

  ESP_LOGI(TAG, "play_dev: %p, rec_dev: %p", config.play_dev, config.rec_dev);

  int ret = 0;

  esp_codec_dev_sample_info_t fs = {
      .sample_rate = 16000,
      .channel = 2,
      .bits_per_sample = 16,
  };
  esp_codec_dev_open(config.rec_dev, &fs);
  esp_codec_dev_open(config.play_dev, &fs);

  // 保存播放和录音设备句柄并初始化音量
  s_bdrtc.play_dev_handle = config.play_dev;
  s_bdrtc.rec_dev_handle = config.rec_dev;
  s_bdrtc.current_volume = 50.0f; // 初始音量
  s_bdrtc.is_playing = false;     // 初始播放状态为停止

  esp_codec_dev_set_out_vol(config.play_dev, (int)s_bdrtc.current_volume);
  esp_codec_dev_set_in_gain(config.rec_dev, 50.0);
  // esp_codec_dev_set_in_channel_gain(config.rec_dev, BIT(0), 16.0);
  // esp_codec_dev_set_in_channel_gain(config.rec_dev, BIT(1), 11.0);

  audio_playback_config_t playback_cfg = DEFAULT_AUDIO_PLAYBACK_CONFIG();
  playback_cfg.event_cb = playback_event_callback_fn;
  audio_playback_open(&playback_cfg);

  audio_prompt_config_t prompt_cfg = DEFAULT_AUDIO_PROMPT_CONFIG();
  // 优化提示音播放配置，解决RTC启动后的卡顿问题
  prompt_cfg.prompt_task_config.task_stack = 4096; // 增加栈4096字节
  prompt_cfg.prompt_task_config.task_prio = 8;     // 优先级8，高于RTC任务(5)
  prompt_cfg.prompt_task_config.task_core =
      1; // 绑定到core 1，避免与RTC任务冲突
  audio_prompt_open(&prompt_cfg);

  av_processor_encoder_config_t encoder_cfg = {0};
  encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
  encoder_cfg.params.pcm.audio_info.sample_rate = 16000;
  encoder_cfg.params.pcm.audio_info.sample_bits = 16;
  encoder_cfg.params.pcm.audio_info.channels = 1;
  encoder_cfg.params.pcm.audio_info.frame_duration = 20;

  audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
  memcpy((void *)&recorder_cfg.encoder_cfg, &encoder_cfg,
         sizeof(av_processor_encoder_config_t));
  recorder_cfg.recorder_event_cb = recorder_event_callback_fn;
  audio_recorder_open(&recorder_cfg);

  av_processor_decoder_config_t feeder_cfg = {0};
  feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
  feeder_cfg.params.pcm.audio_info.sample_rate = 16000;
  feeder_cfg.params.pcm.audio_info.sample_bits = 16;
  feeder_cfg.params.pcm.audio_info.channels = 1;
  feeder_cfg.params.pcm.audio_info.frame_duration = 20;

  audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
  memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
         sizeof(av_processor_decoder_config_t));
  audio_feeder_open(&feeder_config);
  audio_feeder_run();

  // audio_processor_mixer_close();
  audio_processor_mixer_open();
}

void audio_pipe_sip_mode(bool enable) {
  audio_recorder_close();
  audio_feeder_close();

  av_processor_encoder_config_t encoder_cfg = {0};
  av_processor_decoder_config_t feeder_cfg = {0};
  if (enable) {
    encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
    encoder_cfg.params.pcm.audio_info.sample_rate = 8000;
    encoder_cfg.params.pcm.audio_info.sample_bits = 16;
    encoder_cfg.params.pcm.audio_info.channels = 1;
    encoder_cfg.params.pcm.audio_info.frame_duration = 20;
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_G711A;
    feeder_cfg.params.pcm.audio_info.sample_rate = 8000;
    feeder_cfg.params.pcm.audio_info.sample_bits = 16;
    feeder_cfg.params.pcm.audio_info.channels = 1;
    feeder_cfg.params.pcm.audio_info.frame_duration = 20;
  } else {
    encoder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    encoder_cfg.params.pcm.audio_info.sample_rate = 16000;
    encoder_cfg.params.pcm.audio_info.sample_bits = 16;
    encoder_cfg.params.pcm.audio_info.channels = 1;
    encoder_cfg.params.pcm.audio_info.frame_duration = 20;
    feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_PCM;
    feeder_cfg.params.pcm.audio_info.sample_rate = 16000;
    feeder_cfg.params.pcm.audio_info.sample_bits = 16;
    feeder_cfg.params.pcm.audio_info.channels = 1;
    feeder_cfg.params.pcm.audio_info.frame_duration = 20;
  }

  audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
  memcpy((void *)&recorder_cfg.encoder_cfg, &encoder_cfg,
         sizeof(av_processor_encoder_config_t));
  recorder_cfg.recorder_event_cb = recorder_event_callback_fn;
  audio_recorder_open(&recorder_cfg);

  audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();
  memcpy((void *)&feeder_config.decoder_cfg, &feeder_cfg,
         sizeof(av_processor_decoder_config_t));
  audio_feeder_open(&feeder_config);
  audio_feeder_run();
}

static void audio_data_read_task(void *pv) {
  uint8_t *read_buffer = esp_gmf_oal_calloc(1, AUDIO_BUFFER_SIZE);
  if (!read_buffer) {
    ESP_LOGE(TAG, "Failed to allocate audio buffer");
    vTaskDelete(NULL);
    return;
  }

  int ret = 0;
  uint32_t log_counter = 0;
  bool last_rtc_stream_up = false;
  uint32_t idle_count = 0;
  uint32_t consecutive_failures = 0;

  ESP_LOGI(TAG, "音频数据读取任务已启动");

  while (s_bdrtc.byte_rtc_running) {
    // 根据模式选择缓冲区大小

    // 优化：在 SIP 模式下快速退出，避免长时间阻塞
    if (is_sip_mode) {
      if (log_counter % 100 == 0) {
        ESP_LOGW(TAG, "音频读取任务在 SIP 模式下等待");
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    int buffer_size =
        s_bdrtc.is_rtc_mode ? RTC_AUDIO_BUFFER_SIZE : AUDIO_BUFFER_SIZE;

    ret = audio_recorder_read_data(read_buffer, buffer_size);
    log_counter++;

    // 增强日志：在状态变化或连续失败时打印详细信息
    if (s_bdrtc.is_rtc_stream_up != last_rtc_stream_up) {
      ESP_LOGI(TAG,
               "RTC 流状态变化 - ret: %d, is_connected: %d, is_rtc_mode: "
               "%d, is_rtc_stream_up: %d",
               ret, s_bdrtc.is_connected, s_bdrtc.is_rtc_mode,
               s_bdrtc.is_rtc_stream_up);
      last_rtc_stream_up = s_bdrtc.is_rtc_stream_up;
      consecutive_failures = 0; // 状态变化时重置失败计数
    } else if (ret <= 0) {
      consecutive_failures++;
      // 连续失败时增加日志频率
      if (consecutive_failures <= 10 || consecutive_failures % 50 == 0) {
        ESP_LOGW(
            TAG,
            "音频读取失败 #%d - ret: %d, buffer_size: %d, is_rtc_mode: %d, "
            "is_connected: %d, is_rtc_stream_up: %d",
            consecutive_failures, ret, buffer_size, s_bdrtc.is_rtc_mode,
            s_bdrtc.is_connected, s_bdrtc.is_rtc_stream_up);
      }
    } else if (consecutive_failures > 0) {
      // 恢复后打印日志
      ESP_LOGI(TAG, "音频读取恢复正常，连续失败次数：%d", consecutive_failures);
      consecutive_failures = 0;
    }

    // 定期打印统计信息
    if (log_counter % 1000 == 0) {
      ESP_LOGI(TAG, "音频读取统计 - 总循环：%lu, 连续失败：%lu, RTC 流状态：%d",
               (unsigned long)log_counter, (unsigned long)consecutive_failures,
               s_bdrtc.is_rtc_stream_up);
    }

    if (ret > 0 && s_bdrtc.is_connected) {
      if (s_bdrtc.is_rtc_mode && s_bdrtc.rtc_client) {
        // RTC 模式下，音频已经由编码器处理为 8000Hz 的 PCMU 格式
        // 直接发送编码后的数据
        brtc_send_audio(s_bdrtc.rtc_client, read_buffer, ret);
      } else {
        baidu_chat_agent_engine_send_audio(s_bdrtc.engine, read_buffer, ret);
      }
      idle_count = 0;
      consecutive_failures = 0;
    } else {
      // 未连接或读取失败时，增加延时让出 CPU
      idle_count++;
      if (idle_count > 10) {
        vTaskDelay(pdMS_TO_TICKS(5)); // 长时间空闲时增加延时
        idle_count = 0;
      } else {
        vTaskDelay(pdMS_TO_TICKS(1)); // 短暂延时
      }
    }
  }

  ESP_LOGW(TAG, "音频数据读取任务已停止");
  esp_gmf_oal_free(read_buffer);
  vTaskDelete(NULL);
}

static void audio_player_ctrl_task(void *pv) {
  audio_player_ctrl_event_t event;
  while (s_bdrtc.data_proc_running) {

    if (xQueueReceive(s_bdrtc.audio_player_queue, &event, pdMS_TO_TICKS(100)) ==
        pdPASS) {
      ESP_LOGI(TAG, "audio_player_ctrl_task, url: %s, start: %d", event.url,
               event.start);
      if (event.start) {
        // 设置播放状态为播放中
        brtc_set_playing_state(true);
        audio_playback_play(event.url);
        esp_gmf_oal_free(event.url);
      } else {
        // 设置播放状态为停止
        brtc_set_playing_state(false);
        audio_playback_stop();
      }
    }
  }
  vTaskDelete(NULL);
}

static void button_wakeup_task(void *pv) {
  ESP_LOGI(TAG, "按键唤醒处理任务已启动");

  while (s_bdrtc.byte_rtc_running) {
    // 等待按键唤醒事件，超时时间为1000ms
    EventBits_t bits =
        xEventGroupWaitBits(s_bdrtc.wakeup_event, BUTTON_WAKEUP_BIT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(100));

    if (bits & BUTTON_WAKEUP_BIT) {
      ESP_LOGI(TAG, "检测到按键唤醒事件，开始处理...");

      // 创建唤醒信息
      esp_gmf_afe_wakeup_info_t wakeup_info = {
          .data_volume = -20.0f,   // 设置一个合理的音量值
          .wake_word_index = 0,    // 按键唤醒没有特定的唤醒词索引
          .wakenet_model_index = 0 // 按键唤醒没有特定的模型索引
      };

      // 构造AFE唤醒事件
      esp_gmf_afe_evt_t wakeup_event = {.type = ESP_GMF_AFE_EVT_WAKEUP_START,
                                        .event_data = &wakeup_info,
                                        .data_len =
                                            sizeof(esp_gmf_afe_wakeup_info_t)};

      // 调用回调函数来处理唤醒事件
      recorder_event_callback_fn(&wakeup_event, NULL);

      ESP_LOGI(TAG, "按键唤醒事件处理完成");
    }
  }

  ESP_LOGI(TAG, "按键唤醒处理任务已停止");
  vTaskDelete(NULL);
}

esp_err_t brtc_init(void) {
  esp_err_t ret = ESP_OK;

  s_bdrtc.join_event = xEventGroupCreate();
  s_bdrtc.wait_destory_event = xEventGroupCreate();
  s_bdrtc.wakeup_event = xEventGroupCreate();
  s_bdrtc.is_connected = false;
  s_bdrtc.audio_player_queue =
      xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_player_ctrl_event_t));
  // audio_pipe_open();
  // audio_playback_play("file:///spiffs/wifi_wait.mp3");
  if (!s_bdrtc.join_event || !s_bdrtc.wait_destory_event ||
      !s_bdrtc.wakeup_event || !s_bdrtc.audio_player_queue) {
    ESP_LOGE(TAG, "Failed to create events or queue");
    ret = ESP_FAIL;
    goto cleanup;
  }
  extern void* wifi_event_group; // EventGroupHandle_t
  extern uint8_t internet_connected;
  #define CONNECTED_BIT BIT0

  typedef enum {
      BRTC_NET_INIT,
      BRTC_NET_WAIT,
      BRTC_NET_WIFI_OK,
      BRTC_NET_4G_OK,
      BRTC_NET_TIMEOUT
  } brtc_net_state_t;

  brtc_net_state_t net_state = BRTC_NET_INIT;
  int wait_cnt = 0;

  while (net_state == BRTC_NET_INIT || net_state == BRTC_NET_WAIT) {
      switch (net_state) {
          case BRTC_NET_INIT:
              ESP_LOGI(TAG, "BRTC 网络状态机: 开始等待有效网络...");
              net_state = BRTC_NET_WAIT;
              break;
          case BRTC_NET_WAIT:
              if (wifi_event_group != NULL) {
                  EventBits_t bits = xEventGroupGetBits(wifi_event_group);
                  if (bits & CONNECTED_BIT) {
                      net_state = BRTC_NET_WIFI_OK;
                      break;
                  }
              }
              
              if (internet_connected == 1) {
                  net_state = BRTC_NET_4G_OK;
                  break;
              }

              vTaskDelay(pdMS_TO_TICKS(1000));
              wait_cnt++;
              if (wait_cnt >= 90) {
                  net_state = BRTC_NET_TIMEOUT;
              }
              break;
          default:
              break;
      }
  }

  if (net_state == BRTC_NET_WIFI_OK) {
      ESP_LOGI(TAG, "BRTC 网络状态机: 确认使用 WiFi 网络");
  } else if (net_state == BRTC_NET_4G_OK) {
      ESP_LOGI(TAG, "BRTC 网络状态机: 确认使用 4G 网络");
  } else {
      ESP_LOGW(TAG, "BRTC 网络状态机: 等待网络超时，继续执行...");
  }

  // 等待NTP时间同步完成，避免时间变化导致百度RTC断连
  // ESP_LOGI(TAG, "等待NTP时间同步完成...");
  // extern bool wait_for_ntp_sync(uint32_t timeout_ms);
  // bool ntp_sync_ok = wait_for_ntp_sync(30 * 1000); // 等待NTP时间同步完成
  // if (ntp_sync_ok) {
  //   ESP_LOGI(TAG, "NTP时间同步完成，继续初始化百度RTC");
  // } else {
  //   ESP_LOGW(TAG,
  //            "NTP时间同步超时，继续初始化百度RTC（可能存在时间不准确问题）");
  // }

  // audio_prompt_config_t prompt_cfg = DEFAULT_AUDIO_PROMPT_CONFIG();
  // audio_prompt_open(&prompt_cfg);
  // audio_prompt_play("file:///spiffs/wifi_ok.mp3");//播放wifi连接成功//
  // 播放wifi连接成功提示音
  ret = brtc_create();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create brtc");
    ret = ESP_FAIL;
    goto cleanup;
  }

  // audio_pipe_open();
  s_bdrtc.byte_rtc_running = true;
  s_bdrtc.data_proc_running = true;
  esp_gmf_oal_thread_create(&s_bdrtc.read_thread, "audio_data_read_task",
                            audio_data_read_task, NULL, THREAD_STACK_SIZE,
                            THREAD_PRIORITY, true, 0);
  esp_gmf_oal_thread_create(&s_bdrtc.play_ctrl_thread, "audio_player_ctrl_task",
                            audio_player_ctrl_task, NULL, THREAD_STACK_SIZE,
                            THREAD_PRIORITY, true, 0);
  esp_gmf_oal_thread_create(&s_bdrtc.button_wakeup_thread, "button_wakeup_task",
                            button_wakeup_task, NULL, THREAD_STACK_SIZE,
                            THREAD_PRIORITY, true, 0);

  return ESP_OK;

cleanup:
  if (s_bdrtc.join_event) {
    vEventGroupDelete(s_bdrtc.join_event);
    s_bdrtc.join_event = NULL;
  }
  if (s_bdrtc.wait_destory_event) {
    vEventGroupDelete(s_bdrtc.wait_destory_event);
    s_bdrtc.wait_destory_event = NULL;
  }
  if (s_bdrtc.wakeup_event) {
    vEventGroupDelete(s_bdrtc.wakeup_event);
    s_bdrtc.wakeup_event = NULL;
  }
  if (s_bdrtc.audio_player_queue) {
    vQueueDelete(s_bdrtc.audio_player_queue);
    s_bdrtc.audio_player_queue = NULL;
  }
  return ret;
}

esp_err_t brtc_deinit(void) {
  s_bdrtc.byte_rtc_running = false;
  s_bdrtc.data_proc_running = false;
  
  // 等待后台任务(如 audio_player_ctrl_task, button_wakeup_task)检测到标志退出，防止它们还在等待队列/事件组时直接被销毁导致崩溃
  vTaskDelay(pdMS_TO_TICKS(150));

  if (s_bdrtc.audio_player_queue) {
    vQueueDelete(s_bdrtc.audio_player_queue);
    s_bdrtc.audio_player_queue = NULL;
  }
  if (s_bdrtc.join_event) {
    vEventGroupDelete(s_bdrtc.join_event);
    s_bdrtc.join_event = NULL;
  }
  if (s_bdrtc.wait_destory_event) {
    vEventGroupDelete(s_bdrtc.wait_destory_event);
    s_bdrtc.wait_destory_event = NULL;
  }
  if (s_bdrtc.wakeup_event) {
    vEventGroupDelete(s_bdrtc.wakeup_event);
    s_bdrtc.wakeup_event = NULL;
  }

  // 释放动态分配的userId内存
  if (s_bdrtc.config.userId) {
    free((void *)s_bdrtc.config.userId);
    s_bdrtc.config.userId = NULL;
  }
  
  if (s_bdrtc.config.config_server_url) {
    free((void *)s_bdrtc.config.config_server_url);
    s_bdrtc.config.config_server_url = NULL;
  }
  if (s_bdrtc.config.appid) {
    free((void *)s_bdrtc.config.appid);
    s_bdrtc.config.appid = NULL;
  }
  if (s_bdrtc.config.license_key) {
    free((void *)s_bdrtc.config.license_key);
    s_bdrtc.config.license_key = NULL;
  }

  byte_rtc_engine_destroy();

  return ESP_OK;
}

void brtc_sip_error(void) {
  baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine,
                                           "网络不稳定，请稍后重试");
}

void brtc_sip_end(void) {
  baidu_chat_agent_engine_send_text_to_TTS(s_bdrtc.engine, "通话结束");
}
