
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x603 -DNTDDI_VERSION=0x06030000)

add_library(amdppm MODULE amdppm.c amdppm.rc)
set_module_type(amdppm kernelmodedriver)
target_link_libraries(amdppm ppmlib)
add_importlibs(amdppm hal ntoskrnl)
add_cd_file(TARGET amdppm DESTINATION reactos/system32/drivers FOR all)
