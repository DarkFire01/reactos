
spec2def(ndfapi.dll ndfapi.spec)

list(APPEND SOURCE
    ndfapi.c
    ${CMAKE_CURRENT_BINARY_DIR}/ndfapi.def)

add_library(ndfapi MODULE ${SOURCE})
set_module_type(ndfapi win32dll UNICODE ENTRYPOINT 0)
add_importlibs(ndfapi kernel32 ntdll)
add_cd_file(TARGET ndfapi DESTINATION reactos/system32 FOR all)
