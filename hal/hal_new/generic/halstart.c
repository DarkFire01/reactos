/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

BOOLEAN HalpPciLockSettings;
const USHORT HalpBuildType = HAL_BUILD_TYPE;

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
HalpGetParameters(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PCHAR CommandLine;

    /* Make sure we have a loader block and command line */
    if ((LoaderBlock) && (LoaderBlock->LoadOptions))
    {
        /* Read the command line */
        CommandLine = LoaderBlock->LoadOptions;

        /* Check if PCI is locked */
        if (strstr(CommandLine, "PCILOCK")) HalpPciLockSettings = TRUE;

        /* Check for initial breakpoint */
        if (strstr(CommandLine, "BREAK")) DbgBreakPoint();
    }
}

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
HalInitializeProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Hal specific initialization for this cpu */
    HalpInitProcessor(ProcessorNumber, LoaderBlock);

    /* Set default stall count */
    KeGetPcr()->StallScaleFactor = INITIAL_STALL_COUNT;

    /* Update the interrupt affinity and processor mask */
    InterlockedBitTestAndSetAffinity(&HalpActiveProcessors, ProcessorNumber);
    InterlockedBitTestAndSetAffinity(&HalpDefaultInterruptAffinity, ProcessorNumber);

    if (ProcessorNumber == 0)
    {
        /* Register routines for KDCOM */
        HalpRegisterKdSupportFunctions();
    }
}

VOID
NTAPI
HalpSanityCheckNTOSCore()
{
    PKPRCB Prcb = KeGetCurrentPrcb();

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

}

/*
 * @implemented
 */
CODE_SEG("INIT")
BOOLEAN
NTAPI
HalInitSystem(IN ULONG BootPhase,
              IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Check the boot phase */
    if (BootPhase == 0)
    {
        HalpSanityCheckNTOSCore();
    
        /* Get command-line parameters */
        HalpGetParameters(LoaderBlock);

        /* Initialize Enumerators (ACPI) */
        HalpEnumSetupPhase0(LoaderBlock);

        /* Initialize architecture specific logic */
        HalpArchInitializePhase0(LoaderBlock);

        /* Fill out the dispatch tables */
        HalQuerySystemInformation = HaliQuerySystemInformation;
        HalSetSystemInformation = HaliSetSystemInformation;
        HalInitPnpDriver = HaliInitPnpDriver;
        HalGetDmaAdapter = HalpGetDmaAdapter;

        HalGetInterruptTranslator = NULL;  // FIXME: TODO
        HalResetDisplay = HalpBiosDisplayReset;
        HalHaltSystem = HaliHaltSystem;

        /* Setup busy waiting */
        HalpCalibrateStallExecution();

        /* Initialize the clock */
        HalpInitializeClock();

        /*
         * We could be rebooting with a pending profile interrupt,
         * so clear it here before interrupts are enabled
         */
        HalStopProfileInterrupt(ProfileTime);

        HalpInitDma(LoaderBlock);

        /* Do some HAL-specific initialization */
        HalpInitPhase0(LoaderBlock);

    }
    else if (BootPhase == 1)
    {
        /* Initialize bus handlers */
        HalpInitBusHandlers();

        /* Do some HAL-specific initialization */
        HalpInitPhase1();

        /* Initialize architecture specific logic */
        HalpArchInitializePhase1(LoaderBlock);
    }

    /* All done, return */
    return TRUE;
}
