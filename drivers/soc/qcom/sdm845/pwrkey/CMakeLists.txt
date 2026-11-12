
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcompwrkey MODULE
    qcompwrkey.c
    qcompwrkey.rc)

set_module_type(qcompwrkey kernelmodedriver)
target_link_libraries(qcompwrkey qcompnp libcntpr)
add_importlibs(qcompwrkey qcomdbg qcomspmi ntoskrnl hal)
add_cd_file(TARGET qcompwrkey DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcompwrkey qcompwrkey.inf)
add_registry_inf(qcompwrkey_reg.inf)
