

#include <hal.h>
//#define NDEBUG
#include <debug.h>

extern const USHORT HalpBuildType;

CODE_SEG("INIT")
BOOLEAN
NTAPI
HalInitSystem(IN ULONG BootPhase,
              IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    DPRINT("HalInitSystem: Entry - Phase %X, Processor %X\n", BootPhase, Prcb->Number);
    
    if (!BootPhase) /* Phase 0 */
    {
        /* Check for PRCB version mismatch */
        if (Prcb->MajorVersion != PRCB_MAJOR_VERSION)
        {
            /* No match, bugcheck */
            KeBugCheckEx(MISMATCHED_HAL, 1, Prcb->MajorVersion, PRCB_MAJOR_VERSION, 0);
        }

        /* Checked/free HAL requires checked/free kernel */
        if (Prcb->BuildType != HalpBuildType)
        {
            /* No match, bugcheck */
            KeBugCheckEx(MISMATCHED_HAL, 2, Prcb->BuildType, HalpBuildType, 0);
        }
#if 0
        /* Fill out HalDispatchTable */
        HalQuerySystemInformation = HaliQuerySystemInformation;
        HalSetSystemInformation = HaliSetSystemInformation; // FIXME TODO: HalpSetSystemInformation

        if (HalDispatchTableVersion >= HAL_DISPATCH_VERSION)
        {
            /* Fill out HalDispatchTable */
            HalInitPnpDriver = HaliInitPnpDriver;
            HalGetDmaAdapter = HaliGetDmaAdapter;
            HalGetInterruptTranslator = HalacpiGetInterruptTranslator;
            HalInitPowerManagement = HaliInitPowerManagement;

            /* Fill out HalPrivateDispatchTable */
            HalLocateHiberRanges = HaliLocateHiberRanges;        // FIXME: TODO
            HalHaltSystem = HaliHaltSystem;
            HalResetDisplay = HalpBiosDisplayReset;
            HalAllocateMapRegisters = HalpAllocateMapRegisters;  // FIXME: TODO
        }

        HalEnumInitalizePhase0(LoaderBlock); /* ACPI - Legacy - Device Tree(?) */
        HalArchInitalizePhase0(LoaderBlock); /* ARM32, ARM64, XBOX, I386 */
#endif
    }
    else /* Phase 1 */
    {
        __debugbreak();
    }

    return TRUE;
}
