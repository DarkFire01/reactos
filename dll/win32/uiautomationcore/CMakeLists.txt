
spec2def(UIAutomationCore.dll uiautomationcore.spec)

list(APPEND SOURCE
    UIAutomationCore.rc
    ${CMAKE_CURRENT_BINARY_DIR}/UIAutomationCore.def)

add_library(UIAutomationCore MODULE ${SOURCE})
set_module_type(UIAutomationCore nativedll)
add_importlibs(UIAutomationCore ntdll)
add_cd_file(TARGET UIAutomationCore DESTINATION reactos/system32 FOR all)
