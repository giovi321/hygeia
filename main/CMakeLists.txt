idf_component_register(SRCS "app_main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES hy_core hy_board hy_clock hy_store hy_ui hy_audio
                       PRIV_REQUIRES esp_app_format nvs_flash)
