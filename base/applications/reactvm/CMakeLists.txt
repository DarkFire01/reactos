
if(ARCH STREQUAL "i386")
    # A processor that is half as wide calls out for the wide arithmetic here,
    # and the compiler leaves those calls to the runtime to answer.
    list(APPEND REACTVM_ASM_SOURCE
        ${REACTOS_SOURCE_DIR}/sdk/lib/crt/math/i386/allmul_asm.s
        ${REACTOS_SOURCE_DIR}/sdk/lib/crt/math/i386/allshl_asm.s)
endif()

add_asm_files(reactvm_asm ${REACTVM_ASM_SOURCE})

add_executable(reactvm reactvm.c ${reactvm_asm} reactvm.rc)
set_module_type(reactvm win32cui)
add_importlibs(reactvm advapi32 msvcrt kernel32)
add_cd_file(TARGET reactvm DESTINATION reactos/system32 FOR all)
