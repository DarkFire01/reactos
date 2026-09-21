
include_directories(../include)
include_directories(${REACTOS_SOURCE_DIR}/drivers/network/ndis/xlate)

# NDIS60 is what pulls the NET_BUFFER_LIST headers into ndis.h. The test is not
# a miniport, so it declares nothing else about itself.
#
# NDIS60 also implies protocol major version 6, which ndis.h refuses below
# NTDDI_VISTA. The tree default is 0x502, so it has to come off first.
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600 -DNDIS60)

list(APPEND NDIS_TEST_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    NdisNbl.c
    NdisXlate.c
    ${REACTOS_SOURCE_DIR}/drivers/network/ndis/xlate/xlate.c
    NdisNbl_drv.c)

add_library(ndisnbl_drv MODULE ${NDIS_TEST_DRV_SOURCE})
set_module_type(ndisnbl_drv kernelmodedriver)
target_link_libraries(ndisnbl_drv kmtest_printf ${PSEH_LIB})
add_importlibs(ndisnbl_drv ndis ntoskrnl hal)
target_compile_definitions(ndisnbl_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET ndisnbl_drv)
add_rostests_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/ndisnbl.cmd)
