spec2def(kdusb.dll kdusb.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    ehcidbg.c
    kdusb.c
    xhcidbg.c
    kdusb.h)

if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    list(APPEND SOURCE arch/x86/kdusbhw.c)
else()
    list(APPEND SOURCE arch/generic/kdusbhw.c)
endif()

add_library(kdusb MODULE
    ${SOURCE}
    kdusb.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kdusb.def)

set_module_type(kdusb kerneldll ENTRYPOINT 0)
set_subsystem(kdusb native)
add_importlibs(kdusb ntoskrnl hal)
add_dependencies(kdusb psdk bugcodes)
add_cd_file(TARGET kdusb DESTINATION reactos/system32 NO_CAB FOR all)
