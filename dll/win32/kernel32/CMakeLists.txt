

add_definitions(-D_KERNEL32_)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys)
spec2def(kernel32.dll kernel32.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    client/atom.c
    client/atomansi.c
    client/compname.c
    client/console.c
    client/dep.c
    client/dllmain.c
    client/dosdev.c
    client/except.c
    client/firmware.c
    client/job.c
    client/lfile.c
    client/lstring.c
    client/pathansi.c
    client/power.c
    client/procansi.c
    client/synchansi.c
    client/sysinfo.c
    client/sysinfoansi.c
    client/timerqueue.c
    client/toolhelp.c
    client/utils.c
    client/vdm.c
    client/version.c
    client/versionansi.c
    client/file/copyansi.c
    client/file/create.c
    client/file/diransi.c
    client/file/io.c
    client/file/iocompl.c
    client/file/mailslot.c
    client/file/moveansi.c
    client/file/mntpointansi.c
    client/file/volumeansi.c
    client/shared_utils.c
    client/synch.c
    wine/actxctx.c
    wine/comm.c
    wine/localeansi.c
    wine/lzexpand.c
    wine/memory.c
    wine/muldiv.c
    wine/profile.c
    wine/resource.c
    wine/tape.c
    k32.h)

if(MSVC)
    # Disable warning C4146: unary minus operator applied to unsigned type, result still unsigned
    set_source_files_properties(wine/resource.c PROPERTIES COMPILE_OPTIONS "/wd4146")
endif()

add_library(kernel32 MODULE
    ${SOURCE}
    kernel32.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kernel32_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernel32.def)

add_library(kernel32_static ${SOURCE})
target_link_libraries(kernel32_static ${PSEH_LIB})
add_dependencies(kernel32_static psdk)

set_module_type(kernel32 win32dll ENTRYPOINT DllMain 12)
#############################################
## HACK FOR MSVC COMPILATION WITH win32dll ##
set_subsystem(kernel32 console)
################# END  HACK #################

target_link_libraries(kernel32 psapi_static win32_registry wine chkstk ${PSEH_LIB})
if(DLL_EXPORT_VERSION LESS 0x601)
add_importlibs(kernel32 ntdll_vista)
endif()
add_importlibs(kernel32 kernelbase ntdll)
add_pch(kernel32 k32.h SOURCE)
add_dependencies(kernel32 psdk errcodes asm)
add_cd_file(TARGET kernel32 DESTINATION reactos/system32 FOR all)
