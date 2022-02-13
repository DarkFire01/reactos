
include_directories(BEFORE include)
include_directories(BEFORE core/include/)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/include)
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cmlib)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)
include_directories(${REACTOS_SOURCE_DIR}/drivers/filesystems)

add_definitions(-D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

# EFI platform ID
if(ARCH STREQUAL "amd64")
    set(EFI_PLATFORM_ID "BOOTX64")
elseif(ARCH STREQUAL "i386")
    set(EFI_PLATFORM_ID "BOOTIA32")
elseif(ARCH STREQUAL "ia64")
    set(EFI_PLATFORM_ID "BOOTIA64")
elseif(ARCH STREQUAL "arm")
    set(EFI_PLATFORM_ID "BOOTARM")
elseif(ARCH STREQUAL "arm64")
    set(EFI_PLATFORM_ID "BOOTAA64")
else()
    message(FATAL_ERROR "Unknown ARCH '" ${ARCH} "', cannot generate a valid UEFI boot filename.")
endif()

list(APPEND BOOTMGR_BASE_SOURCE
        include/rosefi.h
        entry.c
        time.c
        display/gop.c
        display/eficonsolelib.c
        lib/mm/efimm.c
        debug.c
        lib/comm/rs232.c
    )

add_executable(${EFI_PLATFORM_ID} ${BOOTMGR_BASE_SOURCE})
add_pch(${EFI_PLATFORM_ID} include/rosefi.h BOOTMGR_BASE_SOURCE)
set_target_properties(${EFI_PLATFORM_ID} PROPERTIES SUFFIX ".efi")

if(MSVC)
    if(ARCH STREQUAL "arm" OR ARCH STREQUAL "arm64")
        target_link_options(${EFI_PLATFORM_ID} PRIVATE /ignore:4078 /ignore:4254 /DRIVER)
    else()
        target_link_options(${EFI_PLATFORM_ID} PRIVATE /ignore:4078 /ignore:4254 /DRIVER /DYNAMICBASE:NO /NXCOMPAT:NO /FIXED)
    endif()
else()
    target_link_options(${EFI_PLATFORM_ID} PRIVATE "-Wl,--strip-all,--exclude-all-symbols")
endif()

set_image_base(${EFI_PLATFORM_ID} 0x10000)

if(MSVC)
    set_subsystem(${EFI_PLATFORM_ID} EFI_APPLICATION)
else()
    set_subsystem(${EFI_PLATFORM_ID} 10)
endif()

set_entrypoint(${EFI_PLATFORM_ID} RefiEntry)

target_link_libraries(${EFI_PLATFORM_ID} cportlib cmlib rtl libcntpr)
add_dependencies(${EFI_PLATFORM_ID} xdk)
# dynamic analysis switches
if(STACK_PROTECTOR)
    target_sources(${EFI_PLATFORM_ID} PRIVATE $<TARGET_OBJECTS:gcc_ssp_nt>)
endif()

if(RUNTIME_CHECKS)
    target_link_libraries(${EFI_PLATFORM_ID} runtmchk)
endif()

add_cd_file(TARGET ${EFI_PLATFORM_ID} DESTINATION efi/boot/ NO_CAB FOR all)
