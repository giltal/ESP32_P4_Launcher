idf_component_register(
    SRCS "main.c" "home.c"
    INCLUDE_DIRS "."
    REQUIRES board bsp lx_loader lx_devlink lx_comp lx_audio lx_input lx_common lvgl
             nvs_flash heap esp_mm esp_timer esp_driver_tsens fatfs vfs
)
