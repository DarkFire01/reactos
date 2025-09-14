/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/generic/halinit.c
 * PURPOSE:         HAL Entrypoint and Initialization
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

//#ifdef CONFIG_SMP // FIXME: Reenable conditional once HAL is consistently compiled for SMP mode
BOOLEAN HalpOnlyBootProcessor;
//#endif
BOOLEAN HalpPciLockSettings;

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
NTAPI
HalpTranslatorNull(
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

NTSTATUS
NTAPI
HalpTranslateResource(
    IN OUT PVOID Context OPTIONAL,
    IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    IN RESOURCE_TRANSLATION_DIRECTION Direction,
    IN ULONG AlternativesCount OPTIONAL,
    IN PIO_RESOURCE_DESCRIPTOR Alternatives[],
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;
    ULONG MinimumVector, Vector, k;
    PIO_RESOURCE_DESCRIPTOR Alternative;

    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    /* Copy common fields */
    Target->Type = Source->Type;
    Target->ShareDisposition = Source->ShareDisposition;
    Target->Flags = Source->Flags;

    if (Direction == TranslateChildToParent)
    {
        /* Translate device (child) vector to system (parent) vector */
        Target->u.Interrupt.Vector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                           0,
                                                           Source->u.Interrupt.Vector,
                                                           Source->u.Interrupt.Vector,
                                                           &Irql,
                                                           &Affinity);
        Target->u.Interrupt.Level = Irql;
        Target->u.Interrupt.Affinity = Affinity;
        return STATUS_TRANSLATION_COMPLETE;
    }
    else if (Direction == TranslateParentToChild)
    {
        /* Try to find the device vector that maps to the given system vector */
        for (k = 0; k < AlternativesCount; k++)
        {
            Alternative = Alternatives[k];

            ASSERT(Alternative->Type == CmResourceTypeInterrupt);

            MinimumVector = Alternative->u.Interrupt.MinimumVector;
            while (MinimumVector <= Alternative->u.Interrupt.MaximumVector)
            {
                Vector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                               0,
                                               MinimumVector,
                                               MinimumVector,
                                               &Irql,
                                               &Affinity);

                if (Vector == Source->u.Interrupt.Vector)
                {
                    Target->u.Interrupt.Affinity = (KAFFINITY)-1;
                    Target->u.Interrupt.Vector = MinimumVector;
                    Target->u.Interrupt.Level = MinimumVector;
                    return STATUS_SUCCESS;
                }

                MinimumVector++;
            }
        }
    }

    return STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpTranslateRequirement(
    IN OUT PVOID Context OPTIONAL,
    IN PIO_RESOURCE_DESCRIPTOR Source,
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PULONG TargetCount,
    OUT PIO_RESOURCE_DESCRIPTOR *Target)
{
    KIRQL Irql;
    KAFFINITY Affinity;

    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    PAGED_CODE();

    ASSERT(Source->Type == CmResourceTypeInterrupt);

    *Target = ExAllocatePoolWithTag(PagedPool,
                                    sizeof(IO_RESOURCE_DESCRIPTOR),
                                    'trIH');
    if (!*Target)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(*Target, sizeof(IO_RESOURCE_DESCRIPTOR));
    *TargetCount = 1;

    /* Translate minimum and maximum vectors into parent space */
    (*Target)->Type = CmResourceTypeInterrupt;
    (*Target)->u.Interrupt.MinimumVector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                                 0,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 Source->u.Interrupt.MinimumVector,
                                                                 &Irql,
                                                                 &Affinity);
    (*Target)->u.Interrupt.MaximumVector = HalGetInterruptVector((INTERFACE_TYPE)Context,
                                                                 0,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 Source->u.Interrupt.MaximumVector,
                                                                 &Irql,
                                                                 &Affinity);

    return STATUS_TRANSLATION_COMPLETE;
}
 
NTSTATUS
NTAPI
HalpGetInterruptTranslator(
    IN INTERFACE_TYPE ParentInterfaceType,
    IN ULONG ParentBusNumber,
    IN INTERFACE_TYPE BridgeInterfaceType,
    IN USHORT Size,
    IN USHORT Version,
    OUT PTRANSLATOR_INTERFACE Translator,
    OUT PULONG BridgeBusNumber)
{
    UNREFERENCED_PARAMETER(ParentInterfaceType);
    UNREFERENCED_PARAMETER(ParentBusNumber);
    UNREFERENCED_PARAMETER(BridgeBusNumber);

    PAGED_CODE();

    ASSERT(Size >= sizeof(TRANSLATOR_INTERFACE));
    ASSERT(Version == HAL_IRQ_TRANSLATOR_VERSION);

    /* Only non-internal busses are supported */
    if ((BridgeInterfaceType == Internal) || (BridgeInterfaceType >= MicroChannel))
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    Translator->Size = sizeof(TRANSLATOR_INTERFACE);
    Translator->Version = HAL_IRQ_TRANSLATOR_VERSION;
    Translator->Context = UlongToPtr((BridgeInterfaceType == InterfaceTypeUndefined) ? Isa : BridgeInterfaceType);
    Translator->InterfaceReference = HalpTranslatorNull;
    Translator->InterfaceDereference = HalpTranslatorNull;
    Translator->TranslateResources = (PTRANSLATE_RESOURCE_HANDLER)HalpTranslateResource;
    Translator->TranslateResourceRequirements = HalpTranslateRequirement;

    return STATUS_SUCCESS;
}

static
CODE_SEG("INIT")
VOID
HalpGetParameters(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Make sure we have a loader block and command line */
    if (LoaderBlock && LoaderBlock->LoadOptions)
    {
        /* Read the command line */
        PCSTR CommandLine = LoaderBlock->LoadOptions;

//#ifdef CONFIG_SMP // FIXME: Reenable conditional once HAL is consistently compiled for SMP mode
        /* Check whether we should only start one CPU */
        if (strstr(CommandLine, "ONECPU"))
            HalpOnlyBootProcessor = TRUE;
//#endif

        /* Check if PCI is locked */
        if (strstr(CommandLine, "PCILOCK"))
            HalpPciLockSettings = TRUE;

        /* Check for initial breakpoint */
        if (strstr(CommandLine, "BREAK"))
            DbgBreakPoint();
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

/*
 * @implemented
 */
CODE_SEG("INIT")
BOOLEAN
NTAPI
HalInitSystem(
    _In_ ULONG BootPhase,
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    NTSTATUS Status;

    /* Check the boot phase */
    if (BootPhase == 0)
    {
        /* Save bus type */
        HalpBusType = LoaderBlock->u.I386.MachineType & 0xFF;

        /* Get command-line parameters */
        HalpGetParameters(LoaderBlock);

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

        /* Initialize ACPI */
        Status = HalpSetupAcpiPhase0(LoaderBlock);
        if (!NT_SUCCESS(Status))
        {
            KeBugCheckEx(ACPI_BIOS_ERROR, Status, 0, 0, 0);
        }

        /* Initialize the PICs */
        HalpInitializePICs(TRUE);

        /* Initialize CMOS lock */
        KeInitializeSpinLock(&HalpSystemHardwareLock);

        /* Initialize CMOS */
        HalpInitializeCmos();

        /* Fill out the dispatch tables */
        HalQuerySystemInformation = HaliQuerySystemInformation;
        HalSetSystemInformation = HaliSetSystemInformation;
        HalInitPnpDriver = HaliInitPnpDriver;
        HalGetDmaAdapter = HalpGetDmaAdapter;

        HalGetInterruptTranslator = HalpGetInterruptTranslator;
        HalResetDisplay = HalpBiosDisplayReset;
        HalHaltSystem = HaliHaltSystem;

        /* Setup I/O space */
        HalpDefaultIoSpace.Next = HalpAddressUsageList;
        HalpAddressUsageList = &HalpDefaultIoSpace;
        if (HalpBusType == MACHINE_TYPE_EISA) {
            HalpEisaIoSpace.Next = HalpAddressUsageList;
            HalpAddressUsageList = &HalpEisaIoSpace;
        }

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

        /* Initialize Phase 0 of the x86 emulator */
        HalInitializeBios(0, LoaderBlock);
    }
    else if (BootPhase == 1)
    {
        /* Initialize bus handlers */
        HalpInitBusHandlers();

        /* Do some HAL-specific initialization */
        HalpInitPhase1();

        /* Initialize Phase 1 of the x86 emulator */
        HalInitializeBios(1, LoaderBlock);
    }

    /* All done, return */
    return TRUE;
}
