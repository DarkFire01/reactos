
spec2def(watchdog.sys watchdog.spec ADD_IMPORTLIB)

add_library(watchdog MODULE
    watchdog.c
    ${CMAKE_CURRENT_BINARY_DIR}/watchdog.def)

set_module_type(watchdog kernelmodedriver)
add_importlibs(watchdog ntoskrnl)
# amd64 ships the Windows watchdog.sys from modules/ instead
if(NOT ARCH STREQUAL "amd64")
    add_cd_file(TARGET watchdog DESTINATION reactos/system32/drivers FOR all)
endif()
