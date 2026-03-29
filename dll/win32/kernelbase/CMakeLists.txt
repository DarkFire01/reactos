
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

if (DLL_EXPORT_VERSION LESS 0x601)
    set(kernelbase_name kernelbase)
else()
    set(kernelbase_name kernelbase)
endif()

add_subdirectory(wine)

add_definitions(-D_KERNELBASE_)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys)
spec2def(${kernelbase_name}.dll kernelbase.spec ADD_IMPORTLIB)

if(MSVC)
    # Disable warning C4267: 'initializing': conversion from 'size_t' to 'DWORD', possible loss of data
    set_source_files_properties(kernel32/winnls/string/locale.c PROPERTIES COMPILE_OPTIONS "/wd4267")
else()
    # Disable warning: 'variable' defined but not used
    set_source_files_properties(kernel32/winnls/string/locale.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable")
endif()

# This isn't called KernelBase because apps will explode and we aren't win7 yet.
add_library(${kernelbase_name} MODULE
    wine/kernelbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/${kernelbase_name}_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/${kernelbase_name}.def)

if(MSVC)
    # Disable warning C4133: 'function': incompatible types - from 'WICPixelFormatNumericRepresentation *' to 'DWORD *'
    # Disable warning C4146: unary minus operator applied to unsigned type, result still unsigned
    target_compile_options(${kernelbase_name} PRIVATE /wd4047 /wd4024 /wd4133 /wd4146)
endif()

set_module_type(${kernelbase_name} win32dll ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(${kernelbase_name} console)
################# END  HACK #################

target_link_libraries(${kernelbase_name} win32_registry kernelbase_path kernelbase_static cryptlib wine chkstk ${PSEH_LIB})
add_importlibs(${kernelbase_name} ntdll)
add_pch(${kernelbase_name} kernel32/k32.h SOURCE)
add_dependencies(${kernelbase_name} psdk errcodes asm)
add_cd_file(TARGET ${kernelbase_name} DESTINATION reactos/system32 FOR all)
