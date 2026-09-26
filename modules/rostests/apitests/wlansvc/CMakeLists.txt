
add_executable(wlansvc_apitest crypto.c testlist.c)
target_link_libraries(wlansvc_apitest wlcrypto)
set_module_type(wlansvc_apitest win32cui)
add_importlibs(wlansvc_apitest advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wlansvc_apitest)
