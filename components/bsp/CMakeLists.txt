# Board-independent BSP contract. Each board implements it in boards/<id>/board/.
idf_component_register(INCLUDE_DIRS "include" REQUIRES esp_common)
