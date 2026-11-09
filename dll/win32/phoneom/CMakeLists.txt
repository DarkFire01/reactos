remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

spec2def(phoneom.dll phoneom.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    phoneom.c)

add_library(phoneom MODULE
    ${SOURCE}
    phoneom.rc
    ${CMAKE_CURRENT_BINARY_DIR}/phoneom.def)

set_module_type(phoneom win32dll UNICODE)
add_importlibs(phoneom msvcrt kernel32 ntdll)
add_cd_file(TARGET phoneom DESTINATION reactos/system32 FOR all)
