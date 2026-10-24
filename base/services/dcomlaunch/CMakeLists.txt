spec2def(dcomlaunch.dll dcomlaunch.spec ADD_IMPORTLIB)

add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/rtlauncher.idl)

add_library(dcomlaunch MODULE
    dcomlaunch.c
    network.c
    launch.c
    rtserver.c
    ${CMAKE_CURRENT_BINARY_DIR}/rtlauncher_s.c
    dcomlaunch.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dcomlaunch.def)

set_module_type(dcomlaunch win32dll UNICODE)
target_link_libraries(dcomlaunch wine)
add_importlibs(dcomlaunch advapi32 rpcrt4 msvcrt kernel32 ntdll)
add_delay_importlibs(dcomlaunch userenv)
add_cd_file(TARGET dcomlaunch DESTINATION reactos/system32 FOR all)
