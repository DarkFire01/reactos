
spec2def(bthprops.dll bthprops.spec)

list(APPEND SOURCE
    bthprops.c
    enum.c)

file(GLOB bthprops_rc_deps resources/*.*)
add_rc_deps(bthprops.rc ${bthprops_rc_deps})

add_library(bthprops MODULE
    ${SOURCE}
    bthprops.rc
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops.def
    ${CMAKE_CURRENT_BINARY_DIR}/bthprops_stubs.c)

set_module_type(bthprops win32dll UNICODE)
add_importlibs(bthprops user32 gdi32 advapi32 setupapi comctl32 msvcrt kernel32 ntdll)
add_cd_file(TARGET bthprops DESTINATION reactos/system32 FOR all)
