
list(APPEND SOURCE
    driver.c
    pch.h)

list(APPEND PCH_SKIP_SOURCE
    guid.c)

add_library(usbhub3 MODULE
           ${SOURCE}
           ${PCH_SKIP_SOURCE}
           usbhub3.rc)

if(MSVC)
    target_compile_options(usbhub3 PRIVATE /wd4996)
endif()

set_module_type(usbhub3 kmdfdriver)
add_importlibs(usbhub3 wdfldr ntoskrnl hal)
add_pch(usbhub3 pch.h SOURCE)
add_cd_file(TARGET usbhub3 DESTINATION reactos/system32/drivers NO_CAB FOR all)
