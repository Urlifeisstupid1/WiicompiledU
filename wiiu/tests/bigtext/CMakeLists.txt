# Loader-limit probe: one RPX per configuration with a text blob between
# main() and libwut, so main's library calls cross the blob through linker
# long-branch stubs. Tells us how much code the Wii U (and Cemu) loader
# accepts from an Aroma .wuhb.
#
# Results on hardware (2026-11-15):
#   zero-filled 16 MB          : OK
#   zero-filled 32 / 56 MB     : stuck on the loading screen
#   zero-filled 64 / 128 / 192 : error 160-0220 (Aroma max_codesize = 64 MiB)
#
# Current round:
#   zero-filled 20 / 24 / 28 MB: find the threshold
#   random-filled 32 MB        : incompressible filler. If this launches while the
#                                zero-filled 32 MB build hangs, the hang comes from
#                                the extreme compression ratio, not from the size.

function(add_bigtext_probe size_mb fill)
    set(target wiiu_bigtext_${size_mb}mb_${fill})
    # Each target compiles its own copy of main.c, so per-file properties such as
    # OBJECT_DEPENDS below never leak into the other probe targets.
    set(source "${CMAKE_CURRENT_BINARY_DIR}/main_${size_mb}mb_${fill}.c")
    configure_file(src/main.c "${source}" COPYONLY)
    add_executable(${target} "${source}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE BIGTEXT_MB=${size_mb})
    target_compile_options(${target} PRIVATE -Wall -O2)
    if(fill STREQUAL "random")
        set(blob "${CMAKE_CURRENT_BINARY_DIR}/random_${size_mb}mb.bin")
        # Written to a temporary name and renamed, so the assembler can never
        # .incbin a partially written file.
        add_custom_command(
            OUTPUT "${blob}"
            COMMAND sh -c "head -c ${size_mb}M /dev/urandom > '${blob}.tmp' && mv '${blob}.tmp' '${blob}'"
            COMMENT "Generating ${size_mb} MB of random filler"
            VERBATIM)
        add_custom_target(${target}_blob DEPENDS "${blob}")
        add_dependencies(${target} ${target}_blob)
        target_compile_definitions(${target} PRIVATE BIGTEXT_BLOB_PATH="${blob}")
        set_source_files_properties("${source}" PROPERTIES OBJECT_DEPENDS "${blob}")
    endif()
    wut_create_rpx(${target})
    wut_create_wuhb(${target}
        NAME "WiiC bigtext ${size_mb}MB ${fill}"
        SHORTNAME "WiiC big${size_mb}${fill}"
        AUTHOR "WiiCompiled port"
    )
endfunction()

foreach(size_mb 20 24 28)
    add_bigtext_probe(${size_mb} zero)
endforeach()
add_bigtext_probe(32 random)
