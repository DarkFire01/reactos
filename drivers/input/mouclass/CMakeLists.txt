
list(APPEND SOURCE
    dispatch.c
    mouclass.c
    pnp.c
    power.c)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

add_library(mouclass MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    mouclass.rc)

set_module_type(mouclass kernelmodedriver)
add_importlibs(mouclass ntoskrnl hal wmilib)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(mouclass memcmp)
endif()
add_pch(mouclass mouclass.h "${PCH_SKIP_SOURCE}")
add_cd_file(TARGET mouclass DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(mouclass_reg.inf)
