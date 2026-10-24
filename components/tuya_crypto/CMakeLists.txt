idf_component_register(
    SRCS
        "src/aes_gcm.cc"
        "src/at_rest_crypto.cc"
        "src/hmac_sha256.cc"
        "src/md5.cc"
        "src/sha256.cc"
        "src/base64.cc"
        "src/uuid.cc"
        "src/json_writer.cc"
        "src/json_guard.cc"
        "src/self_test.cc"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        mbedtls
        esp_security        # esp_hmac (upstream HMAC peripheral, eFuse-keyed)
        efuse               # eFuse key-block burn / purpose read
        bootloader_support  # bootloader_random_* for the pre-WiFi key burn
    # esp_hw_support (esp_fill_random) and esp_log are auto-required common components.
)
