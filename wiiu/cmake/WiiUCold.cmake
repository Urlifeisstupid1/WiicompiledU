# WiiUCold.cmake - split "cold" code into Aroma's codegen (JIT) area.
#
#   wiiu_add_cold_overlay(<rpx-target> IMAGE <path/to/content/cold.bin> [NO_LOADER])
#
# NO_LOADER: the target already links wiicold_loader.c through a library (the
# runtime does), so it is not added again.
#
# Call it BEFORE wut_create_rpx(<rpx-target>): the split must rewrite the ELF
# before elf2rpl converts it (POST_BUILD commands run in the order they are added).
# Code from static libraries named libwiicold_*.a (CMake targets "wiicold_*")
# linked into <rpx-target> is placed in the codegen area. Declare each with
# wiiu_cold_library(); code that calls into them must be compiled with -mlongcall
# (WIIU_COLD_COMPILE_OPTIONS). The image must be shipped in the .wuhb content
# folder and loaded with WiiColdLoad() before cold code runs.
#
# How: a copy of wut.ld is generated with a "coldmem" region at the codegen
# address and the .wiicold section (wiicold.ld) placed ahead of .text, and a
# small specs file makes the link use that script instead of wut.ld.

set(WIIU_COLD_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(WIIU_COLD_COMPILE_OPTIONS -mlongcall)
set(WIIU_COLD_BASE "0x01800000")
set(WIIU_COLD_SIZE "32M")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(WIIU_OBJCOPY_EXE NAMES powerpc-eabi-objcopy HINTS "${DEVKITPPC}/bin" "$ENV{DEVKITPPC}/bin")
if(NOT WIIU_OBJCOPY_EXE)
    message(FATAL_ERROR "powerpc-eabi-objcopy not found")
endif()

# ---- private register save/restore helpers for cold code -------------------------------
# At -Os, GCC saves/restores registers through libgcc's out-of-line helpers
# (_savegpr_*, _restgpr_*_x, _savefpr_*, ...), reached with a plain relative branch
# that -mlongcall does not cover. Cold code is too far from the RPX copies, so it
# gets its own: libgcc's crtsav*/crtres* objects with every symbol renamed to
# __wiicold_<name>, built into libwiicold_savres.a (placed in the codegen area), and
# every cold library's references renamed the same way after each build.
# (libgcc is covered by the GCC Runtime Library Exception.)
set(WIIU_COLD_SAVRES_MEMBERS
    crtsavfpr.o crtresfpr.o crtsavgpr.o crtresgpr.o crtresxfpr.o crtresxgpr.o crtsavevr.o crtrestvr.o)
find_program(WIIU_AR_EXE NAMES powerpc-eabi-ar HINTS "${DEVKITPPC}/bin" "$ENV{DEVKITPPC}/bin")
find_program(WIIU_NM_EXE NAMES powerpc-eabi-nm HINTS "${DEVKITPPC}/bin" "$ENV{DEVKITPPC}/bin")

function(_wiiu_cold_savres_setup)
    if(TARGET wiicold_savres)
        return()
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-libgcc-file-name
        OUTPUT_VARIABLE libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(work "${CMAKE_BINARY_DIR}/wiicold_savres")
    file(MAKE_DIRECTORY "${work}")
    execute_process(COMMAND "${WIIU_AR_EXE}" x "${libgcc}" ${WIIU_COLD_SAVRES_MEMBERS}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "WiiUCold: could not extract the save/restore helpers from ${libgcc}")
    endif()
    set(map "")
    set(objects "")
    foreach(member IN LISTS WIIU_COLD_SAVRES_MEMBERS)
        execute_process(COMMAND "${WIIU_NM_EXE}" --defined-only "${work}/${member}"
            OUTPUT_VARIABLE symbols)
        string(REGEX MATCHALL "[TtWw] _[a-z]+_[0-9a-z_]+" entries "${symbols}")
        foreach(entry IN LISTS entries)
            string(REGEX REPLACE "^[TtWw] " "" name "${entry}")
            string(APPEND map "${name} __wiicold_${name}\n")
        endforeach()
        list(APPEND objects "${work}/${member}")
    endforeach()
    set(WIIU_COLD_SAVRES_MAP "${work}/rename.map" CACHE INTERNAL "")
    file(WRITE "${WIIU_COLD_SAVRES_MAP}" "${map}")
    foreach(object IN LISTS objects)
        execute_process(COMMAND "${WIIU_OBJCOPY_EXE}" "--redefine-syms=${WIIU_COLD_SAVRES_MAP}" "${object}")
    endforeach()
    add_library(wiicold_savres STATIC IMPORTED GLOBAL)
    set(archive "${work}/libwiicold_savres.a")
    file(REMOVE "${archive}")
    execute_process(COMMAND "${WIIU_AR_EXE}" rcs "${archive}" ${objects})
    set_target_properties(wiicold_savres PROPERTIES IMPORTED_LOCATION "${archive}")
endfunction()

#   wiiu_cold_library(<static library target>)
# Marks a wiicold_* library as cold code: -mlongcall, and its save/restore helper
# references renamed to the private cold copies after every build.
function(wiiu_cold_library target)
    _wiiu_cold_savres_setup()
    target_compile_options(${target} PRIVATE ${WIIU_COLD_COMPILE_OPTIONS})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${WIIU_OBJCOPY_EXE}" "--redefine-syms=${WIIU_COLD_SAVRES_MAP}" "$<TARGET_FILE:${target}>"
        COMMENT "Pointing ${target} at the cold copies of the register save/restore helpers"
        VERBATIM)
    target_link_libraries(${target} INTERFACE wiicold_savres)
endfunction()

function(_wiiu_generate_cold_link_files out_specs)
    set(wut_ld "${WUT_ROOT}/share/wut.ld")
    file(READ "${wut_ld}" script)
    file(READ "${WIIU_COLD_DIR}/wiicold.ld" fragment)
    set(memory_anchor "MEMORY {\n")
    set(sections_anchor "   . = ORIGIN(codemem);\n")
    string(FIND "${script}" "${memory_anchor}" memory_pos)
    string(FIND "${script}" "${sections_anchor}" sections_pos)
    if(memory_pos EQUAL -1 OR sections_pos EQUAL -1)
        message(FATAL_ERROR "WiiUCold: ${wut_ld} does not have the expected layout; update WiiUCold.cmake")
    endif()
    # The RPX's code is linked at 0x04000000 instead of wut's 0x02000000 so that it
    # cannot overlap the codegen area (0x01800000 + 32 MiB). Its link address is
    # arbitrary: Cafe OS relocates the RPX when loading it (it runs at 0x0C000000).
    set(codemem_default "codemem (rwx) : ORIGIN = 0x02000000, LENGTH = 224M")
    string(FIND "${script}" "${codemem_default}" codemem_pos)
    if(codemem_pos EQUAL -1)
        message(FATAL_ERROR "WiiUCold: ${wut_ld} has an unexpected codemem region; update WiiUCold.cmake")
    endif()
    string(REPLACE "${codemem_default}" "codemem (rwx) : ORIGIN = 0x04000000, LENGTH = 192M"
        script "${script}")
    string(REPLACE "${memory_anchor}"
        "${memory_anchor}   coldmem (rwx) : ORIGIN = ${WIIU_COLD_BASE}, LENGTH = ${WIIU_COLD_SIZE}\n   coldehmem (rw) : ORIGIN = 0x80000000, LENGTH = 512M\n"
        script "${script}")
    string(REPLACE "${sections_anchor}" "${fragment}\n${sections_anchor}" script "${script}")
    set(generated_ld "${CMAKE_BINARY_DIR}/wiicold_wut.ld")
    file(WRITE "${generated_ld}" "/* Generated by WiiUCold.cmake from ${wut_ld} + wiicold.ld. */\n${script}")
    set(specs "${CMAKE_BINARY_DIR}/wiicold.specs")
    file(WRITE "${specs}"
        "*link:\n%(old_link) -T ${generated_ld} --gc-sections --emit-relocs --no-eh-frame-hdr -z nocopyreloc %(wut_entry)\n")
    set(${out_specs} "${specs}" PARENT_SCOPE)
endfunction()

function(wiiu_add_cold_overlay target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_LOADER" "IMAGE" "")
    if(NOT ARG_IMAGE)
        message(FATAL_ERROR "wiiu_add_cold_overlay: IMAGE is required")
    endif()
    get_filename_component(image "${ARG_IMAGE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    _wiiu_generate_cold_link_files(specs)
    if(NOT ARG_NO_LOADER)
        target_sources(${target} PRIVATE "${WIIU_COLD_DIR}/../common/wiicold_loader.c")
    endif()
    target_include_directories(${target} PRIVATE "${WIIU_COLD_DIR}/../common")
    # Comes after wut.specs on the link line, so this *link spec replaces wut's.
    target_link_options(${target} PRIVATE "-specs=${specs}")
    # The split runs as a POST_BUILD step, so a change to the tool must relink.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${WIIU_COLD_DIR}/wiicold.ld" "${WIIU_COLD_DIR}/../tools/wiiu_cold_split.py")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WIIU_COLD_DIR}/../tools/wiiu_cold_split.py"
                --objcopy "${WIIU_OBJCOPY_EXE}"
                --keep-full "$<TARGET_FILE:${target}>.full"
                "$<TARGET_FILE:${target}>" "${image}"
        COMMENT "Splitting cold code of ${target} into ${image}"
        VERBATIM)
endfunction()
