idf_component_register(
    SRCS
        "src/command_service.cc"
        "src/command_service_test.cc"
        "src/dedup_filter.cc"
        "src/discovery.cc"
        "src/discovery_test.cc"
        "src/openapi_command.cc"
        "src/openapi_enrichment.cc"
        "src/openapi_enrichment_test.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "hub_core"     # Device/DeviceRegistry/DpValue in public headers
    PRIV_REQUIRES
        "tuya_cloud"   # CustomerApi/OpenApiClient/time_sync — src only
        "sys"          # sys/settings.h — quota NVS (command_service.cc)
        "tuya_crypto"  # JsonWriter/JsonFields/JsonGuard — src only
    # cJSON: managed PRIVATE dep (idf_component.yml) — src-only includes.
)

# parse_helpers.h stays hub_core-internal (hub_core public headers stay
# cJSON-free); discovery.cc/openapi_enrichment.cc include it src-only.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../hub_core/src")
