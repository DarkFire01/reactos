
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../sdhc)

list(APPEND SOURCE
    qcsdhc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdhc/sdhci.c)

add_library(qcsdhc MODULE
    ${SOURCE}
    qcsdhc.rc)

set_module_type(qcsdhc kernelmodedriver)
add_importlibs(qcsdhc sdport ntoskrnl hal)
add_cd_file(TARGET qcsdhc DESTINATION reactos/system32/drivers NO_CAB FOR all)
