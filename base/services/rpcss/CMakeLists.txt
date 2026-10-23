
add_rpc_files(server
    ${REACTOS_SOURCE_DIR}/sdk/include/wine/epm.idl
    ${REACTOS_SOURCE_DIR}/sdk/include/wine/irot.idl
    ${REACTOS_SOURCE_DIR}/sdk/include/wine/irpcss.idl
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/rtscm.idl)


list(APPEND SOURCE
    epmp.c
    irotp.c
    rpcss_main.c
    setup.c
    precomp.h
    ${CMAKE_CURRENT_BINARY_DIR}/epm_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/irot_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/irpcss_s.c)

include_directories(${REACTOS_BINARY_DIR}/sdk/include/wine)
add_executable(rpcss ${SOURCE} rtlaunch.c rtscm.c ${CMAKE_CURRENT_BINARY_DIR}/rtscm_s.c rpcss.rc)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(rpcss PRIVATE "-Wno-cast-calling-convention")
endif()

target_link_libraries(rpcss ${PSEH_LIB} wine)
set_module_type(rpcss win32cui UNICODE)
add_importlibs(rpcss advapi32 rpcrt4 msvcrt kernel32 ntdll)
add_delay_importlibs(rpcss userenv)
add_pch(rpcss precomp.h SOURCE)
set_source_files_properties(rtlaunch.c rtscm.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
add_cd_file(TARGET rpcss DESTINATION reactos/system32 FOR all)
