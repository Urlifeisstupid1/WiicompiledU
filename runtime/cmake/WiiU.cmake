# SPDX-License-Identifier: GPL-3.0-or-later
#
# Wii U (Cafe OS, Aroma .wuhb) build of the runtime and products.
# Included from runtime/CMakeLists.txt when the devkitPro Wii U toolchain is used:
#
#   cmake -S runtime -B build-wiiu -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
#
# Differences from the PC build (PORTING_NOTES.md has the reasoning):
#   - GCC (devkitPPC) instead of Clang, 32-bit big-endian PowerPC.
#   - aurora/Dawn/SDL3 are replaced by wiiu/aurora-wiiu (D10); host-only runtime
#     files are excluded and src/platform/wiiu/ provides the platform layer.
#   - Translated code is linked into Aroma's codegen area through the cold-overlay
#     split (D8, wiiu/cmake/WiiUCold.cmake); everything is built with -mlongcall.
#   - Translated code is optimised for size (-Os, D7): the RPX may hold ~28 MB of
#     code and the codegen area 32 MiB.
#   - Retro Rewind is not built for the Wii U yet.

set(MKW_PLATFORM_WIIU TRUE)
set(CMAKE_CXX_STANDARD 20)
# devkitPro's CafeOS platform module does not define CMake's WHOLE_ARCHIVE link
# feature; GNU ld spells it the usual way.
set(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE
    "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_language(C ASM)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "The Wii U build uses devkitPPC GCC (use $DEVKITPRO/cmake/WiiU.cmake)")
endif()

set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "")
get_filename_component(MKW_RUNTIME_SOURCE_DIR "${MKW_RUNTIME_SOURCE_DIR}" ABSOLUTE)
get_filename_component(MKW_WIIU_DIR "${MKW_RUNTIME_SOURCE_DIR}/../wiiu" ABSOLUTE)
include("${MKW_WIIU_DIR}/cmake/WiiUCold.cmake")

# ---- aurora headers (Dolphin-SDK API; the implementation is aurora-wiiu) ---------------
set(MKW_AURORA_DIR "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main" CACHE PATH
    "aurora checkout; only its include/ directory is used by the Wii U build")
if(NOT EXISTS "${MKW_AURORA_DIR}/include/dolphin/gx.h")
    message(FATAL_ERROR "aurora headers not found in ${MKW_AURORA_DIR}. Run "
        "git submodule update --init aurora-main, or set MKW_AURORA_DIR.")
endif()

# ---- xxHash (same release aurora pins) ------------------------------------------------------
include(FetchContent)
FetchContent_Declare(mkw_wiiu_xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz
    URL_HASH SHA256=5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
    SOURCE_SUBDIR "no-cmake-project"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(mkw_wiiu_xxhash)
set(MKW_WIIU_XXHASH_DIR "${mkw_wiiu_xxhash_SOURCE_DIR}")

# ---- shared settings ------------------------------------------------------------------------
set(MKW_WIIU_DEFINITIONS
    TARGET_PC NOMINMAX SDL_MAIN_HANDLED
    # Crypto++ configuration: must match between the library and every includer.
    CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE CRYPTOPP_DISABLE_OS_RNG NO_OS_DEPENDENCE)
set(MKW_WIIU_COMMON_OPTIONS ${WIIU_COLD_COMPILE_OPTIONS} -ffunction-sections -fdata-sections)
set(MKW_WIIU_INCLUDE_DIRS
    "${MKW_RUNTIME_SOURCE_DIR}/include"
    "${MKW_RUNTIME_SOURCE_DIR}/src"
    "${MKW_RUNTIME_SOURCE_DIR}/.."              # "generated/<x>.h"
    "${MKW_WIIU_DIR}/aurora-wiiu/include"       # overrides, searched before aurora
    "${MKW_AURORA_DIR}/include"
    "${MKW_WIIU_DIR}/common"
    "${MKW_RUNTIME_SOURCE_DIR}/third_party"
    "${MKW_RUNTIME_SOURCE_DIR}/third_party/pugixml"
    "${MKW_RUNTIME_SOURCE_DIR}/third_party/toml11"
    "${MKW_RUNTIME_SOURCE_DIR}/third_party/libco"
    "${MKW_WIIU_XXHASH_DIR}")
# The translator's explicit float casts are PPC single-precision rounding points
# (same policy as the PC build).
set(MKW_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

function(mkw_wiiu_configure target)
    target_compile_definitions(${target} PRIVATE ${MKW_WIIU_DEFINITIONS})
    target_include_directories(${target} PRIVATE ${MKW_WIIU_INCLUDE_DIRS})
    target_compile_options(${target} PRIVATE ${MKW_WIIU_COMMON_OPTIONS})
endfunction()

# ---- third-party libraries ------------------------------------------------------------------
file(GLOB MKW_WIIU_CRYPTOPP_SOURCES CONFIGURE_DEPENDS "${MKW_RUNTIME_SOURCE_DIR}/third_party/cryptopp/*.cpp")
list(FILTER MKW_WIIU_CRYPTOPP_SOURCES EXCLUDE REGEX
    "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
list(FILTER MKW_WIIU_CRYPTOPP_SOURCES EXCLUDE REGEX "(_avx|_ppc|_simd|_sse)\\.cpp$")
add_library(mkw_wiiu_cryptopp STATIC ${MKW_WIIU_CRYPTOPP_SOURCES})
mkw_wiiu_configure(mkw_wiiu_cryptopp)
target_compile_options(mkw_wiiu_cryptopp PRIVATE -Os -w)

add_library(mkw_wiiu_thirdparty STATIC
    "${MKW_RUNTIME_SOURCE_DIR}/third_party/pugixml/pugixml.cpp"
    "${MKW_RUNTIME_SOURCE_DIR}/third_party/libco/libco.c"
    "${MKW_WIIU_XXHASH_DIR}/xxhash.c")
mkw_wiiu_configure(mkw_wiiu_thirdparty)
target_compile_options(mkw_wiiu_thirdparty PRIVATE -O2 -w)

# ---- aurora-wiiu (D10) ----------------------------------------------------------------------
add_library(mkw_wiiu_aurora STATIC
    "${MKW_WIIU_DIR}/aurora-wiiu/src/aurora_wiiu.cpp"
    "${MKW_WIIU_DIR}/aurora-wiiu/src/aurora_wiiu_stubs.cpp")
mkw_wiiu_configure(mkw_wiiu_aurora)
target_compile_options(mkw_wiiu_aurora PRIVATE -O2)

# ---- runtime --------------------------------------------------------------------------------
file(GLOB_RECURSE MKW_WIIU_RUNTIME_SOURCES CONFIGURE_DEPENDS "${MKW_RUNTIME_SOURCE_DIR}/src/*.cpp")
# Host-only or replaced on the Wii U (see PORTING_NOTES.md, D10):
list(FILTER MKW_WIIU_RUNTIME_SOURCES EXCLUDE REGEX
    "/src/(platform/macos/|product/|guest_flat_memory(_macos)?\\.cpp$|host_cpu_baseline\\.cpp$|discord_presence\\.cpp$|steam_input_layout\\.cpp$|settings_overlay\\.cpp$|controller_mapping_wizard\\.cpp$|friend_code_keyboard\\.cpp$|external_audio_macos\\.cpp$|audio_backend\\.cpp$|input_bindings\\.cpp$|motion_trick\\.cpp$|wiimote/|hle/net/)")
add_library(mkw_wiiu_runtime STATIC
    ${MKW_WIIU_RUNTIME_SOURCES}
    "${MKW_RUNTIME_SOURCE_DIR}/src/platform/wiiu/atomic64.c"
    "${MKW_WIIU_DIR}/common/wiicold_loader.c")
mkw_wiiu_configure(mkw_wiiu_runtime)
target_compile_options(mkw_wiiu_runtime PRIVATE -O2 -ffast-math -w)
set_source_files_properties(
    "${MKW_RUNTIME_SOURCE_DIR}/src/ppc_helpers.cpp"
    "${MKW_RUNTIME_SOURCE_DIR}/src/fpu_helpers.cpp"
    "${MKW_RUNTIME_SOURCE_DIR}/src/ppc_quantized.cpp"
    PROPERTIES COMPILE_OPTIONS "${MKW_TRANSLATED_PPC_FP_OPTIONS}")
# ax_effects.cpp relies on `#pragma clang fp contract(off)`, which GCC ignores.
set_source_files_properties("${MKW_RUNTIME_SOURCE_DIR}/src/hle/audio/ax_effects.cpp"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# ---- products -------------------------------------------------------------------------------
#
#   mkw_wiiu_add_product(<target>
#       PRODUCT_SOURCE <runtime/src/product/...cpp>
#       GENERATED_DIR <dir with RuntimeConfig.h, data_sections_init.cpp, ...>
#       [GUEST_REGION <n>]
#       [COLD_LIBRARIES <wiicold_* static libraries>]   translated code for the codegen area
#       [HOT_SOURCES <sources>]                          translated code kept in the RPX
#       NAME <title> SHORTNAME <short title>)
#
# Produces <target>.wuhb with the cold image and the bundled assets (dsp_coef.bin,
# wii_bootstrap/) in its content folder (fs:/vol/content on the console).
function(mkw_wiiu_add_product target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PRODUCT_SOURCE;GENERATED_DIR;GUEST_REGION;NAME;SHORTNAME"
        "COLD_LIBRARIES;HOT_SOURCES")
    if(NOT ARG_GUEST_REGION)
        set(ARG_GUEST_REGION 0)
    endif()
    set(generated "${ARG_GENERATED_DIR}")
    set(game_sources
        "${generated}/data_sections_init.cpp"
        "${MKW_RUNTIME_SOURCE_DIR}/game/guest_region.cpp"
        "${MKW_RUNTIME_SOURCE_DIR}/game/runtime_config_values.cpp"
        "${MKW_RUNTIME_SOURCE_DIR}/game/setup_version.cpp")
    if(EXISTS "${generated}/guest_symbol_table.cpp")
        list(APPEND game_sources "${generated}/guest_symbol_table.cpp")
    else()
        list(APPEND game_sources "${MKW_RUNTIME_SOURCE_DIR}/cmake/guest_symbol_table_stub.cpp")
    endif()
    if(EXISTS "${generated}/data_sections_init_blobs.S")
        list(APPEND game_sources "${generated}/data_sections_init_blobs.S")
    endif()

    add_executable(${target} "${ARG_PRODUCT_SOURCE}" ${game_sources} ${ARG_HOT_SOURCES})
    mkw_wiiu_configure(${target})
    target_include_directories(${target} PRIVATE "${generated}/..")
    target_compile_definitions(${target} PRIVATE MKW_TRANSLATED_GUEST_REGION=${ARG_GUEST_REGION})
    target_compile_options(${target} PRIVATE -Os ${MKW_TRANSLATED_PPC_FP_OPTIONS} -w)
    # Whole-archive: the runtime registers HLE overrides from static constructors.
    target_link_libraries(${target} PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,mkw_wiiu_runtime>"
        ${ARG_COLD_LIBRARIES}
        mkw_wiiu_aurora mkw_wiiu_thirdparty mkw_wiiu_cryptopp m)

    set(content_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_content")
    file(MAKE_DIRECTORY "${content_dir}")
    file(COPY "${MKW_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin" DESTINATION "${content_dir}")
    file(COPY "${MKW_RUNTIME_SOURCE_DIR}/assets/wii/" DESTINATION "${content_dir}/wii_bootstrap")
    # The runtime loads fs:/vol/content/cold.bin at startup when the product has one.
    if(ARG_COLD_LIBRARIES)
        wiiu_add_cold_overlay(${target} IMAGE "${content_dir}/cold.bin" NO_LOADER)
    endif()
    # The translator's data-blob assembly declares .note.GNU-stack, which elf2rpl rejects.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${WIIU_OBJCOPY_EXE}" --remove-section=.note.GNU-stack "$<TARGET_FILE:${target}>"
        VERBATIM)
    wut_create_rpx(${target})
    wut_create_wuhb(${target}
        CONTENT "${content_dir}"
        NAME "${ARG_NAME}"
        SHORTNAME "${ARG_SHORTNAME}"
        AUTHOR "WiiCompiled")
endfunction()

# ---- the game -------------------------------------------------------------------------------
option(MKW_BUILD_PRODUCTS "Build the translated game (needs generated/ from your own disc)" ON)
set(MKW_WIIU_SHARD_MANIFEST "${MKW_RUNTIME_SOURCE_DIR}/../generated/build_shards/shards.cmake")
if(MKW_BUILD_PRODUCTS AND EXISTS "${MKW_WIIU_SHARD_MANIFEST}")
    include("${MKW_WIIU_SHARD_MANIFEST}")
    if(NOT MKW_BASE_COMMON_SHARDS)
        message(FATAL_ERROR "Translator build graph contains no shared base shards")
    endif()
    # All translated code runs on the codegen core (D9), so its placement only
    # matters for size: the shared shards go to the codegen area, the small
    # registration/sensitive sets stay in the RPX.
    add_library(wiicold_mkw_base_shared STATIC ${MKW_BASE_COMMON_SHARDS})
    mkw_wiiu_configure(wiicold_mkw_base_shared)
    wiiu_cold_library(wiicold_mkw_base_shared)
    target_compile_definitions(wiicold_mkw_base_shared PRIVATE
        MKW_TRANSLATED_GUEST_REGION=${MKW_TRANSLATED_GUEST_REGION})
    target_compile_options(wiicold_mkw_base_shared PRIVATE -Os ${MKW_TRANSLATED_PPC_FP_OPTIONS} -w)
    mkw_wiiu_add_product(WiiCompiled
        PRODUCT_SOURCE "${MKW_RUNTIME_SOURCE_DIR}/src/product/base_product.cpp"
        GENERATED_DIR "${MKW_RUNTIME_SOURCE_DIR}/../generated"
        GUEST_REGION ${MKW_TRANSLATED_GUEST_REGION}
        COLD_LIBRARIES wiicold_mkw_base_shared
        HOT_SOURCES ${MKW_BASE_REGISTRATION_SOURCES} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
        NAME "WiiCompiled" SHORTNAME "WiiCompiled")
elseif(MKW_BUILD_PRODUCTS)
    message(STATUS "WiiCompiled (Wii U): no generated/build_shards/shards.cmake yet - "
        "only the runtime libraries and the smoke test are built. See PORTING_NOTES.md.")
endif()

# ---- runtime smoke test (no game code; safe to share) ------------------------------------------
# Boots upstream's RuntimeMain with empty generated stand-ins: exercises startup,
# logging, config, guest RAM, the system bridge, aurora-wiiu and the GX worker,
# then stops with a clean error at the missing game entry point.
mkw_wiiu_add_product(wiiu_runtime_smoke
    PRODUCT_SOURCE "${MKW_RUNTIME_SOURCE_DIR}/src/product/base_product.cpp"
    GENERATED_DIR "${MKW_WIIU_DIR}/tests/runtime-smoke/generated"
    NAME "WiiCompiled runtime smoke test" SHORTNAME "WiiC smoke")
