# The machine is the one the terminal front end drives, and the contract it
# speaks is ReacTVmm's, so both are read from where they already are.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../reactvmm/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../reacthypervtest)

add_executable(reacthypervgui
    main.cpp
    ../reacthypervtest/vm.cpp
    ../reacthypervtest/runner.cpp
    ../reacthypervtest/newdelete.cpp)
set_module_type(reacthypervgui win32gui)
add_importlibs(reacthypervgui user32 gdi32 comdlg32 ole32 oleaut32 msvcrt
                              kernel32 ntdll)
add_dependencies(reacthypervgui psdk)
add_cd_file(TARGET reacthypervgui DESTINATION reactos/system32 FOR all)
