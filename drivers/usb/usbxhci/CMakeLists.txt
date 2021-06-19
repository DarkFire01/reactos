
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers)

list(APPEND SOURCE
    #debug.c
    roothub.c
    usbxhci.c
    usbxhcip.c
    usbxhci.h)

add_library(usbxhci SHARED
    ${SOURCE}
    guid.c
    usbxhci.rc)

set_module_type(usbxhci kernelmodedriver)
add_importlibs(usbxhci usbport ntoskrnl hal usbd)
add_pch(usbxhci usbxhci.h SOURCE)
add_cd_file(TARGET usbxhci DESTINATION reactos/system32/drivers NO_CAB FOR all)

