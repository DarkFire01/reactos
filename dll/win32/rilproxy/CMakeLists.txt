
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

spec2def(rilproxy.dll rilproxy.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    client.c
    exports.c
    simple.c
    rilproxy.h)

add_library(rilproxy MODULE
    ${SOURCE}
    rilproxy.rc
    ${CMAKE_CURRENT_BINARY_DIR}/rilproxy_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/rilproxy.def)

set_module_type(rilproxy win32dll UNICODE)
target_link_libraries(rilproxy uuid)
add_importlibs(rilproxy setupapi advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_pch(rilproxy rilproxy.h SOURCE)
add_cd_file(TARGET rilproxy DESTINATION reactos/system32 FOR all)
