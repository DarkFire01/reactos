
spec2def(authui.dll authui.spec)

file(GLOB authui_rc_deps res/*.*)
add_rc_deps(authui.rc ${authui_rc_deps})

list(APPEND SOURCE
    authui.cpp
    shutdownchoices.cpp
    precomp.h)

add_library(authui MODULE
    ${SOURCE}
    authui.rc
    ${CMAKE_CURRENT_BINARY_DIR}/authui.def)

set_module_type(authui win32dll UNICODE)
target_link_libraries(authui uuid cpprt atl_classes)
add_importlibs(authui powrprof shlwapi ole32 oleaut32 advapi32 user32 msvcrt kernel32 ntdll)
add_pch(authui precomp.h SOURCE)
add_cd_file(TARGET authui DESTINATION reactos/system32 FOR all)
