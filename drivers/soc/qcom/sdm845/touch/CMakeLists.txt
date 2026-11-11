
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

list(APPEND SOURCE
    qcomtouch.c
    rmi4.c
    qcomtouch.h)

add_library(qcomtouch MODULE ${SOURCE} qcomtouch.rc)
set_module_type(qcomtouch kernelmodedriver)
target_link_libraries(qcomtouch qcompnp dmilib libcntpr)
add_importlibs(qcomtouch qcomdbg qcomgeni qcomtlmm qcomrpmh ntoskrnl hal)
add_cd_file(TARGET qcomtouch DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcomtouch qcomtouch.inf)
add_registry_inf(qcomtouch_reg.inf)
