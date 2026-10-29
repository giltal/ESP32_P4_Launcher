idf_component_register(
    SRCS "lx_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES papp_abi
    PRIV_REQUIRES lx_gfx lx_comp lx_input bsp esp_timer heap vfs
)
