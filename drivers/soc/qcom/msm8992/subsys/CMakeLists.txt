remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    main.c
    fsm.c
    hw.c
    monitor.c
    ssr.c
    child.c
    subsys.h)

add_library(qcomsubsys MODULE ${SOURCE} qcomsubsys.rc)
set_module_type(qcomsubsys kmdfdriver)
target_link_libraries(qcomsubsys ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomsubsys wdfldr qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomsubsys DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomsubsys qcomsubsys.inf)
