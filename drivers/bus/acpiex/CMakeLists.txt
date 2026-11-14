
list(APPEND SOURCE
    acpiex.c
    translate.c
    acpiexp.h)

remove_definitions(-D_WIN32_WINNT=0x502)

add_library(acpiex MODULE ${SOURCE} acpiex.rc)
target_compile_definitions(acpiex PRIVATE _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)
set_module_type(acpiex kernelmodedriver)
add_importlibs(acpiex ntoskrnl hal)
add_cd_file(TARGET acpiex DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_registry_inf(acpiex_reg.inf)
