idf_component_register(
    SRCS
        "src/web_server.cc"
        "src/rest_api.cc"
        "src/rest_api_json.cc"
        "src/rest_api_marshal.cc"
        "src/rest_api_devices.cc"
        "src/rest_api_discovery.cc"
        "src/rest_api_ota.cc"
        "src/rest_api_rooms.cc"
        "src/api_auth.cc"
        "src/ws_handler.cc"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "esp_http_server"   # httpd lifecycle + URI handlers + WebSocket
        "hub_core"          # DeviceRegistry (domain model)
        "hub_services"      # CommandService / DiscoveryService
        "tuya_cloud"        # CloudAuthManager (auth status, login, unlink)
        "tuya_mqtt"         # TuyaMqttClient (status snapshot + events)
        "sys"               # WifiStation / SystemInfo / Settings / DeviceState
        "web_assets"        # WAS1 asset store (FindAsset + upload path)
    PRIV_REQUIRES
        "espressif__mdns"   # managed component (espressif/mdns, declared in
                            # idf_component.yml) — mDNS announcement
        "esp_timer"         # esp_timer_get_time (WS event timestamps)
        "lwip"              # getpeername for the per-IP auth lockout (src only)
        "tuya_crypto"       # api_auth Sha256Hex (PSA Crypto — the legacy
                            # mbedtls_sha256 API is private in IDF v6)
        "app_update"        # esp_ota_* + esp_app_desc/esp_image_header
                            # (esp_app_format/esp_partition propagate)
    # cJSON arrives via idf_component.yml (espressif/cjson, PRIVATE):
    # only src/*.cc includes <cJSON.h>, never a public header.
)
