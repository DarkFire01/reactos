
include_directories(${REACTOS_SOURCE_DIR}/drivers/bluetooth/fbtusb/include)

add_executable(bthprobe bthprobe.c bthprobe.rc)
set_module_type(bthprobe win32cui)
add_importlibs(bthprobe msvcrt kernel32)
add_cd_file(TARGET bthprobe DESTINATION reactos/system32 FOR all)
