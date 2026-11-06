
include_directories(${REACTOS_SOURCE_DIR}/win32ss)

spec2def(win32u.dll win32u.spec ADD_IMPORTLIB)
add_asm_files(win32u_asm sys-stubs.S)

add_library(win32u MODULE
    main.c
    ${win32u_asm}
    ${CMAKE_CURRENT_BINARY_DIR}/win32u.def)

set_module_type(win32u module)

# The ARM64EC run time checks call memcpy instead of inlining it
if(ARCH STREQUAL "arm64ec")
    target_link_libraries(win32u libcntpr)
endif()
add_dependencies(win32u psdk)
add_cd_file(TARGET win32u DESTINATION reactos/system32 FOR all)

if (STACK_PROTECTOR)
    target_compile_options(win32u PRIVATE -fno-stack-protector)
endif()
