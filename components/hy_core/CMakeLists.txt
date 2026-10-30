idf_component_register(
    SRCS
        "src/med.c"
        "src/medform.c"
        "src/occ.c"
        "src/events.c"
        "src/actions.c"
        "src/codec.c"
        "src/power.c"
        "src/types.c"
        "src/tzlock.c"
        "src/sched.c"
        "src/summary.c"
        "src/nav.c"
        "src/tzdb.c"
        "src/tzdb_data.c"
        "src/chime.c"
        "src/settings.c"
        "src/battery.c"
        "src/gap.c"
        "src/screen.c"
        "src/orient.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espressif__cjson
)

# Large static buffers marked HY_EXT_BSS live in PSRAM (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY). PUBLIC so
# hy_store, hy_sched and hy_ui get it too; the host build leaves HY_EXT_BSS empty (hy/types.h).
target_compile_definitions(${COMPONENT_LIB} PUBLIC [[HY_EXT_BSS=__attribute__((section(".ext_ram.bss")))]])
