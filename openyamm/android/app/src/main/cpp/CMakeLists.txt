get_filename_component(OPENYAMM_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

if (OPENYAMM_VITA_LOADER)
    add_library(main SHARED "${OPENYAMM_REPO_ROOT}/packaging/vita/android-loader-entry.cpp")
    # Community ELF loaders consume ordinary ARM relocations and section tables.
    target_link_options(main PRIVATE
        "-Wl,--hash-style=both"
        "-Wl,--pack-dyn-relocs=none"
        "-Wl,--exclude-libs,ALL"
        "-Wl,-z,max-page-size=4096")
else()
    add_library(main SHARED openyamm_android_entry.cpp)
endif()

target_include_directories(main
    PRIVATE
        "${OPENYAMM_REPO_ROOT}"
)

target_compile_features(main PRIVATE cxx_std_20)
target_link_libraries(main PRIVATE openyamm_game SDL3::SDL3 log)
