
spec2def(qcomtlmm.sys qcomtlmm.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomtlmm MODULE
    qcomtlmm.c
    qcomtlmm.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomtlmm.def)

set_module_type(qcomtlmm kernelmodedriver)
target_link_libraries(qcomtlmm qcompnp libcntpr)
add_importlibs(qcomtlmm qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomtlmm DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcomtlmm qcomtlmm.inf)
add_registry_inf(qcomtlmm_reg.inf)
