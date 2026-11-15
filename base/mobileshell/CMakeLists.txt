
# ReactOS Mobile Shell: a phone shell with the Windows 10 Mobile architecture (host, message
# session, UIX co-render engine, chrome and home components) and its out of process apps.
# Everything installs to %SystemRoot%\MobileShell, next to the skins and shell.xml it reads.

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/appclient
    ${REACTOS_SOURCE_DIR}/DarkFiresReactOSModules/dwm_win11/dcomp
    ${REACTOS_SOURCE_DIR}/DarkFiresReactOSModules/dwm_win11/include)

set(MOBILESHELL_DESTINATION reactos/MobileShell)

# C++20 against ReactRTCPP, as reactrtcpp_target does; ReactRT is added after base.
# The windows.h min and max macros break its <limits>, so they become templates instead.
function(mobileshell_cpp _target)
    set_target_properties(${_target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    set_target_cpp_properties(${_target} WITH_EXCEPTIONS WITH_RTTI)
    target_compile_definitions(${_target} PRIVATE NOMINMAX)
    target_compile_options(${_target} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/uixminmax.h")
    target_link_libraries(${_target} reactrtcpp cpprt uuid)
    add_dependencies(${_target} psdk)
endfunction()

# Message session
spec2def(coremessaging.dll coremessaging/coremessaging.spec ADD_IMPORTLIB)
add_library(coremessaging MODULE coremessaging/session.cpp ${CMAKE_CURRENT_BINARY_DIR}/coremessaging.def)
set_module_type(coremessaging win32dll UNICODE)
mobileshell_cpp(coremessaging)
add_importlibs(coremessaging msvcrt kernel32 ntdll)
add_cd_file(TARGET coremessaging DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

# UIX runtime and co-render engine
spec2def(uixrender.dll uixrender/uixrender.spec ADD_IMPORTLIB)
add_library(uixrender MODULE
    uixrender/engine.cpp
    uixrender/scene.cpp
    uixrender/theme.cpp
    uixrender/wicguids.c
    ${CMAKE_CURRENT_BINARY_DIR}/uixrender.def)
set_module_type(uixrender win32dll UNICODE)
mobileshell_cpp(uixrender)
add_importlibs(uixrender d3d11 d2d1 dwrite dcomp xmllite shlwapi ole32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET uixrender DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

# Shell components, loaded by name from the host
spec2def(shellchrome.dll shellchrome/shellchrome.spec)
add_library(shellchrome MODULE shellchrome/shellchrome.cpp ${CMAKE_CURRENT_BINARY_DIR}/shellchrome.def)
set_module_type(shellchrome win32dll UNICODE)
mobileshell_cpp(shellchrome)
add_importlibs(shellchrome iphlpapi ws2_32 ole32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET shellchrome DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

spec2def(mobileui.dll mobileui/mobileui.spec)
add_library(mobileui MODULE mobileui/mobileui.cpp ${CMAKE_CURRENT_BINARY_DIR}/mobileui.def)
set_module_type(mobileui win32dll UNICODE)
mobileshell_cpp(mobileui)
add_importlibs(mobileui xmllite shlwapi advapi32 iphlpapi ws2_32 ole32 user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET mobileui DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

# Host
add_executable(mobileshell phoneshellui/main.cpp uixrender/wicguids.c)
set_module_type(mobileshell win32gui UNICODE)
mobileshell_cpp(mobileshell)
add_importlibs(mobileshell coremessaging uixrender d3d11 dwmapi user32 gdi32 ole32 shell32 shlwapi xmllite msvcrt kernel32 ntdll)
add_cd_file(TARGET mobileshell DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

# Apps, each its own process over appclient
function(mobileshell_app _target _source)
    add_executable(${_target} ${_source} appclient/appclient.cpp)
    set_module_type(${_target} win32gui UNICODE)
    mobileshell_cpp(${_target})
    add_importlibs(${_target} uixrender d3d11 dxgi xmllite shlwapi psapi iphlpapi ws2_32 ole32 shell32 advapi32 user32 msvcrt kernel32 ntdll)
    add_cd_file(TARGET ${_target} DESTINATION ${MOBILESHELL_DESTINATION} FOR all)
endfunction()

mobileshell_app(mobsettings apps/settings/settings.cpp)
mobileshell_app(mobcontacts apps/contacts/contacts.cpp)
mobileshell_app(mobcalendar apps/calendar/calendar.cpp)
mobileshell_app(mobcalc apps/calculator/calculator.cpp)
mobileshell_app(mobmarkup apps/markupapp/markupapp.cpp)
mobileshell_app(moboffice apps/office/office.cpp)
mobileshell_app(mobfiles apps/files/files.cpp)

# Configuration and skins keep their layout under MobileShell
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/shell.xml DESTINATION ${MOBILESHELL_DESTINATION} FOR all)

file(GLOB_RECURSE MOBILESHELL_SKIN_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/skins/*)
foreach(_file ${MOBILESHELL_SKIN_FILES})
    get_filename_component(_directory ${_file} DIRECTORY)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/${_file} DESTINATION ${MOBILESHELL_DESTINATION}/${_directory} FOR all)
endforeach()
