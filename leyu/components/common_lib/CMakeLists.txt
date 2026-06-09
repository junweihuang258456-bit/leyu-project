idf_component_register(
    SRCS ""
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash driver freertos esp_wifi esp_adc driver
)

# 收集所有静态库文件并链接
file(GLOB LIB_FILES "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.a")
target_link_libraries(${COMPONENT_LIB} INTERFACE ${LIB_FILES})