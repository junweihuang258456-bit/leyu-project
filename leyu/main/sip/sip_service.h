#ifndef _SIP_SERVICE_H
#define _SIP_SERVICE_H

#include "esp_rtc.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// SIP 音频结构体
typedef struct {
  bool is_sip_mode;
  bool is_sip_calling;
  bool is_sip_incoming;
  bool is_sip_begin;
} sip_audio_t;

esp_rtc_handle_t sip_service_start(const char *uri);
int sip_service_stop(esp_rtc_handle_t esp_sip);

void sip_event_handler_task(void *pvParameters);
extern bool is_sip_event_calling;
extern bool is_sip_event_incoming;
extern bool is_sip_incoming;
extern bool is_sip_begin;

#ifdef __cplusplus
}
#endif

#endif
