idf_component_register(
    SRCS "board.c" "board_display.c" "board_touch.c" "board_audio.c" "board_buttons.c" "board_rtc.c"
    INCLUDE_DIRS "."
    REQUIRES bsp fatfs vfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc esp_lcd esp_hw_support esp_mm esp_driver_i2s esp_adc esp_timer
    WHOLE_ARCHIVE       # implements the bsp.h contract: link every file, callers sit in any component
)

# Hardware revision: idf.py -DPICOHELD2_REV=13 ... (13 = v1.3, 14 = v1.4, default set in board_config.h)
if(DEFINED PICOHELD2_REV)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PICOHELD2_REV=${PICOHELD2_REV})
endif()
# Panel mounting: idf.py -DPICOHELD2_LCD_ROT=270 ... if the picture comes out upside down
if(DEFINED PICOHELD2_LCD_ROT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PICOHELD2_LCD_ROT=${PICOHELD2_LCD_ROT})
endif()
