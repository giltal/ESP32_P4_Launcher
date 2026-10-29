idf_component_register(
    SRCS "lx_app.c" "lx_services.c" "lx_svc_disp.c" "lx_svc_audio.c" "lx_svc_input.c" "lx_svc_thrd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES papp_abi esp_mm
    PRIV_REQUIRES lx_common bsp lx_comp lx_audio lx_input esp_timer heap esp_system
)
