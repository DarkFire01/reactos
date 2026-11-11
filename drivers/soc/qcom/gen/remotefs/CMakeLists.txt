remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    main.c
    service.c
    remotefs.h)

add_library(qcomremotefs MODULE ${SOURCE} qcomremotefs.rc)
set_module_type(qcomremotefs kmdfdriver)
target_link_libraries(qcomremotefs qcomqmi ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomremotefs wdfldr ntoskrnl hal)
add_cd_file(TARGET qcomremotefs DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomremotefs qcomremotefs.inf)
