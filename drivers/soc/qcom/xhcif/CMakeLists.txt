remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    main.c
    phy.c
    xhcif.h)

add_library(qcomxhcif MODULE ${SOURCE} qcomxhcif.rc)
set_module_type(qcomxhcif kmdfdriver)
target_link_libraries(qcomxhcif ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomxhcif wdfldr ntoskrnl hal)
add_cd_file(TARGET qcomxhcif DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomxhcif qcomxhcif.inf)
