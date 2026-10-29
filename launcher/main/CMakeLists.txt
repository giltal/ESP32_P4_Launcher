idf_component_register(
    SRCS "main.c" "status_screen.c"
    INCLUDE_DIRS "."
    REQUIRES board bsp lx_loader lx_devlink lx_gfx lx_comp lx_audio lx_input lx_common nvs_flash heap esp_mm esp_timer
)
