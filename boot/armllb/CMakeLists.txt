if(MSVC)
    # Explicitly use string pooling
    add_compile_options("/GF")
endif()

include_directories(BEFORE include)
include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/include)
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/cmlib)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/elf)

add_definitions(-D_NTHAL_ -D_BLDR_ -D_NTSYSTEM_)

list(APPEND ARMLLB_ASM_SOURCE
    boot.S)

list(APPEND ARMLLB_GENERIC_SOURCE
    main.c
    crtsupp.c
    envir.c
    fw.c
    os/loader.c
    hw/keyboard.c
    hw/matrix.c
    hw/serial.c
    hw/video.c
    hw/time.c)

list(APPEND ARMLLB_OMAP3_BEAGLE_SOURCE
    hw/versatile/hwinfo.c
    hw/versatile/hwinit.c
    hw/versatile/hwclcd.c
    hw/versatile/hwuart.c
    hw/versatile/hwkmi.c)

add_asm_files(armllb_asm ${ARMLLB_ASM_SOURCE})
#add_library(armllb ${ARMLLB_OMAP3_BEAGLE_SOURCE} ${ARMLLB_GENERIC_SOURCE} ${armllb_asm})
add_library(armllb ${armllb_asm})
target_link_libraries(armllb rtl cportlib)
add_dependencies(armllb bugcodes bootmsg xdk asm)

