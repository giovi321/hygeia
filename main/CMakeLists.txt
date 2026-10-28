idf_component_register(SRCS "app_main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES hy_board
                       PRIV_REQUIRES esp_app_format)
