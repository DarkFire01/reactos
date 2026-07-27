# Source list mirrors dlls/qcap/Makefile.in upstream; keep them in sync on resync.
#
# Upstream splits qcap into a PE half and a Unix half (UNIXLIB = qcap.so), where
# v4l.c is the Linux video4linux backend and vfwcapture.c reaches it through
# WINE_UNIX_CALL. There is no Unix side here, so v4l.c and vfwcapture.c carry
# __REACTOS__ guards: v4l.c's body is already inside upstream's own
# HAVE_LINUX_VIDEODEV2_H conditional and compiles away to nothing, and
# vfwcapture.c's V4L_CALL becomes E_NOTIMPL. The VfW capture filter therefore
# reports no devices; the rest of qcap -- the AVI mux, the file writer, the
# smart tee and audio capture -- works normally.

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x600
    -DWINVER=0x600
    # qcap_main.c has its own DllRegisterServer and calls the modern
    # no-argument __wine_register_resources.
    -DUSE_NEW_WINE_REGISTER_RESOURCES)

spec2def(qcap.dll qcap.spec)

list(APPEND SOURCE
    audiorecord.c
    avico.c
    avimux.c
    capturegraph.c
    filewriter.c
    qcap_main.c
    smartteefilter.c
    v4l.c
    vfwcapture.c)

add_library(qcap MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/qcap.def)

# qcap_classes.idl carries "#pragma makedep register": it is the COM
# registration data for DllRegisterServer, not a header. Must come after
# add_library(): the helper appends to the existing target.
add_idl_reg_scripts(qcap registry qcap_classes.idl)

set_module_type(qcap win32dll)
# wine_dll_register/wine_dll_canunload supply the DllRegisterServer,
# DllUnregisterServer and DllCanUnloadNow exports declared in qcap.spec.
# qcap_main.c defines DllRegisterServer/DllUnregisterServer itself, so only
# the DllCanUnloadNow helper is needed here.
target_link_libraries(qcap wine wine_dll_canunload strmbase strmiids uuid oldnames)
add_delay_importlibs(qcap msvfw32)
add_importlibs(qcap
    ole32 oleaut32 winmm user32 gdi32 advapi32
    msvcrt kernel32 ntdll)
add_dependencies(qcap wineheaders)
add_cd_file(TARGET qcap DESTINATION reactos/system32 FOR all)
set_wine_module(qcap)
