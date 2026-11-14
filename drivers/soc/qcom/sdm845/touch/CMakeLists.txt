
list(APPEND SOURCE
    hid.c
    qcomtouch.c
    rmi4.c
    spb.c
    qcomtouch.h)

add_library(qcomtouch MODULE ${SOURCE} qcomtouch.rc)
set_module_type(qcomtouch kmdfdriver)

target_compile_definitions(qcomtouch PRIVATE
    _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(qcomtouch dmilib ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(qcomtouch qcomrpmh wdfldr ntoskrnl hal)
add_cd_file(TARGET qcomtouch DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(qcomtouch qcomtouch.inf)
add_registry_inf(qcomtouch_reg.inf)
