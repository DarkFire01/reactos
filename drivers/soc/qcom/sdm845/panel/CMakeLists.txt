
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcompanel MODULE
    qcompanel.c
    qcompanel.rc)

set_module_type(qcompanel kernelmodedriver)
target_link_libraries(qcompanel qcompnp libcntpr)
add_importlibs(qcompanel qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcompanel DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcompanel qcompanel.inf)
add_registry_inf(qcompanel_reg.inf)
