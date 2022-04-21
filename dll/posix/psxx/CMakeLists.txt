
add_definitions(-D_PSXX_)
spec2def(psxx.dll psxx.spec)

list(APPEND SOURCE
    psxx.c
    stubs.c)

add_library(psxx MODULE
    ${SOURCE}
    psxx.rc
    ${CMAKE_CURRENT_BINARY_DIR}/psxx.def
	${CMAKE_CURRENT_BINARY_DIR}/psxx_stubs.c)

set_module_type(psxx win32dll ENTRYPOINT DllMain)
add_importlibs(psxx kernel32 ntdll msvcrt gdi32)
add_dependencies(psxx psdk)
add_cd_file(TARGET psxx DESTINATION reactos/system32 FOR all)
