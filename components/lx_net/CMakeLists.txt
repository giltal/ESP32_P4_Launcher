idf_component_register(SRCS "lx_net.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip)
