# Runtime-primitives test: MKW_THREAD_LOCAL, the 64-bit atomic shim and
# HostContext fibers (libco ppc.c), using the runtime's own sources, with the
# fibers on the codegen core calling cold-overlay code.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/WiiUCold.cmake")
set(runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime")

add_library(wiicold_primitives_test STATIC src/cold.cpp)
target_compile_options(wiicold_primitives_test PRIVATE -O2 -Wall ${WIIU_COLD_COMPILE_OPTIONS})

add_executable(wiiu_primitives_test
    src/main.cpp
    "${runtime_dir}/src/host_context.cpp"
    "${runtime_dir}/src/platform/wiiu/mkw_thread_local.cpp"
    "${runtime_dir}/src/platform/wiiu/atomic64.c"
    "${runtime_dir}/third_party/libco/libco.c")
target_include_directories(wiiu_primitives_test PRIVATE
    "${runtime_dir}/include" "${runtime_dir}/third_party/libco")
target_compile_options(wiiu_primitives_test PRIVATE -O2 ${WIIU_COLD_COMPILE_OPTIONS})
set_source_files_properties("${runtime_dir}/third_party/libco/libco.c" PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(wiiu_primitives_test PRIVATE wiicold_primitives_test)

set(content_dir "${CMAKE_CURRENT_BINARY_DIR}/content")
file(MAKE_DIRECTORY "${content_dir}")
wiiu_add_cold_overlay(wiiu_primitives_test IMAGE "${content_dir}/cold.bin")
wut_create_rpx(wiiu_primitives_test)
wut_create_wuhb(wiiu_primitives_test
    CONTENT "${content_dir}"
    NAME "WiiCompiled primitives test"
    SHORTNAME "WiiC prims"
    AUTHOR "WiiCompiled port"
)
