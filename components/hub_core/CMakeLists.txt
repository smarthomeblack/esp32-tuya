idf_component_register(
    SRCS
        "src/device_registry.cc"
        "src/presence_status.cc"
        "src/psram_resource.cc"
        "src/strategies.cc"
        "src/parse_helpers.cc"
        "src/parse.cc"
        "src/model_self_test.cc"
        "src/device_store.cc"
        "src/device_store_format.cc"
        "src/device_store_test.cc"
        "src/main_thread.cc"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        "tuya_crypto"    # JsonWriter — src-only
        "heap"           # heap_caps_* PSRAM upstream (psram_resource.cc)
    # cJSON + littlefs: managed PRIVATE deps (idf_component.yml) — public
    # headers stay cJSON-free (parse helpers live in src/).
)
