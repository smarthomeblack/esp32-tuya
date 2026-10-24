idf_component_register(SRCS "main.cc" "application.cc" "application_persist.cc"
                              "application_tasks.cc" "application_transports.cc"
                              "boot_self_test.cc" "nvs_secure_boot.cc"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event nvs_flash nvs_sec_provider esp_timer sys tuya_crypto tuya_cloud hub_core hub_services tuya_mqtt web_server web_assets app_update)
# web_assets: asset self-test gate; app_update: rollback-valid confirm after WebServer::Start.
# nvs_sec_provider: ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND (the scheme itself auto-registers).
