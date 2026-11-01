idf_component_register(SRCS "lx_update.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update esp_app_format bootloader_support log nvs_flash)
