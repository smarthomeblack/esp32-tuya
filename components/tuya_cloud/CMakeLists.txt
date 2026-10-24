idf_component_register(
    SRCS
        "src/credentials.cc"
        "src/http_client.cc"
        "src/token_manager.cc"
        "src/customer_api.cc"
        "src/login_control.cc"
        "src/cloud_auth.cc"
        "src/time_sync.cc"
        "src/pipeline_self_test.cc"
        "src/openapi_client.cc"
        "src/openapi_self_test.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "tuya_crypto"     # AES-GCM pack/unpack, HMAC, MD5, UUID, JsonWriter
        "sys"             # sys::Settings (NVS credentials)
        "esp_http_client" # blocking HTTP/HTTPS transport
        "mbedtls"         # esp_crt_bundle (TLS CA verification)
        "esp_netif"       # esp_netif_sntp (SNTP time sync)
    PRIV_REQUIRES
        "esp_timer" # esp_timer_get_time (request timing; not a common component)
    # cJSON arrives via the managed dependency in idf_component.yml.
    # esp_log, esp_hw_support and freertos are common components (auto-required).
)
