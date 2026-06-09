#ifndef MQTT_MSG_BUILDER_H
#define MQTT_MSG_BUILDER_H
#include <stdbool.h>

void send_device_command_response(const char *ori_serial, const char *busi, const char *code, const char *msg, const char *cmd_res);
bool send_heartbeat_packet(const char *status);
bool send_contact_sync_data(void);
void send_voice_call_notification(const char *phone, const char *status);
void send_sms_notification(const char *phone, const char *content);

#endif
