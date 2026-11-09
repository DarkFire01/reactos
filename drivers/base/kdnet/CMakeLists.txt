spec2def(kdnet.dll kdnet.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    kdnet_aux.c
    kdnet_crypto_prim.c
    kdnet_crypto.c
    kdnet_ext.c
    kdnet_init.c
    kdnet_net.c
    kdnet_packet.c
    kdnet_power.c
    )

if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    list(APPEND SOURCE arch/x86/kdnethw.c)
endif()

add_library(kdnet MODULE
    ${SOURCE}
    kdnet.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kdnet.def)

set_module_type(kdnet kerneldll ENTRYPOINT 0)
set_subsystem(kdnet native)
add_importlibs(kdnet ntoskrnl hal kdstub)
add_dependencies(kdnet psdk bugcodes)
add_cd_file(TARGET kdnet DESTINATION reactos/system32 NO_CAB FOR all)
