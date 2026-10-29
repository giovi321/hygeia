idf_component_register(
    SRCS
        "src/clock.c"
        "src/clock_valid.c"
        "src/rtc_codec.c"
        "src/pcf85063.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        hy_core
        esp_driver_i2c
        esp_driver_gpio
    PRIV_REQUIRES
        nvs_flash
)

# Build epoch (UTC seconds) taken when CMake configures; a time before it is implausible.
# It changes whenever the project is reconfigured.
string(TIMESTAMP HY_BUILD_EPOCH "%s" UTC)
set_source_files_properties("src/clock.c" PROPERTIES
    COMPILE_DEFINITIONS "HY_BUILD_EPOCH=${HY_BUILD_EPOCH}LL")
