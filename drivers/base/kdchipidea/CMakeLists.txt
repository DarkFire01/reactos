
spec2def(kdchipidea.dll kdchipidea.spec ADD_IMPORTLIB)

# Under the KD protocol the port carries packets only
if(_WINKD_)
    add_definitions(-D_WINKD_)
endif()

list(APPEND SOURCE
    chipidea.c
    kdci.c
    platform.c
    soc/msm8916.c
    ../kdcom/kddll.c
    ../kdcom/kdserial.c
    kdci.h)

add_library(kdchipidea MODULE
    ${SOURCE}
    kdchipidea.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kdchipidea.def)

set_module_type(kdchipidea kerneldll ENTRYPOINT 0)
set_subsystem(kdchipidea native)
add_importlibs(kdchipidea ntoskrnl hal)
add_dependencies(kdchipidea psdk bugcodes)
add_cd_file(TARGET kdchipidea DESTINATION reactos/system32 NO_CAB FOR all)
