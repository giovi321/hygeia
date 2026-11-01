idf_component_register(SRCS "app_main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES espressif__cjson hy_core hy_board hy_clock hy_store hy_sched hy_ui hy_audio hy_power hy_motion hy_net hy_web hy_update
                       PRIV_REQUIRES app_update esp_app_format nvs_flash esp_driver_usb_serial_jtag)
