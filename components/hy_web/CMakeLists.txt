idf_component_register(
    SRCS
        "src/server.c"
        "src/bridge.c"
        "src/session.c"
        "src/ws.c"
        "src/h_core.c"
        "src/h_util.c"
        "src/h_meds.c"
        "src/h_log.c"
        "src/h_settings.c"
        "src/h_wifi.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        hy_core
        esp_common
    PRIV_REQUIRES
        esp_http_server
        esp_app_format
        esp_timer
        freertos
        lwip
        mbedtls
        nvs_flash
        hy_audio
        hy_board
        hy_clock
        hy_net
        hy_power
        hy_sched
        hy_store
        lvgl__lvgl
        espressif__cjson
    WHOLE_ARCHIVE
)

# The web UI bundle: tools/web_bundle.py gzips web/ into a C table whenever a file under web/ or the script changes.
# The script fails the build when the gzipped total is above 200 KB.
idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(python PYTHON)
file(GLOB_RECURSE web_sources CONFIGURE_DEPENDS "${project_dir}/web/*")
set(web_assets_c "${CMAKE_CURRENT_BINARY_DIR}/web_assets.c")
add_custom_command(
    OUTPUT "${web_assets_c}"
    COMMAND "${python}" "${project_dir}/tools/web_bundle.py" --src "${project_dir}/web" --out "${web_assets_c}"
    DEPENDS ${web_sources} "${project_dir}/tools/web_bundle.py"
    COMMENT "Bundling the web UI"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${web_assets_c}")
