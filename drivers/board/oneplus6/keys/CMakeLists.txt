
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcomkeys MODULE
    qcomkeys.c
    qcomkeys.rc)

set_module_type(qcomkeys kernelmodedriver)
target_link_libraries(qcomkeys dmilib libcntpr)
add_importlibs(qcomkeys qcomdbg qcomspmi ntoskrnl hal)
add_cd_file(TARGET qcomkeys DESTINATION reactos/system32/drivers NO_CAB FOR all)
