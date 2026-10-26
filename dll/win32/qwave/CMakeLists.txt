remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

spec2def(qwave.dll qwave.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    qwave.c
    ${CMAKE_CURRENT_BINARY_DIR}/qwave_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/qwave.def)

add_library(qwave MODULE ${SOURCE} qwave.rc)
set_module_type(qwave win32dll)
add_importlibs(qwave ws2_32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET qwave DESTINATION reactos/system32 FOR all)
