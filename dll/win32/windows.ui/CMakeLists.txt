remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x602)

spec2def(windows.ui.dll windows.ui.spec)

list(APPEND SOURCE
    uisettings.cpp)

add_library(windows.ui MODULE
    ${SOURCE}
    windows.ui.rc
    ${CMAKE_CURRENT_BINARY_DIR}/windows.ui_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/windows.ui.def)

set_module_type(windows.ui win32dll UNICODE)
target_link_libraries(windows.ui uuid cppstl)
add_importlibs(windows.ui combase ole32 user32 advapi32 msvcrt kernel32 ntdll)
add_dependencies(windows.ui psdk)
add_cd_file(TARGET windows.ui DESTINATION reactos/system32 FOR all)
