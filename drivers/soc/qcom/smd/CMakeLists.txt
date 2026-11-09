remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    channel.c
    lock.c
    main.c
    smem.c
    smd.h)

add_library(qcomsmd MODULE ${SOURCE} qcomsmd.rc)
set_module_type(qcomsmd kmdfdriver)
target_link_libraries(qcomsmd ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomsmd wdfldr ntoskrnl hal)
add_cd_file(TARGET qcomsmd DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(qcomsmd qcomsmd.inf)
