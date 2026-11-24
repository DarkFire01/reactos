
remove_definitions(-D_WIN32_WINNT=0x502)

spec2def(hidclass.sys hidclass.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    fdo.c
    file.c
    hidclass.c
    ioctl.c
    pdo.c
    power.c
    read.c
    wmi.c)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

add_library(hidclass MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    hidclass.rc
    ${CMAKE_CURRENT_BINARY_DIR}/hidclass.def)

set_module_type(hidclass kernelmodedriver ENTRYPOINT 0)
target_compile_definitions(hidclass PRIVATE _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)
target_link_libraries(hidclass ${PSEH_LIB})
add_importlibs(hidclass ntoskrnl hidparse wmilib hal)
add_pch(hidclass precomp.h "${PCH_SKIP_SOURCE}")
add_cd_file(TARGET hidclass DESTINATION reactos/system32/drivers NO_CAB FOR all)
