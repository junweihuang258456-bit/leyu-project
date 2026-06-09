# http_ota 静态库组件

idf_component_register(
    SRCS ""
    INCLUDE_DIRS "include"
    REQUIRES mbedtls esp_event esp_netif freertos esp_psram
)

# 添加静态库链接
target_link_libraries(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/lib/libhttp_ota.a")
