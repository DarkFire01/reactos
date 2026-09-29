add_executable(reactvmm
    main.cpp
    machine.cpp
    bus.cpp

    devicehost.cpp
    vdevhost.cpp
    vdevguid.cpp
    panel.cpp
    newdelete.cpp
    reactvmm.rc)
set_module_type(reactvmm win32cui)
add_importlibs(reactvmm user32 gdi32 advapi32 msvcrt kernel32 ntdll)
add_dependencies(reactvmm psdk)
add_cd_file(TARGET reactvmm DESTINATION reactos/system32 FOR all)
