
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomtypec MODULE
    qcomtypec.c
    qcomtypec.rc)

set_module_type(qcomtypec kernelmodedriver)
target_link_libraries(qcomtypec qcompnp libcntpr)
add_importlibs(qcomtypec qcomdbg qcomspmi ntoskrnl hal)
add_cd_file(TARGET qcomtypec DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcomtypec qcomtypec.inf)
add_registry_inf(qcomtypec_reg.inf)
