# Cold-overlay test: proves the RPX + codegen-area split (wiiu/cmake/WiiUCold.cmake)
# works on hardware before the runtime depends on it.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/WiiUCold.cmake")

add_library(wiicold_overlay_test STATIC src/cold.cpp)
target_compile_options(wiicold_overlay_test PRIVATE -O2 -Wall)
wiiu_cold_library(wiicold_overlay_test)

add_executable(wiiu_overlay_test src/hot.cpp)
target_compile_options(wiiu_overlay_test PRIVATE -O2 -Wall ${WIIU_COLD_COMPILE_OPTIONS})
target_link_libraries(wiiu_overlay_test PRIVATE wiicold_overlay_test)

set(content_dir "${CMAKE_CURRENT_BINARY_DIR}/content")
file(MAKE_DIRECTORY "${content_dir}")
wiiu_add_cold_overlay(wiiu_overlay_test IMAGE "${content_dir}/cold.bin")
wut_create_rpx(wiiu_overlay_test)
wut_create_wuhb(wiiu_overlay_test
    CONTENT "${content_dir}"
    NAME "WiiCompiled overlay test"
    SHORTNAME "WiiC overlay"
    AUTHOR "WiiCompiled port"
)
