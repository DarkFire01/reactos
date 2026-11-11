
spec2def(qcomrpmh.sys qcomrpmh.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomrpmh MODULE
    qcomrpmh.c
    qcomrpmh.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomrpmh.def)

set_module_type(qcomrpmh kernelmodedriver)
target_link_libraries(qcomrpmh libcntpr)
add_importlibs(qcomrpmh qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomrpmh DESTINATION reactos/system32/drivers NO_CAB FOR all)
