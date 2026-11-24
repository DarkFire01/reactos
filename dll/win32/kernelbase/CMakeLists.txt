
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys)
add_subdirectory(wine)

if(DLL_EXPORT_VERSION GREATER_EQUAL 0x601)
set(KERNELBASE_NAME kernelbase)
else()
set(KERNELBASE_NAME kernelbase_ros)
endif()

add_definitions(
    -D_KERNEL32_
    -D_KERNELBASE_)

spec2def(${KERNELBASE_NAME}.dll kernelbase.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    client/shared_utils.c
    client/file/fileutils.c
    client/actctx.c
    client/appcache.c
    client/atom.c
    client/compname.c
    client/debugger.c
    client/dosdev.c
    client/dllmain.c
    client/environ.c
    client/except.c
    client/fiber.c
    client/firmware.c
    client/GetFinalPathNameByHandle.c
    client/handle.c
    client/heapmem.c
    client/japanese.c
    client/job.c
    client/loader.c
    client/lstring.c
    client/nls.c
    client/path.c
    client/perfcnt.c
    client/proc.c
    client/resntfy.c
    client/synch.c
    client/sysinfo.c
    client/time.c
    client/timerqueue.c
    client/utils.c
    client/thread.c
    client/ThreadDescription.c
    client/vdm.c
    client/virtmem.c
    client/vista.c
    client/console/alias.c
    client/console/console.c
    client/console/history.c
    client/console/init.c
    client/console/readwrite.c
    client/console/vista.c
    client/file/backup.c
    client/file/cnotify.c
    client/file/copy.c
    client/file/create.c
    client/file/delete.c
    client/file/deviceio.c
    client/file/dir.c
    client/file/disk.c
    client/file/fileinfo.c
    client/file/fileinfoansi.c
    client/file/filemap.c
    client/file/filename.c
    client/file/find.c
    client/file/hardlink.c
    client/file/iocompl.c
    client/file/lock.c
    client/file/move.c
    client/file/mntpoint.c
    client/file/npipe.c
    client/file/rw.c
    client/file/tape.c
    client/file/volume.c
)

if(ARCH STREQUAL "i386")
    list(APPEND ASM_SOURCE
        client/i386/fiber.S
        client/i386/thread.S)
elseif(ARCH STREQUAL "amd64")
    list(APPEND ASM_SOURCE
        client/amd64/fiber.S)
elseif(ARCH STREQUAL "arm")
    list(APPEND ASM_SOURCE
        client/arm/fiber.S
        client/arm/thread.S)
endif()

add_asm_files(kernelbase_asm ${ASM_SOURCE})
add_library(${KERNELBASE_NAME} MODULE
    ${CMAKE_CURRENT_BINARY_DIR}/${KERNELBASE_NAME}_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/${KERNELBASE_NAME}.def
    kernelbase.rc
    ${SOURCE}
    ${kernelbase_asm})

set_module_type(${KERNELBASE_NAME} win32dll ENTRYPOINT DllMain 12)
target_link_libraries(${KERNELBASE_NAME} win32_registry psapi_static kernelbase_static wine chkstk)
add_importlibs(${KERNELBASE_NAME} ntdll ntdll_vista)
add_dependencies(${KERNELBASE_NAME} psdk errcodes)
add_cd_file(TARGET ${KERNELBASE_NAME} DESTINATION reactos/system32 FOR all)

if(DLL_EXPORT_VERSION LESS 0x601)
add_library(libkernelbase INTERFACE)
target_link_libraries(libkernelbase INTERFACE lib${KERNELBASE_NAME})
endif()
