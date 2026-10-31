idf_component_register(
    SRCS
        "src/board.c"
        "src/lv_mem_psram.c"
        "src/qmi8658.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        hy_core
        waveshare__esp32_s3_touch_amoled_2_16
        espressif__esp_lvgl_adapter
        lvgl__lvgl
        esp_lcd
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        esp_pm
)

# Nothing in this component references the LVGL memory backend; only liblvgl does.
# Force the object out of the archive so the linker resolves lv_malloc_core.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")
