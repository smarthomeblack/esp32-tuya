# Host-build of the hub's pure-logic self-test suites (plain gcc/g++/cmake,
# no ESP-IDF). The component sources are compiled verbatim; ESP-IDF headers
# are shadowed by shims/ via the include path order.
cmake_minimum_required(VERSION 3.16)
project(host_selftests C CXX)

include(CheckCXXCompilerFlag)

# The firmware builds with -std=gnu++26; older host GCCs only accept the
# draft spelling (gnu++2c). Fall back to the newest dialect available.
check_cxx_compiler_flag("-std=gnu++26" HOST_HAVE_GNUXX26)
check_cxx_compiler_flag("-std=gnu++2c" HOST_HAVE_GNUXX2C)
if(HOST_HAVE_GNUXX26)
    add_compile_options(-std=gnu++26)
elseif(HOST_HAVE_GNUXX2C)
    add_compile_options(-std=gnu++2c)
else()
    add_compile_options(-std=gnu++20)
endif()

# Mirror the firmware build policy: -fno-exceptions -Wall -Wextra -Werror
# with the same -Wno-error relaxations the ESP-IDF build carries.
add_compile_options(
    -fno-exceptions
    -Wall
    -Wextra
    -Werror
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
    -Wno-error=deprecated-declarations
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-format-truncation
)

# The ESP32 target guarantees freestanding <stdint.h> in every TU (the IDF
# build force-includes the generated config, which drags it in); component
# headers rely on that without including <cstdint> themselves.
add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/shims/host_prelude.h)

set(SHIM_SRCS
    shims/src/cloud_stubs.cc
    shims/src/esp_shims.cc
    shims/src/nvs_shim.cc
    shims/src/psa_shim.cc
)

set(COMPONENT_SRCS
    # sys — real Settings code over the in-memory NVS shim
    ../components/sys/src/settings.cc

    # tuya_mqtt — MQTT message suite (114)
    ../components/tuya_mqtt/src/mqtt_message.cc
    ../components/tuya_mqtt/src/mqtt_message_test.cc

    # hub_core — model (172), store (64)
    ../components/hub_core/src/device_registry.cc
    ../components/hub_core/src/device_store_format.cc
    ../components/hub_core/src/device_store_test.cc
    ../components/hub_core/src/main_thread.cc
    ../components/hub_core/src/model_self_test.cc
    ../components/hub_core/src/parse.cc
    ../components/hub_core/src/parse_helpers.cc
    ../components/hub_core/src/presence_status.cc
    ../components/hub_core/src/strategies.cc

    # hub_services — command (219), enrich (87), discovery (90)
    ../components/hub_services/src/command_service.cc
    ../components/hub_services/src/command_service_test.cc
    ../components/hub_services/src/dedup_filter.cc
    ../components/hub_services/src/discovery.cc
    ../components/hub_services/src/discovery_test.cc
    ../components/hub_services/src/openapi_command.cc
    ../components/hub_services/src/openapi_enrichment.cc
    ../components/hub_services/src/openapi_enrichment_test.cc

    # tuya_cloud — pipeline (26), openapi (28)
    ../components/tuya_cloud/src/customer_api.cc
    ../components/tuya_cloud/src/openapi_client.cc
    ../components/tuya_cloud/src/openapi_self_test.cc
    ../components/tuya_cloud/src/pipeline_self_test.cc
    ../components/tuya_cloud/src/token_manager.cc

    # tuya_crypto — primitives used by the suites above
    ../components/tuya_crypto/src/aes_gcm.cc
    ../components/tuya_crypto/src/base64.cc
    ../components/tuya_crypto/src/hmac_sha256.cc
    ../components/tuya_crypto/src/json_guard.cc
    ../components/tuya_crypto/src/json_writer.cc
    ../components/tuya_crypto/src/md5.cc
    ../components/tuya_crypto/src/sha256.cc
    ../components/tuya_crypto/src/uuid.cc

    # Real cJSON (managed component, vendored upstream source)
    ../managed_components/espressif__cjson/cJSON/cJSON.c
)

add_executable(selftest_runner
    runner/main.cc
    ${SHIM_SRCS}
    ${COMPONENT_SRCS}
)

target_include_directories(selftest_runner PRIVATE
    # FIRST on purpose: shadows <esp_log.h>, <esp_random.h>, <esp_timer.h>,
    # <esp_err.h>, <nvs.h>, <freertos/*> and <psa/crypto.h> on the host.
    shims
    ../components/sys/include
    ../components/tuya_cloud/include
    ../components/tuya_cloud/src
    ../components/tuya_crypto/include
    ../components/tuya_crypto/src
    ../components/hub_core/include
    ../components/hub_core/src
    ../components/hub_services/include
    ../components/hub_services/src
    ../components/tuya_mqtt/include
    ../components/tuya_mqtt/src
    ../managed_components/espressif__cjson/cJSON
)

# ESP32 is ILP32 (size_t == unsigned): component printf formats using %u
# for size_t are correct on target and false alarms on the LP64 host. The
# device CI build still enforces -Werror=format; the host build opts out.
set_source_files_properties(${COMPONENT_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-format")

# Vendored third-party: keep its own warnings out of the -Werror build.
set_source_files_properties(
    ../managed_components/espressif__cjson/cJSON/cJSON.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
