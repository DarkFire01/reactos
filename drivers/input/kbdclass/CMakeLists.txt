
list(APPEND SOURCE
    kbdclass.c
    io.c
    pnp.c
    power.c
    wmi.c)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

add_library(kbdclass MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    kbdclass.rc)

set_module_type(kbdclass kernelmodedriver)
add_pch(kbdclass kbdclass.h "${PCH_SKIP_SOURCE}")
add_importlibs(kbdclass ntoskrnl hal wmilib)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(kbdclass memcmp)
endif()
add_cd_file(TARGET kbdclass DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(kbdclass_reg.inf)
