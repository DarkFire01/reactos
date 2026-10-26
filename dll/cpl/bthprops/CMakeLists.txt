
spec2def(bthprops.cpl bthprops.cpl.spec)
add_rc_deps(bthprops.rc ${REACTOS_SOURCE_DIR}/dll/win32/bthci/resources/bluetooth.ico)

list(APPEND SOURCE
    bthprops.c
    bthprops.rc
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops.def)

add_library(bthprops MODULE ${SOURCE})
set_module_type(bthprops cpl UNICODE)
target_link_libraries(bthprops wine wine_dll_canunload)
add_importlibs(bthprops bluetoothapis comctl32 user32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET bthprops DESTINATION reactos/system32 FOR all)
