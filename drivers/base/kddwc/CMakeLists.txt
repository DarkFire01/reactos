
spec2def(kddwc.dll kddwc.spec ADD_IMPORTLIB)

# Under the KD protocol the port carries packets only
if(_WINKD_)
    add_definitions(-D_WINKD_)
endif()

list(APPEND SOURCE
    dwc3.c
    kddwc.c
    platform.c
    rpmh.c
    soc/msm8994.c
    soc/msm8998.c
    soc/sc7180.c
    soc/sc7280.c
    soc/sc8180x.c
    soc/sc8280xp.c
    soc/sdm845.c
    soc/sm8150.c
    soc/sm8350.c
    soc/x1e80100.c
    ../kdcom/kddll.c
    ../kdcom/kdserial.c
    kddwc.h)

add_library(kddwc MODULE
    ${SOURCE}
    kddwc.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kddwc.def)

set_module_type(kddwc kerneldll ENTRYPOINT 0)
set_subsystem(kddwc native)
add_importlibs(kddwc ntoskrnl hal)
add_dependencies(kddwc psdk bugcodes)
add_cd_file(TARGET kddwc DESTINATION reactos/system32 NO_CAB FOR all)
