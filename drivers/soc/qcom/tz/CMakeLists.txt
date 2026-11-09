remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    main.c
    smc.c
    tz.h)

list(APPEND ASM_SOURCE
    smc.s)

add_asm_files(qcomtz_asm ${ASM_SOURCE})

add_library(qcomtz MODULE ${SOURCE} ${qcomtz_asm} qcomtz.rc)
set_module_type(qcomtz kmdfdriver)
target_link_libraries(qcomtz ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomtz wdfldr ntoskrnl hal)
add_dependencies(qcomtz asm)
add_cd_file(TARGET qcomtz DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomtz qcomtz.inf)
