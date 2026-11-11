
spec2def(qcomspmi.sys qcomspmi.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomspmi MODULE
    qcomspmi.c
    qcomspmi.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcomspmi.def)

set_module_type(qcomspmi kernelmodedriver)
target_link_libraries(qcomspmi libcntpr)
add_importlibs(qcomspmi qcomdbg ntoskrnl hal)
add_cd_file(TARGET qcomspmi DESTINATION reactos/system32/drivers NO_CAB FOR all)
