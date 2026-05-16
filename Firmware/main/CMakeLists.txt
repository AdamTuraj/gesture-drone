idf_component_register(
    SRCS "mavlink_bridge.c" "main.c" "rpi_wifi.c" "camera.c"
    INCLUDE_DIRS "."
    REQUIRES mavlink esp_driver_gpio esp_driver_uart esp_wifi lwip nvs_flash esp_netif esp_event
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
