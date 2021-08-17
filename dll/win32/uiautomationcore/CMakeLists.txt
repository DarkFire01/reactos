
spec2def(UIAutomationCore.dll uiautomationcore.spec)

list(APPEND SOURCE
    uiautomationcore.rc
    ${CMAKE_CURRENT_BINARY_DIR}/UIAutomationCore.def)

add_library(uiautomationcore MODULE ${SOURCE})
set_module_type(uiautomationcore nativedll)
add_importlibs(uiautomationcore ntdll)
add_cd_file(TARGET uiautomationcore DESTINATION reactos/system32 FOR all)
