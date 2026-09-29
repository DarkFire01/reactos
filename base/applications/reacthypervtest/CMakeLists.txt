# The contract this drives real hardware through is the one ReacTVmm was
# written against, so it is read from there rather than copied.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../reactvmm/include)

add_executable(reacthypervtest
    main.cpp
    runner.cpp)
set_module_type(reacthypervtest win32cui)
add_importlibs(reacthypervtest ole32 msvcrt kernel32 ntdll)
add_dependencies(reacthypervtest psdk)
add_cd_file(TARGET reacthypervtest DESTINATION reactos/system32 FOR all)
