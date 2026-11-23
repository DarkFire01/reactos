
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x603 -DNTDDI_VERSION=0x06030000)

add_library(intelppm MODULE intelppm.c intelppm.rc)
set_module_type(intelppm kernelmodedriver)
target_link_libraries(intelppm ppmlib)
add_importlibs(intelppm hal ntoskrnl)
add_cd_file(TARGET intelppm DESTINATION reactos/system32/drivers FOR all)
