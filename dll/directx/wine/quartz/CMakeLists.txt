# Source list mirrors dlls/quartz/Makefile.in upstream; keep them in sync on resync.

remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D__ROS_LONG64__
    -D_WIN32_WINNT=0x600
    -DWINVER=0x600
    # main.c calls the QUARTZ_-prefixed entry points that the generated
    # proxy dlldata provides, and registers through the .rgs resources.
    -DENTRY_PREFIX=QUARTZ_
    -DPROXY_DELEGATION
    -DWINE_REGISTER_DLL)

spec2def(quartz.dll quartz.spec)

list(APPEND SOURCE
    acmwrapper.c
    avidec.c
    decoder.c
    dsoundrender.c
    filesource.c
    filtergraph.c
    filtermapper.c
    main.c
    memallocator.c
    parser.c
    passthrough.c
    systemclock.c
    videorenderer.c
    vmr7.c
    vmr7_presenter.c
    vmr9.c
    window.c)

list(APPEND PCH_SKIP_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/proxy.dlldata.c
    ${CMAKE_CURRENT_BINARY_DIR}/quartz_strmif_p.c)

# quartz_strmif.idl carries "#pragma makedep proxy" and "#pragma makedep
# register"; control_tlb.idl carries "#pragma makedep regtypelib".
add_typelib(control_tlb.idl)
add_rpcproxy_files(quartz_strmif.idl)

add_library(quartz MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    quartz.rc
    ${CMAKE_CURRENT_BINARY_DIR}/quartz.def)

# quartz_strmif.idl also carries "#pragma makedep register": it declares every
# coclass this DLL exposes, CLSID_FilterMapper2 among them. Without this the
# only registration data in the module is quartz.rgs (media types), so
# DllRegisterServer registers its resources and then cannot CoCreateInstance
# its own filter mapper. control_tlb.idl needs its regtypelib script too.
# Both must come after add_library(): the helper appends to the target.
add_idl_reg_scripts(quartz registry quartz_strmif.idl)
add_idl_reg_scripts(quartz regtypelib control_tlb.idl)

# quartz.rc pulls in quartz.rgs and the compiled typelib.
set_source_files_properties(quartz.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/control_tlb.tlb;${CMAKE_CURRENT_SOURCE_DIR}/quartz.rgs")

set_module_type(quartz win32dll)
# main.c defines DllRegisterServer/DllUnregisterServer itself; only the
# DllCanUnloadNow helper is needed. oldnames covers the POSIX aliases.
target_link_libraries(quartz wine wine_dll_canunload strmbase strmiids uuid dxguid oldnames ${PSEH_LIB})
# ddraw is delay-loaded upstream: only the VMR7 path reaches for it.
add_delay_importlibs(quartz ddraw)
# kernel32_vista for SetThreadDescription: filesource.c, filtergraph.c and
# systemclock.c name their worker threads, and the plain kernel32 importlib
# is trimmed to DLL_EXPORT_VERSION and does not carry it.
add_importlibs(quartz
    dsound msacm32 msvfw32 ole32 oleaut32 rpcrt4 user32 gdi32 advapi32 winmm
    kernel32_vista msvcrt kernel32 ntdll)
add_dependencies(quartz wineheaders dxsdk stdole2)
add_cd_file(TARGET quartz DESTINATION reactos/system32 FOR all)
set_wine_module(quartz)
