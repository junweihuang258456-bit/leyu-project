idf_component_register(
    SRCS "uart_comm.c" "system_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_common esp_timer main
)