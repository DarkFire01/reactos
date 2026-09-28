spec2def(kernelbase.dll kernelbase.spec)

add_library(kernelbase MODULE
    kernelbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase.def)

set_module_type(kernelbase win32dll ENTRYPOINT 0)
add_importlibs(kernelbase kernel32)
add_cd_file(TARGET kernelbase DESTINATION reactos/system32 FOR all)
