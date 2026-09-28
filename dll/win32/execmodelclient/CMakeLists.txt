spec2def(execmodelclient.dll execmodelclient.spec)

list(APPEND SOURCE
    execmodelclient.c
    ${CMAKE_CURRENT_BINARY_DIR}/execmodelclient.def)

add_library(execmodelclient MODULE ${SOURCE} execmodelclient.rc)
set_module_type(execmodelclient win32dll ENTRYPOINT DllMain 12)
add_importlibs(execmodelclient combase kernel32 ntdll)
target_link_libraries(execmodelclient wine uuid)
add_cd_file(TARGET execmodelclient DESTINATION reactos/system32 FOR all)
