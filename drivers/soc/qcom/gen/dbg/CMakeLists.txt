
spec2def(qcomdbg.sys qcomdbg.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomdbg MODULE
    qcomdbg.c
    qcomdbg.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomdbg.def)

set_module_type(qcomdbg kernelmodedriver)
target_link_libraries(qcomdbg ntoskrnl_vista libcntpr)
add_importlibs(qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomdbg DESTINATION reactos/system32/drivers NO_CAB FOR all)
