idf_component_register(
    SRCS ""
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client esp_https_ota esp_netif esp_wifi mbedtls esp_event freertos esp_psram nvs_flash protocol_examples_common driver spi_flash
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/lib/libota_component.a")