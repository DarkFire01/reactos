spec2def(qcomamp.sys qcomamp.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomamp MODULE
    amp.c
    qcomamp.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomamp.def)

set_module_type(qcomamp kernelmodedriver)
target_link_libraries(qcomamp libcntpr)
add_importlibs(qcomamp qcomgeni qcomtlmm qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomamp DESTINATION reactos/system32/drivers NO_CAB FOR all)
