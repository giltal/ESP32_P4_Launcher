idf_component_register(
    SRCS "lx_input.c" "lx_input_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES papp_abi
    PRIV_REQUIRES bsp esp_timer usb
)
