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
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espressif__cjson
)

# The action module's medicine arrays (HY_MAX_STORED entries each) live in PSRAM
# (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY); the host build leaves HY_EXT_BSS empty.
target_compile_definitions(${COMPONENT_LIB} PRIVATE [[HY_EXT_BSS=__attribute__((section(".ext_ram.bss")))]])
