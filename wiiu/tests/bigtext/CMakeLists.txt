# Loader-limit probe: one RPX per size with a text blob of that size between
# main() and libwut, so main's library calls cross the blob through linker
# long-branch stubs. Tells us how much translated code the Wii U (and Cemu)
# loader accepts from an Aroma .wuhb.
# Result on hardware (2026-11-15): 64/128/192 MB all fail with error 160-0220.
# Aroma's MochaPayload sets max_codesize = 64 MiB for homebrew, so these sizes
# (16/32/56 MB) are expected to launch.
foreach(size_mb 16 32 56)
    set(target wiiu_bigtext_${size_mb}mb)
    add_executable(${target} src/main.c)
    target_compile_definitions(${target} PRIVATE BIGTEXT_MB=${size_mb})
    target_compile_options(${target} PRIVATE -Wall -O2)
    wut_create_rpx(${target})
    wut_create_wuhb(${target}
        NAME "WiiC bigtext ${size_mb}MB"
        SHORTNAME "WiiC big${size_mb}"
        AUTHOR "WiiCompiled port"
    )
endforeach()
