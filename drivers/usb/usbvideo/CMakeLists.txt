

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

list(APPEND SOURCE
    uvc.c
    bulk.c
    iso.c
    usbvideo.c
    pool.c
    filter.c
    pin.c
    guid.c
    descriptor.c)

add_library(usbvideo MODULE
    ${SOURCE}
    usbvideo.rc)

target_link_libraries(usbvideo
    libcntpr
    ${PSEH_LIB})

set_module_type(usbvideo kernelmodedriver)
add_importlibs(usbvideo ntoskrnl hal usbd ks)

add_cd_file(TARGET usbvideo DESTINATION reactos/system32/drivers NO_CAB FOR all)
