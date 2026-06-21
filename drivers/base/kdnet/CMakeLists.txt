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

# Architecture-specific timing primitives (TSC + PIT calibration on x86).
# TODO: ARM/ARM64 (architected generic timer).
if(ARCH STREQUAL "i386")
    list(APPEND SOURCE i386/kdnethw.c)
elseif(ARCH STREQUAL "amd64")
    list(APPEND SOURCE amd64/kdnethw.c)
endif()

add_library(kdnet MODULE
    ${SOURCE}
    kdnet.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kdnet.def)

set_module_type(kdnet kerneldll ENTRYPOINT 0)
set_subsystem(kdnet native)
add_importlibs(kdnet ntoskrnl hal kd_02_8086)
add_dependencies(kdnet psdk bugcodes)
add_cd_file(TARGET kdnet DESTINATION reactos/system32 NO_CAB FOR all)
