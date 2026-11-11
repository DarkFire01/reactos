
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

add_library(qcombatt MODULE
    qcombatt.c
    qcombatt.rc)

set_module_type(qcombatt kernelmodedriver)
target_link_libraries(qcombatt dmilib libcntpr)
add_importlibs(qcombatt battc qcomdbg qcomgeni ntoskrnl hal)
add_cd_file(TARGET qcombatt DESTINATION reactos/system32/drivers NO_CAB FOR all)
