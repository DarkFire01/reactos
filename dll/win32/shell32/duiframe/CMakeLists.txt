
# Declared from DarkFiresReactOSModules/ModernUI so modernui_target and the ReactUI build
# settings exist.

list(APPEND SOURCE
    cidlabel.cpp
    cmdguids.cpp
    cmdhandlers.cpp
    cmdutil.cpp
    commandfolder.cpp
    commandmodule.cpp
    commonlayout.cpp
    duihelp.cpp
    elements.cpp
    hwndview.cpp
    infopane.cpp
    librarypane.cpp
    modules.cpp
    propertree.cpp
    readingpane.cpp
    sizer.cpp
    taskmanager.cpp
    thumbnail.cpp
    metadata.cpp
    templatebg.cpp
    viewframe.cpp
    foldermenu.cpp
    legacycmd.cpp
    regcommand.cpp
    splitbutton.cpp
    viewcontrol.cpp
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
add_delay_importlibs(shell32 dui70 duser propsys)

# The live image builds its registry from inf files and never runs DllRegisterServer
add_registry_inf(commandbar_reg.inf)
