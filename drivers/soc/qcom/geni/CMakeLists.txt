
spec2def(qcomgeni.sys qcomgeni.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomgeni MODULE
    qcomgeni.c
    qcomgeni.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomgeni.def)

set_module_type(qcomgeni kernelmodedriver)
target_link_libraries(qcomgeni libcntpr)
add_importlibs(qcomgeni qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomgeni DESTINATION reactos/system32/drivers NO_CAB FOR all)
