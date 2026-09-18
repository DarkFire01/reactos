
# USBD_HANDLE and everything built on it only appear from Vista onwards, so the
# tree default has to come off before the floor this library needs goes on.
remove_definitions(-D_WIN32_WINNT=0x502)

add_definitions(
    -D_NTSYSTEM_
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

list(APPEND SOURCE
    usbdex.c)

add_library(usbdex ${SOURCE})
add_dependencies(usbdex xdk)
