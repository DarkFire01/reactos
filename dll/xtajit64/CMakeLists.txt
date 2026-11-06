
# The x64 emulator only runs on ARM64, next to ARM64X system DLLs
if(NOT ARCH STREQUAL "arm64")
    return()
endif()

spec2def(xtajit64.dll xtajit64.spec)

list(APPEND SOURCE
    cpu.c
    xtajit64.c)

add_asm_files(xtajit64_asm dispatch.s)

add_library(xtajit64 MODULE
    ${SOURCE}
    ${xtajit64_asm}
    ${CMAKE_CURRENT_BINARY_DIR}/xtajit64.def)

set_module_type(xtajit64 win32dll ENTRYPOINT 0)
add_importlibs(xtajit64 ntdll)
add_dependencies(xtajit64 psdk asm)
add_cd_file(TARGET xtajit64 DESTINATION reactos/system32 FOR all)
