
add_definitions(-D_WIN32_WINNT=0x600 -DNTDDI_VERSION=NTDDI_VISTA)

spec2def(fsdepends.sys fsdepends.spec ADD_IMPORTLIB)

add_library(fsdepends MODULE
    fsdepends.c
    fsdepends.rc
    ${CMAKE_CURRENT_BINARY_DIR}/fsdepends.def)

set_module_type(fsdepends kernelmodedriver)
add_importlibs(fsdepends ntoskrnl hal)
add_cd_file(TARGET fsdepends DESTINATION reactos/system32/drivers FOR all)
