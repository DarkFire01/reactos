
spec2def(sdport.sys sdport.spec ADD_IMPORTLIB)

remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(
    -D_WIN32_WINNT=0x602
    -DNTDDI_VERSION=0x06020000)

list(APPEND SOURCE
    sdport.c
    pnp.c
    request.c
    power.c
    init_card.c
    precomp.h)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

add_library(sdport MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    sdport.rc
    ${CMAKE_CURRENT_BINARY_DIR}/sdport.def)

add_pch(sdport precomp.h "${PCH_SKIP_SOURCE}")
set_module_type(sdport kernelmodedriver)
add_importlibs(sdport ntoskrnl hal)
if(ARCH STREQUAL "i386")
    # The x86 kernel does not export memcmp
    target_link_libraries(sdport memcmp)
endif()
add_cd_file(TARGET sdport DESTINATION reactos/system32/drivers NO_CAB FOR all)
