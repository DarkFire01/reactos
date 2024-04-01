
/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

#ifdef CONFIG_SMP
#define HAL_BUILD_TYPE (DBG ? PRCB_BUILD_DEBUG : 0)
#else
#define HAL_BUILD_TYPE ((DBG ? PRCB_BUILD_DEBUG : 0) | PRCB_BUILD_UNIPROCESSOR)
#endif

KAFFINITY HalpActiveProcessors;
const USHORT HalpBuildType = HAL_BUILD_TYPE;

#ifdef CONFIG_SMP
KIRQL HalpIrqlSynchLevel = IPI_LEVEL - 2;
#else
KIRQL HalpIrqlSynchLevel = DISPATCH_LEVEL;
#endif


VOID
NTAPI
HalInitializeProcessor(
    _In_ ULONG ProcessorNumber,
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    __debugbreak();
#if 0
    NTSTATUS Status;

    InterlockedBitTestAndSet((PLONG)&HalpActiveProcessors, ProcessorNumber);

    if (ProcessorNumber == 0)
    {
        Status = HalEnumDetectProcessorCount(LoaderBlock);
        if (!Status)
        {
            //TODO: hmm
        }

        /* Register routines for KDCOM */

        /* Register PCI Device Functions */
        KdSetupPciDeviceForDebugging = HalpSetupPciDeviceForDebugging;
        KdReleasePciDeviceforDebugging = HalpReleasePciDeviceForDebugging;

        /* Register ACPI stub */
        KdGetAcpiTablePhase0 = HalAcpiGetTable;
        KdCheckPowerButton = HalpCheckPowerButton;

        /* Register memory functions */
        KdMapPhysicalMemory64 = HalpMapPhysicalMemory64;
        KdUnmapVirtualAddress = HalpUnmapVirtualAddress;


    }

    HalpArchSetupProcessor(LoaderBlock);
#endif
}
