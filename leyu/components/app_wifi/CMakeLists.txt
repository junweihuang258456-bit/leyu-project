set(REQUIRES esp_wifi esp_eth esp_timer nvs_flash spiffs esp-tls vfs lwip esp_http_server)

if(CONFIG_EXAMPLE_ENABLE_WEB_ROUTER)
    idf_component_register(SRC_DIRS "." "router_web"
                    INCLUDE_DIRS "." "include" "router_web"
                    PRIV_REQUIRES ${REQUIRES})
    # 将Web界面文件放入专门的wifi_web分区，避免与AI模型和音频文件分区冲突
    spiffs_create_partition_image(wifi_web ./spiffs FLASH_IN_PROJECT)
else()
    idf_component_register(SRC_DIRS "."
                    INCLUDE_DIRS "." "include"
                    PRIV_REQUIRES ${REQUIRES})
endif()
