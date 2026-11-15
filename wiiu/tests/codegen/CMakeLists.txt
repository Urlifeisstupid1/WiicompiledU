# Codegen-area probe: checks whether Aroma's JIT ("codegen") area can hold and
# run translated code next to the RPX, as overflow space for the ~28 MB RPX
# code limit measured with tests/bigtext.
add_executable(wiiu_codegen_probe src/main.c)
target_compile_options(wiiu_codegen_probe PRIVATE -Wall -Wextra -O2)
wut_create_rpx(wiiu_codegen_probe)
wut_create_wuhb(wiiu_codegen_probe
    NAME "WiiCompiled codegen probe"
    SHORTNAME "WiiC codegen"
    AUTHOR "WiiCompiled port"
)
