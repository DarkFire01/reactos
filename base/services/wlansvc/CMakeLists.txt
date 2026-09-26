
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers/ndisuio
    ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/symcrypt/inc)
add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlansvc.idl)

list(APPEND SOURCE
    device.c
    supplicant.c
    wlansvc.c
    rpcserver.c
    precomp.h
    ${CMAKE_CURRENT_BINARY_DIR}/wlansvc_s.c)

# SymCrypt brings its own view of the Windows headers
list(APPEND PCH_SKIP_SOURCE
    crypto.c)

add_executable(wlansvc ${SOURCE} ${PCH_SKIP_SOURCE} wlansvc.rc)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(wlansvc PRIVATE "-Wno-cast-calling-convention")
endif()

if(MSVC)
    # warning C4068: unknown pragma 'GCC'
    set_source_files_properties(crypto.c PROPERTIES COMPILE_OPTIONS "/wd4068")
else()
    target_link_libraries(wlansvc ${PSEH_LIB})
endif()

target_link_libraries(wlansvc symcrypt)

set_module_type(wlansvc win32cui UNICODE)
add_importlibs(wlansvc advapi32 rpcrt4 iphlpapi msvcrt kernel32 ntdll)
add_pch(wlansvc precomp.h "${PCH_SKIP_SOURCE}")
add_cd_file(TARGET wlansvc DESTINATION reactos/system32 FOR all)
