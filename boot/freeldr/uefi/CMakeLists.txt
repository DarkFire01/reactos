

if(MSVC)
# Explicitly use string pooling
add_compile_options("/GF")
endif()

spec2def(freeldr_pe.exe freeldr.spec)

include_directories(BEFORE include)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/include)
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cmlib)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf)

add_definitions(-D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

list(APPEND FREELDR_BOOTLIB_SOURCE
        ../freeldr/lib/arcsupp.c
        ../freeldr/lib/debug.c
        ../freeldr/lib/peloader.c
        ../freeldr/lib/cache/blocklist.c
        ../freeldr/lib/cache/cache.c
        ../freeldr/lib/comm/rs232.c
        ../freeldr/## add KD support
        ../freeldr/lib/fs/btrfs.c
        ../freeldr/lib/fs/ext2.c
        ../freeldr/lib/fs/fat.c
        ../freeldr/lib/fs/fs.c
        ../freeldr/lib/fs/iso.c
        ../freeldr/lib/fs/ntfs.c
        ../freeldr/lib/inifile/ini_init.c
        ../freeldr/lib/inifile/inifile.c
        ../freeldr/lib/inifile/parse.c
        ../freeldr/lib/mm/meminit.c
        ../freeldr/lib/mm/mm.c
        ../freeldr/lib/mm/heap.c)

list(APPEND FREELDR_NTLDR_SOURCE
../freeldr/ntldr/conversion.c
../freeldr/ntldr/registry.c
../freeldr/ntldr/winldr.c
../freeldr/ntldr/wlmemory.c
../freeldr/ntldr/wlregistry.c)

list(APPEND FREELDR_ARC_SOURCE
../freeldr/arcname.c
../freeldr/arch/drivers/hwide.c
../freeldr/arch/arcemul.c
../freeldr/arch/archwsup.c
../freeldr/disk/disk.c
../freeldr/disk/partition.c
../freeldr/disk/ramdisk.c)
#disk/scsiport.c

list(APPEND FREELDR_BOOTMGR_SOURCE
../freeldr/include/freeldr.h
../freeldr/cmdline.c
../freeldr/custom.c
../freeldr/linuxboot.c
../freeldr/miscboot.c
../freeldr/options.c
../freeldr/oslist.c
../freeldr/ui/directui.c
../freeldr/# ui/gui.c
../freeldr/ui/minitui.c
../freeldr/ui/noui.c
../freeldr/ui/tui.c
../freeldr/ui/tuimenu.c
../freeldr/ui/ui.c
../freeldr/ui/video.c)


if(ARCH STREQUAL "i386")


list(APPEND FREELDR_NTLDR_SOURCE
    ../freeldr/ntldr/arch/i386/winldr.c
    ../freeldr/ntldr/headless.c)

list(APPEND FREELDR_ARC_SOURCE
    ../freeldr/lib/fs/pxe.c
    ../freeldr/arch/i386/halstub.c
    ../freeldr/arch/i386/ntoskrnl.c
    ../freeldr/arch/i386/drivemap.c
    ../freeldr/arch/i386/hwacpi.c
    ../freeldr/arch/i386/hwapm.c
    ../freeldr/arch/i386/hwdisk.c
    ../freeldr/arch/i386/hwpci.c
    ../freeldr/arch/i386/i386bug.c
    ../freeldr/arch/i386/i386idt.c
    ../freeldr/disk/scsiport.c)

elseif(ARCH STREQUAL "amd64")

list(APPEND FREELDR_NTLDR_SOURCE
../freeldr/ntldr/arch/amd64/winldr.c)

list(APPEND FREELDR_ARC_SOURCE
    ../freeldr/lib/fs/pxe.c
    ../freeldr/arch/i386/ntoskrnl.c
    ../freeldr/arch/i386/drivemap.c
    ../freeldr/arch/i386/hwacpi.c
    ../freeldr/arch/i386/hwapm.c
    ../freeldr/arch/i386/hwdisk.c
    ../freeldr/arch/i386/hwpci.c
    ../freeldr/arch/i386/i386bug.c
    ../freeldr/arch/i386/pc/pcbeep.c
    ../freeldr/arch/i386/pc/pccons.c
    ../freeldr/arch/i386/pc/pcdisk.c
    ../freeldr/arch/i386/pc/pchw.c
    ../freeldr/arch/i386/pc/pcmem.c
    ../freeldr/arch/i386/pc/pcrtc.c
    ../freeldr/arch/i386/pc/pcvesa.c
    ../freeldr/arch/i386/pc/pcvideo.c)

elseif(ARCH STREQUAL "arm")
list(APPEND FREELDR_COMMON_UEFI_ASM_SOURCE)

list(APPEND FREELDR_NTLDR_SOURCE
    ../freeldr/ntldr/arch/arm/winldr.c)

elseif(ARCH STREQUAL "arm64")

list(APPEND FREELDR_NTLDR_SOURCE
    ../freeldr/ntldr/arch/arm64/winldr.c)
else()
list(APPEND FREELDR_NTLDR_SOURCE
    ../freeldr/ntldr/arch/arm64/winldr.c)
#TBD
endif()

#add_asm_files(freeldr_common_uefi_asm ${FREELDR_COMMON_UEFI_ASM_SOURCE})

add_library(freeldr_common_uefi
#${freeldr_common_uefi_asm}
${FREELDR_BOOTLIB_SOURCE}
${FREELDR_ARC_SOURCE}
${FREELDR_NTLDR_SOURCE}
${FREELDR_BOOTMGR_SOURCE})

if(USE_CLANG_CL)
# We need to reduce the binary size
target_compile_options(freeldr_common_uefi PRIVATE "/Os")
endif()
if(NOT MSVC AND ARCH STREQUAL "i386" AND SARCH STREQUAL "xbox")
# Prevent a warning when doing a memcmp with address 0
set_source_files_properties(arch/i386/xbox/xboxmem.c PROPERTIES COMPILE_FLAGS "-Wno-nonnull")
endif()
if(ARCH STREQUAL "amd64" AND NOT MSVC)
# Prevent using SSE (not support in freeldr)
target_compile_options(freeldr_common_uefi PUBLIC -mno-sse)
endif()

set(PCH_SOURCE
${FREELDR_BOOTLIB_SOURCE}
${FREELDR_ARC_SOURCE}
${FREELDR_NTLDR_SOURCE}
${FREELDR_BOOTMGR_SOURCE})

add_pch(freeldr_common_uefi include/freeldr.h PCH_SOURCE)
add_dependencies(freeldr_common_uefi bugcodes xdk)

## GCC builds need this extra thing for some reason...
if(ARCH STREQUAL "i386" AND NOT MSVC)
target_link_libraries(freeldr_common_uefi mini_hal)
endif()


if(MSVC)
    # Explicitly use string pooling
    add_compile_options("/GF")
endif()

spec2def(uefildr.exe freeldr.spec)

include_directories(BEFORE include)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/include)
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cmlib)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/ntldr)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/fs)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/arch)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/arch/pc)
include_directories(${REACTOS_SOURCE_DIR}/boot/freeldr/uefi/include/efi)
add_definitions(-D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)




list(APPEND UEFILDR_BASE_SOURCE
    include/uefildr.h
    freeldr.c
    misc.c
    utils.c
    stubs.c
    uefisetup.c
    ueficon.c
    uefivid.c
    uefimem.c
    uefidisk.c
    uefiux.c
    uefidebug.c
   #${freeldr_base_asm}
   ../freeldr/bootmgr.c# This file is compiled with custom definitions
   ../freeldr/ntldr/setupldr.c ## Strangely enough this file is needed in GCC builds
   # even if ${FREELDR_NTLDR_SOURCE} is not added,
   # otherwise we get linking errors with Rtl**Bitmap** APIs.
   # Do not happen on MSVC builds however...
   ../freeldr/ntldr/inffile.c
   ../freeldr/ntldr/ntldropts.c
   ../freeldr/lib/rtl/libsupp.c)

if(ARCH STREQUAL "i386")
    # Must be included together with disk/scsiport.c
    list(APPEND UEFILDR_BASE_SOURCE
        ${CMAKE_CURRENT_BINARY_DIR}/uefildr.def)
endif()

add_executable(uefildr ${UEFILDR_BASE_SOURCE})

if(MSVC)
        target_link_options(uefildr PRIVATE /ignore:4078 /ignore:4254 /DRIVER)
    # We don't need hotpatching
    remove_target_compile_option(uefildr "/hotpatch")
else()
    target_link_options(uefildr PRIVATE -Wl,--exclude-all-symbols,--file-alignment,0x200,--section-alignment,0x200)
    # Strip everything, including rossym data
    add_custom_command(TARGET uefildr
                    POST_BUILD
                    COMMAND ${CMAKE_STRIP} --remove-section=.rossym $<TARGET_FILE:uefildr>
                    COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:uefildr>)
endif()

if(ARCH STREQUAL "amd64")
#set_image_base(uefildr 0x10000)
endif()
if(MSVC)
    set_subsystem(uefildr EFI_APPLICATION)
else()
    set_subsystem(uefildr 10)
endif()

set_target_properties(uefildr PROPERTIES SUFFIX ".efi")

set_entrypoint(uefildr EfiEntry)

if(ARCH STREQUAL "i386")
    target_link_libraries(uefildr mini_hal)
endif()

target_link_libraries(uefildr freeldr_common cportlib cmlib rtl libcntpr)

# dynamic analysis switches
if(STACK_PROTECTOR)
    target_sources(uefildr PRIVATE $<TARGET_OBJECTS:gcc_ssp_nt>)
endif()

if(RUNTIME_CHECKS)
    target_link_libraries(uefildr runtmchk)
    target_link_options(uefildr PRIVATE "/MERGE:.rtc=.text")
endif()

add_dependencies(uefildr xdk)
#add_custom_target(freeldr ALL DEPENDS uefildr)
