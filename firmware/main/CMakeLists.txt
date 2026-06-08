idf_component_register(
    SRCS "mavlink_bridge.c" "main.c" "camera.c" "usb_video.c"
    INCLUDE_DIRS "."
    REQUIRES mavlink esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
