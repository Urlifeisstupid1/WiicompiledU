# Checks the C++ runtime features WiiCompiled's runtime relies on:
# std::thread, mutex/condvar, exceptions, std::filesystem on the SD card, chrono.
# thread_local and 64-bit atomics are deliberately absent: they do not link
# (see PORTING_NOTES.md, items T1 and T2).
add_executable(wiiu_stdtest src/main.cpp)
target_compile_options(wiiu_stdtest PRIVATE -Wall -O2)
wut_create_rpx(wiiu_stdtest)
wut_create_wuhb(wiiu_stdtest
    NAME "WiiCompiled stdlib test"
    SHORTNAME "WiiC stdtest"
    AUTHOR "WiiCompiled port"
)
