#ifndef MQTT_MSG_PARSER_H
#define MQTT_MSG_PARSER_H

void parse_service_status(const char *json_str);
void parse_sip_info(const char *json_str);
void parse_server_command_response(const char *json_str);
void parse_voice_intercept_rules(const char *json_str);
void parse_voice_call_command(const char *json_str);
void parse_sms_send_command(const char *json_str);
void parse_at_command(const char *json_str);
void parse_key_bind_message(const char *json_str);
void parse_ota_upgrade_command(const char *json_str);
void parse_contact_sync_message(const char *json_str);

#endif
