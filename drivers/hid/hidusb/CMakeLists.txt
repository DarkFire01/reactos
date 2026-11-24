remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

list(APPEND SOURCE
    hidusb.c
    ioctl.c
    pnp.c
    usb.c
    hidusb.h)

add_library(hidusb MODULE
    ${SOURCE}
    hidusb.rc)

set_module_type(hidusb kernelmodedriver)
add_importlibs(hidusb hidclass ntoskrnl usbd hal)
add_cd_file(TARGET hidusb DESTINATION reactos/system32/drivers NO_CAB FOR all)
