add_executable(wiiu_hello src/main.c)
target_compile_options(wiiu_hello PRIVATE -Wall -Wextra -O2)
wut_create_rpx(wiiu_hello)
wut_create_wuhb(wiiu_hello
    NAME "WiiCompiled Hello"
    SHORTNAME "WiiC Hello"
    AUTHOR "WiiCompiled port"
)
