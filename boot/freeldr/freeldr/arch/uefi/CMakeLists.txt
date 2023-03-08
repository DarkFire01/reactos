
include_directories(BEFORE
    ${REACTOS_SOURCE_DIR}/boot/environ/include/efi
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/arch/uefi)

include_directories(
    ${REACTOS_SOURCE_DIR}/ntoskrnl/include
    ${REACTOS_SOURCE_DIR}/sdk/lib/cmlib
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf)

add_definitions(-DUEFIBOOT -D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

list(APPEND FREELDR_BOOTLIB_SOURCE
     ../../lib/arcsupp.c
     ../../lib/debug.c
     ../../lib/peloader.c
     ../../lib/cache/blocklist.c
     ../../lib/cache/cache.c
     ../../lib/comm/rs232.c
     ../../lib/fs/btrfs.c
     ../../lib/fs/ext2.c
     ../../lib/fs/fat.c
     ../../lib/fs/fs.c
     ../../lib/fs/iso.c
     ../../lib/fs/ntfs.c
     ../../lib/inifile/ini_init.c
     ../../lib/inifile/inifile.c
     ../../lib/inifile/parse.c
     ../../lib/mm/meminit.c
     ../../lib/mm/mm.c
     ../../lib/mm/heap.c)

list(APPEND FREELDR_NTLDR_SOURCE
     ${REACTOS_SOURCE_DIR}/ntoskrnl/config/cmboot.c
     ../../ntldr/conversion.c
     ../../ntldr/registry.c
     ../../ntldr/winldr.c
     ../../ntldr/wlmemory.c
     ../../ntldr/wlregistry.c)

list(APPEND FREELDR_ARC_SOURCE
     ../drivers/hwide.c
     ../arcemul.c
     ../archwsup.c
     ../../arcname.c
     ../../disk/disk.c
     ../../disk/partition.c
     ../../disk/ramdisk.c)

list(APPEND FREELDR_BOOTMGR_SOURCE
     ../../include/freeldr.h
     ../../cmdline.c
     ../../custom.c
     ../../linuxboot.c
     ../../miscboot.c
     ../../options.c
     ../../oslist.c
     ../../ui/directui.c
     ../../ui/minitui.c
     ../../ui/noui.c
     ../../ui/tui.c
     ../../ui/tuimenu.c
     ../../ui/ui.c
     ../../ui/video.c)

list(APPEND FREELDR_ARC_SOURCE
     ../../lib/fs/pxe.c
     stubs.c
     uefiutil.c
     ueficon.c
     uefisetup.c
     uefidisk.c
     uefimem.c
     uefivid.c)

if(ARCH STREQUAL "i386")
    list(APPEND FREELDR_BOOTMGR_SOURCE
         ../i386/i386bug.c)
    list(APPEND FREELDR_BASE_ASM_SOURCE
         ../i386/multiboot.S)

    list(APPEND FREELDR_COMMON_ASM_SOURCE
         ../i386/drvmap.S
         ../i386/int386.S
         ../i386/pnpbios.S
         ../i386/i386trap.S
         ../i386/linux.S)

    list(APPEND FREELDR_NTLDR_SOURCE
        ../../ntldr/arch/i386/winldr.c
        ../../ntldr/headless.c)
elseif(ARCH STREQUAL "amd64")
    list(APPEND FREELDR_BOOTMGR_SOURCE
         ../i386/i386bug.c)
    list(APPEND FREELDR_COMMON_ASM_SOURCE
         ../amd64/int386.S
         ../amd64/misc.S
         ../amd64/linux.S
         ../amd64/pnpbios.S)

    list(APPEND FREELDR_NTLDR_SOURCE
         ../../ntldr/arch/amd64/winldr.c)
elseif(ARCH STREQUAL "arm")
    list(APPEND FREELDR_NTLDR_SOURCE
         ../../ntldr/arch/arm/winldr.c)
elseif(ARCH STREQUAL "arm64")
    #TBD
else()
    #TBD
endif()

add_asm_files(uefifreeldr_common_asm ${FREELDR_COMMON_ASM_SOURCE})

add_library(uefifreeldr_common
    ${uefifreeldr_common_asm}
    ${FREELDR_MISC_SOURCE}
    ${FREELDR_BOOTLIB_SOURCE}
    ${FREELDR_ARC_SOURCE}
    ${FREELDR_NTLDR_SOURCE}
    ${FREELDR_BOOTMGR_SOURCE})

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Prevent using SSE (not support in freeldr)
    target_compile_options(uefifreeldr_common PUBLIC -mno-sse)
endif()

set(PCH_SOURCE
    ${FREELDR_BOOTLIB_SOURCE}
    ${FREELDR_ARC_SOURCE}
    ${FREELDR_NTLDR_SOURCE}
    ${FREELDR_BOOTMGR_SOURCE})

add_pch(uefifreeldr_common ../../include/arch/uefi/uefildr.h PCH_SOURCE)
add_dependencies(uefifreeldr_common bugcodes asm xdk)

## GCC builds need this extra thing for some reason...
if(ARCH STREQUAL "i386" AND NOT MSVC)
    target_link_libraries(uefifreeldr_common mini_hal)
endif()

if(MSVC)
    # Explicitly use string pooling
    add_compile_options("/GF")
endif()

spec2def(uefildr.exe ../../freeldr.spec)

list(APPEND UEFILDR_BASE_SOURCE
     ../../include/arch/uefi/uefildr.h
     uefildr.c
     ../vgafont.c
     ../../bootmgr.c
     ../../freeldr.c
     ../../ntldr/setupldr.c
     ../../ntldr/inffile.c
     ../../ntldr/ntldropts.c
     ../../lib/rtl/libsupp.c)

if(ARCH STREQUAL "i386")
    # Must be included together with disk/scsiport.c
    list(APPEND UEFILDR_BASE_SOURCE
        ${CMAKE_CURRENT_BINARY_DIR}/uefildr.def)
endif()

add_executable(uefildr ${UEFILDR_BASE_SOURCE})

if(MSVC)
    target_link_options(uefildr PRIVATE /DYNAMICBASE:NO /NXCOMPAT:NO /ignore:4078 /ignore:4254 /DRIVER)
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

target_link_libraries(uefildr uefifreeldr_common cportlib blcmlib blrtl libcntpr)

# dynamic analysis switches
if(STACK_PROTECTOR)
    target_sources(uefildr PRIVATE $<TARGET_OBJECTS:gcc_ssp_nt>)
endif()

if(RUNTIME_CHECKS)
    target_link_libraries(uefildr runtmchk)
endif()

add_dependencies(uefildr xdk)
