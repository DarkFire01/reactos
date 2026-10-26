spec2def(shacct.dll shacct.spec)

file(GLOB shacct_rc_deps res/*.*)
add_rc_deps(shacct.rc ${shacct_rc_deps})

list(APPEND SOURCE
    localaccounts.cpp
    sampropstore.cpp
    samutil.cpp
    shacct.cpp
    precomp.h)

add_library(shacct MODULE
    ${SOURCE}
    shacct.rc
    ${CMAKE_CURRENT_BINARY_DIR}/shacct.def)

set_module_type(shacct win32dll UNICODE)
target_link_libraries(shacct uuid cpprt atl_classes)
add_importlibs(shacct samlib userenv shell32 shlwapi ole32 oleaut32 advapi32 user32 msvcrt kernel32 ntdll)
add_pch(shacct precomp.h SOURCE)
add_cd_file(TARGET shacct DESTINATION reactos/system32 FOR all)

# DllInstall writes these on install; the LiveCD profile is read only, so it carries them
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/res/user.bmp ${CMAKE_CURRENT_SOURCE_DIR}/res/guest.bmp
            DESTINATION "ProgramData/Microsoft/User Account Pictures" NO_CAB FOR livecd)
