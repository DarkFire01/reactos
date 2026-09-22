
include_directories(${REACTOS_SOURCE_DIR}/drivers/bluetooth/fbtusb/include)

list(APPEND SOURCE
    bthprobe.c
    hci.c
    hid.c
    l2cap.c
    sdp.c
    security.c
    bthprobe.h)

add_executable(bthprobe ${SOURCE} bthprobe.rc)
set_module_type(bthprobe win32cui)
add_importlibs(bthprobe advapi32 msvcrt kernel32)
add_cd_file(TARGET bthprobe DESTINATION reactos/system32 FOR all)
