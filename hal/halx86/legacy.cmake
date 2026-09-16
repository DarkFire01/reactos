include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/arbiter)

list(APPEND HAL_LEGACY_ASM_SOURCE
    legacy/pir/toshsmi.S)

list(APPEND HAL_LEGACY_SOURCE
    legacy/bus/bushndlr.c
    legacy/bus/cmosbus.c
    legacy/bus/isabus.c
    legacy/bus/pcibus.c
    legacy/pir/aliirq.c
    legacy/pir/atiirq.c
    legacy/pir/compaqirq.c
    legacy/pir/cyrixirq.c
    legacy/pir/intelirq.c
    legacy/pir/legacypcirqarb.c
    legacy/pir/nsirq.c
    legacy/pir/optiirq.c
    legacy/pir/pirroute.c
    legacy/pir/sisirq.c
    legacy/pir/toshirq.c
    legacy/pir/vesuvirq.c
    legacy/pir/viairq.c
    legacy/pir/vlsiirq.c
    legacy/irqtrans.c
    ${CMAKE_CURRENT_BINARY_DIR}/pci_classes.c
    ${CMAKE_CURRENT_BINARY_DIR}/pci_vendors.c
    legacy/bus/sysbus.c
    legacy/bussupp.c
    legacy/halpnpdd.c
    legacy/halpcat.c
    smp/mps/mps.c)

add_asm_files(lib_hal_legacy_asm ${HAL_LEGACY_ASM_SOURCE})
add_library(lib_hal_legacy OBJECT ${HAL_LEGACY_SOURCE} ${lib_hal_legacy_asm})
#add_pch(lib_hal_legacy include/hal.h)
add_dependencies(lib_hal_legacy bugcodes xdk asm)
