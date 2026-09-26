
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/drivers/ndisuio
    ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/symcrypt/inc)
add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlansvc.idl)

# The supplicant crypto, on its own so the tests can link it
add_library(wlcrypto crypto.c sae.c)
target_include_directories(wlcrypto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wlcrypto symcrypt)
add_dependencies(wlcrypto psdk)

if(MSVC)
    # warning C4068: unknown pragma 'GCC'
    target_compile_options(wlcrypto PRIVATE /wd4068)
endif()

list(APPEND SOURCE
    device.c
    supplicant.c
    wlansvc.c
    rpcserver.c
    precomp.h
    ${CMAKE_CURRENT_BINARY_DIR}/wlansvc_s.c)

add_executable(wlansvc ${SOURCE} wlansvc.rc)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(wlansvc PRIVATE "-Wno-cast-calling-convention")
endif()

if(NOT MSVC)
    target_link_libraries(wlansvc ${PSEH_LIB})
endif()

target_link_libraries(wlansvc wlcrypto)

set_module_type(wlansvc win32cui UNICODE)
add_importlibs(wlansvc advapi32 rpcrt4 iphlpapi msvcrt kernel32 ntdll)
add_pch(wlansvc precomp.h SOURCE)
add_cd_file(TARGET wlansvc DESTINATION reactos/system32 FOR all)
