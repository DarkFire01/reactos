
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x602)

include_directories("include/")

spec2def(dxgkrnl.sys dxgkrnl.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    entry.c
    registry.cpp
    resource.cpp
    dxgkinterface.cpp
    dxgkinterrupts.cpp
    miniport/dxgkport.cpp
    miniport/miniport.cpp
    vidpnss/vidpnmgr.cpp
    vidpnss/vidpninterface.cpp
    vidpnss/vidpnsourcemodset.cpp
    vidpnss/vidpntargetmodeset.cpp
    vidpnss/vidpntopology.cpp
    services/dxgkagp.cpp
    services/dxgkbdd.cpp
    services/dxgkbugreport.cpp
    services/dxgkfirmwaretable.cpp
    services/dxgkidd.cpp
    services/dxgkspb.cpp
    services/dxgktimedoperation.cpp)

add_library(dxgkrnl MODULE
    ${SOURCE}
    dxgkrnl.rc
    ${CMAKE_CURRENT_BINARY_DIR}/dxgkrnl_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/dxgkrnl.def)

set_module_type(dxgkrnl kernelmodedriver)
add_importlibs(dxgkrnl ntoskrnl hal)
add_cd_file(TARGET dxgkrnl DESTINATION reactos/system32/drivers NO_CAB FOR all)
