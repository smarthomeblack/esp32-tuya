idf_component_register(
    SRCS
        "src/tuya_mqtt_client.cc"
        "src/mqtt_message.cc"
        "src/mqtt_message_test.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "hub_core"     # DeviceRegistry / Device / Strategies (report conversion),
                       # public parse wrappers (bindUser refetch)
        "tuya_cloud"   # CustomerApi / constants / ApiResponse / HttpMethod
    PRIV_REQUIRES
        "espressif__mqtt"  # esp-mqtt client (managed); public headers stay
                           # esp-mqtt-free via an opaque forward declaration
        "mbedtls"     # esp_crt_bundle_attach (broker TLS verification)
        "esp_timer"   # esp_timer_get_time (config-refresh deadline)
        "tuya_crypto" # JsonWriter (linkId / devIds plaintexts), Uuid4
    # cJSON: idf_component.yml dependency (PRIVATE, src-only includes).
)
