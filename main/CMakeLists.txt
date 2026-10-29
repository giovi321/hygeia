idf_component_register(SRCS "app_main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES hy_core hy_board hy_clock hy_store hy_sched hy_ui hy_audio hy_power
                       PRIV_REQUIRES esp_app_format nvs_flash)
