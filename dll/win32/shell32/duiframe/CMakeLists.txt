
# Declared from DarkFiresReactOSModules/ModernUI so modernui_target and the ReactUI build
# settings exist.

list(APPEND SOURCE
    commonlayout.cpp
    duihelp.cpp
    elements.cpp
    hwndview.cpp
    modules.cpp
    sizer.cpp
    templatebg.cpp
    viewframe.cpp
    exports.cpp
    precomp.h)

add_library(shell32_duiframe ${SOURCE} guids.c)
modernui_target(shell32_duiframe)
target_link_libraries(shell32_duiframe modernui_guids shellstock)
add_dependencies(shell32_duiframe psdk)
add_pch(shell32_duiframe precomp.h "guids.c")

# shell32 is configured before this directory, so the frame is attached to it from here
target_link_libraries(shell32 shell32_duiframe)
target_compile_definitions(shell32 PRIVATE SHELL32_DUIFRAME)
add_delay_importlibs(shell32 dui70 propsys)
