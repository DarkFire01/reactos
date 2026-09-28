spec2def(hvsocket.sys hvsocket.spec)

list(APPEND SOURCE
    hvsocket.c
    ${CMAKE_CURRENT_BINARY_DIR}/hvsocket.def)

add_library(hvsocket MODULE ${SOURCE} hvsocket.rc)
set_module_type(hvsocket kernelmodedriver)
add_importlibs(hvsocket ntoskrnl)

# Microsoft's own is a strict superset of this: the three the machine bus
# imports, plus the fifteen its control driver wants, behind a real transport.
# It ships instead whenever it is there, since two files cannot both be
# system32\drivers\hvsocket.sys. This stays as what to fall back to, because a
# dependency whose DllInitialize fails takes the driver that imported it down
# with it.
if(ARCH STREQUAL "amd64")
    set(HVSOCKET_SLIPSTREAM ${REACTOS_SOURCE_DIR}/modules/x64/drivers/hvsocket.sys)
else()
    set(HVSOCKET_SLIPSTREAM ${REACTOS_SOURCE_DIR}/modules/x86/drivers/hvsocket.sys)
endif()

if(NOT EXISTS ${HVSOCKET_SLIPSTREAM})
    add_cd_file(TARGET hvsocket DESTINATION reactos/system32/drivers FOR all)
endif()
