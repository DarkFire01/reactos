remove_definitions(-D_WIN32_WINNT=0x502)

add_library(urssynopsys MODULE urssynopsys.c urssynopsys.rc)
set_module_type(urssynopsys kmdfdriver)

target_compile_definitions(urssynopsys PRIVATE
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

target_link_libraries(urssynopsys ursstub ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(urssynopsys wdfldr ntoskrnl hal)
add_cd_file(TARGET urssynopsys DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(urssynopsys urssynopsys.inf)
