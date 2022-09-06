include_directories(BEFORE include)

include_directories(BEFORE
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include
    ${REACTOS_SOURCE_DIR}/boot/freeldr/uefi/include
    ${REACTOS_SOURCE_DIR}/boot/freeldr/uefi/include/efi)

include_directories(
    ${REACTOS_SOURCE_DIR}/ntoskrnl/include
    ${REACTOS_SOURCE_DIR}/sdk/lib/cmlib
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/uefi)

add_definitions(-DUEFIBOOT -D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

list(APPEND FREELDR_BOOTLIB_SOURCE
    ../freeldr/lib/arcsupp.c
    ../freeldr/lib/debug.c
    ../freeldr/lib/peloader.c
    ../freeldr/lib/cache/blocklist.c
    ../freeldr/lib/cache/cache.c
    ../freeldr/lib/comm/rs232.c## add KD support
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
    ${REACTOS_SOURCE_DIR}/ntoskrnl/config/cmboot.c
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

list(APPEND FREELDR_BOOTMGR_SOURCE
    ../freeldr/include/freeldr.h
    ../freeldr/cmdline.c
    ../freeldr/custom.c
    #../freeldr/linuxboot.c
    ../freeldr/miscboot.c
    ../freeldr/options.c
    ../freeldr/oslist.c
    ../freeldr/ui/directui.c
    #../freeldr/ui/gui.c
    ../freeldr/ui/minitui.c
    ../freeldr/ui/noui.c
    ../freeldr/ui/tui.c
    ../freeldr/ui/tuimenu.c
    ../freeldr/ui/ui.c
    ../freeldr/ui/video.c)

list(APPEND FREELDR_ARC_SOURCE
    ../freeldr/lib/fs/pxe.c
    stubs.c
    utils.c
    uefiacpi.c
    uefisetup.c
    uefivid.c)

list(APPEND FREELDR_BASE_ASM_SOURCE)

if(ARCH STREQUAL "i386")
    list(APPEND FREELDR_BASE_ASM_SOURCE)
            #../freeldr/arch/i386/multiboot.S)

    list(APPEND FREELDR_COMMON_ASM_SOURCE
        ../freeldr/arch/i386/drvmap.S
        #arch/i386/entry.S #We don't use bootsectors for UEFI
        ../freeldr/arch/i386/int386.S
        ../freeldr/arch/i386/pnpbios.S
        ../freeldr/arch/i386/i386trap.S
        ../freeldr/arch/i386/linux.S)

    list(APPEND FREELDR_NTLDR_SOURCE
        ../freeldr/ntldr/arch/i386/winldr.c
        ../freeldr/ntldr/headless.c)
elseif(ARCH STREQUAL "amd64")
    list(APPEND FREELDR_COMMON_ASM_SOURCE
        arch/amd64/misc.S
        #arch/amd64/entry.S #We don't use bootsectors for UEFI
        ../freeldr/arch/amd64/int386.S
        ../freeldr/arch/amd64/pnpbios.S
        ../freeldr/arch/amd64/linux.S)

    list(APPEND FREELDR_NTLDR_SOURCE
            ../freeldr/ntldr/arch/amd64/winldr.c)
elseif(ARCH STREQUAL "arm")
        list(APPEND FREELDR_NTLDR_SOURCE
            ../freeldr/ntldr/arch/arm/winldr.c)
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

add_pch(uefifreeldr_common include/uefildr.h PCH_SOURCE)
add_dependencies(uefifreeldr_common bugcodes asm xdk)

## GCC builds need this extra thing for some reason...
if(ARCH STREQUAL "i386" AND NOT MSVC)
    target_link_libraries(uefifreeldr_common mini_hal)
endif()


if(MSVC)
    # Explicitly use string pooling
    add_compile_options("/GF")
endif()

spec2def(uefildr.exe ../freeldr/freeldr.spec)

list(APPEND UEFILDR_BASE_SOURCE
    include/uefildr.h
    uefildr.c
   ../freeldr/bootmgr.c
   ../freeldr/ntldr/setupldr.c
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
    target_link_options(uefildr PRIVATE "/MERGE:.rtc=.text")
endif()

add_dependencies(uefildr xdk)
