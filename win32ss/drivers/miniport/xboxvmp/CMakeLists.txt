
list(APPEND SOURCE
    xboxvmp.c
    nv2a_accel.c
    xboxvmp.h
    nv2a_accel.h)

add_library(xboxvmp MODULE
    ${SOURCE}
    xboxvmp.rc)

set_module_type(xboxvmp kernelmodedriver)
add_importlibs(xboxvmp ntoskrnl videoprt)

# Set to 1 to enable NV2A hardware 2D acceleration by default.  Leave at 0
# until the driver has been validated against the target hardware (xemu / a
# real Xbox) — when disabled, the accelerator init code still runs but the
# DMA pusher is not engaged and the public IOCTLs fall back to a CPU memcpy.
target_compile_definitions(xboxvmp PRIVATE XBOX_NV2A_ACCEL_DEFAULT_ON=1)

if(SARCH STREQUAL "xbox")
    add_cd_file(TARGET xboxvmp DESTINATION reactos/system32/drivers FOR all)
    add_registry_inf(xboxvmp_reg.inf)
    add_driver_inf(xboxvmp xboxdisp.inf)
endif()
