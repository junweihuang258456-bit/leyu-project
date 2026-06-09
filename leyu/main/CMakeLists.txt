idf_component_register(
    SRCS
        "battery_module.c"
        "usb_rndis_4g_module.c"
        "ringtone_player.c"
        "call_manager.c"
        "network_manager.c"
        "button_handler.c"
        "mqtt5_client.c"
        "mqtt_example.c"
        "mqtt_msg_parser.c"
        "mqtt_msg_builder.c"
        "main.c"
        "brtc_app.c"
        "basic_board.c"
        "blufi_example_main.c"
        "blufi_security.c"
        "blufi_init.c"
        "uart_echo_example_main.c"
        "contact_manager.c"
        "sip/sip_service.c"
        "sip/av_system_app.c"
        "rgb_led/xgb-1815.c"
        "rgb_led/led_control.c"
        "console/console.c"
    INCLUDE_DIRS
        "."
        "sip"
        "rgb_led"
        "console"
    # RESOURCES
        
    EMBED_TXTFILES
        ${project_dir}/server_certs/ca_cert.pem
    )

# 为音频文件创建SPIFFS镜像，放入spiffs_data分区
spiffs_create_partition_image(spiffs_data ../spiffs FLASH_IN_PROJECT)

# Force CMake to regenerate spiffs.bin
