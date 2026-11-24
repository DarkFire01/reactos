
spec2def(wmilib.sys wmilib.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wmilib.c
    wmilib.rc
    ${CMAKE_CURRENT_BINARY_DIR}/wmilib.def)

add_library(wmilib MODULE ${SOURCE})
set_module_type(wmilib kernelmodedriver ENTRYPOINT 0)
add_importlibs(wmilib ntoskrnl)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(wmilib memcmp)
endif()
add_cd_file(TARGET wmilib DESTINATION reactos/system32/drivers NO_CAB FOR all)
