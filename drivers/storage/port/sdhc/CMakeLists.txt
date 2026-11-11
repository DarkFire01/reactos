
list(APPEND SOURCE
    sdhc.c
    sdhci.c
    sdhci.h)

add_library(sdhc MODULE
    ${SOURCE}
    sdhc.rc)

set_module_type(sdhc kernelmodedriver)
add_importlibs(sdhc sdport ntoskrnl hal)
add_cd_file(TARGET sdhc DESTINATION reactos/system32/drivers NO_CAB FOR all)
