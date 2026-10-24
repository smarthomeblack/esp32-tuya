# The SPA lives in the "assets" flash partition as a packed WAS1 image
# (dist/web_assets.bin); only the ~2 KB recovery page stays embedded.
idf_component_register(
    SRCS
        "src/asset_store.cc"
        "src/asset_self_test.cc"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "dist/fallback.html.gz"
    REQUIRES
        "esp_partition"  # esp_partition_* + mmap (v6: its own component)
        "esp_rom"        # esp_rom_crc32_le
        "spi_flash"      # spi_flash_mmap_get_free_pages / MMU page size
)

# ---- dist/web_assets.bin: repacked on every build ----
# The component lib must depend on the bin before the embed-asm step.
idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(web_python PYTHON)

file(GLOB_RECURSE web_asset_src_files LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/dist/web_assets.bin"
           "${CMAKE_CURRENT_SOURCE_DIR}/dist/fallback.html.gz"
    COMMAND ${web_python} "${project_dir}/scripts/build_web.py"
    DEPENDS ${web_asset_src_files}
            "${project_dir}/scripts/build_web.py"
            "${project_dir}/scripts/web_asset_pack.py"
    WORKING_DIRECTORY "${project_dir}"
    COMMENT "Packing web assets -> dist/web_assets.bin (WAS1)"
    VERBATIM
)
add_custom_target(web_assets_bin
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dist/web_assets.bin"
)
add_dependencies(${COMPONENT_LIB} web_assets_bin)

# `idf.py flash` uploads the packed image together with the app.
esptool_py_flash_to_partition(flash "assets"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/web_assets.bin")
add_dependencies(flash web_assets_bin)
