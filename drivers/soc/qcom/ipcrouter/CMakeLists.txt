remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    client.c
    core.c
    database.c
    main.c
    qsocket.c
    transport.c
    wire.c
    ipcrouter.h)

add_library(qcomipcrouter MODULE ${SOURCE} qcomipcrouter.rc)
set_module_type(qcomipcrouter kmdfdriver)
target_link_libraries(qcomipcrouter ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomipcrouter wdfldr ntoskrnl hal)
add_cd_file(TARGET qcomipcrouter DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomipcrouter qcomipcrouter.inf)
