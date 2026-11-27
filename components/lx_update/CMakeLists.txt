idf_component_register(SRCS "lx_update.c" "lx_layout.c" "lx_update_net.c" "lx_http.c" "lx_appstore.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES bsp app_update esp_app_format bootloader_support log nvs_flash spi_flash esp_partition esp_rom
                                 esp_http_client esp-tls mbedtls json)
