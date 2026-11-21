
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x600 -DWINVER=0x600)

spec2def(qcril.dll qcril.spec)

list(APPEND SOURCE
    codec.c
    dcm.c
    ipc.c
    ril.c
    dms.c
    nas.c
    pdc.c
    ims.c
    imsdata.c
    uim.c
    voice.c
    wds.c
    wms.c
    qcril.h)

add_library(qcril MODULE
    ${SOURCE}
    qcril.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcril.def)

set_module_type(qcril win32dll UNICODE)
add_importlibs(qcril advapi32 kernel32_vista msvcrt kernel32 ntdll)
add_pch(qcril qcril.h SOURCE)
add_cd_file(TARGET qcril DESTINATION reactos/system32 FOR all)
