/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI power-management services offered to the ACPI bus driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The ACPI bus driver owns the AML interpreter and the ACPI event model; the
 * HAL owns the fixed hardware the driver cannot reach on its own: the sleep
 * transition itself (PM1 control), the interrupt controller state around it,
 * the RTC wake alarm, raw table access and PCI configuration space. Those
 * services are published through the table HalInitPowerManagement returns.
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "dispatch.h"
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* The ACPI driver's callbacks (SCI/GPE control) */
PACPI_DRIVER_DISPATCH_TABLE HalpAcpiDriverDispatch;

/* Which interrupt controller this HAL drives (see generic/halinit.c) */
extern ULONG HalpInterruptModel;

/* ACPI tables the HAL already collected */
extern PACPI_BIOS_MULTI_NODE HalpAcpiMultiNode;
extern FADT HalpFixedAcpiDescTable;

/* Runtime mappings handed to the driver, created on first use */
static PFACS HalpFacsMapping;
static PULONG_PTR HalpAcpiTableList;
static FAST_MUTEX HalpAcpiDispatchLock;

/* Set once a sleep transition disturbed the interrupt controller */
static BOOLEAN HalpInterruptControllerIntact = TRUE;

/* RTC wake alarm state */
static BOOLEAN HalpWakeAlarmEnabled;
static BOOLEAN HalpWakeAlarmArmed;
static TIME_FIELDS HalpWakeAlarmTime;

/* Processor rendezvous for the sleep entry */
static volatile LONG HalpSleepArrivals;

/* Sleep-state values the driver reported, kept for the ones registered later */
static HAL_ACPI_SLEEP_STATE HalpSleepStates[HAL_ACPI_SLEEP_STATE_COUNT];

/*
 * The context handed to the power manager for each registered sleep state:
 * the two SLP_TYP values and the state number, packed so the sleep handler
 * needs nothing else.
 */
#define HALP_SLEEP_CONTEXT(State, TypA, TypB) \
    (((TypA) & 0x7) | (((TypB) & 0x7) << 4) | ((State) << 8))
#define HALP_SLEEP_CONTEXT_TYPA(Context)  ((Context) & 0x7)
#define HALP_SLEEP_CONTEXT_TYPB(Context)  (((Context) >> 4) & 0x7)
#define HALP_SLEEP_CONTEXT_STATE(Context) (((Context) >> 8) & 0xF)

/* PM1 event/control register bits */
#define PM1_STS_WAK             0x8000
#define PM1_CNT_SLP_EN          0x2000
#define PM1_CNT_SLP_TYP_SHIFT   10
#define PM1_CNT_KEEP_MASK       0x0203      /* SCI_EN, BM_RLD, GBL_RLS */

/* RTC register B bit that lets the alarm raise an interrupt / wake */
#define RTC_REG_B_AIE           0x20

/* Registers 1, 3 and 5 hold the seconds, minutes and hours alarm */
#define RTC_ALARM_SECOND        0x01
#define RTC_ALARM_MINUTE        0x03
#define RTC_ALARM_HOUR          0x05

/* Minimum lead time for a wake alarm, in 100ns units (10 seconds) */
#define HALP_MIN_WAKE_LEAD      100000000LL

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Returns the port of a PM1 register block, or NULL when the block is
 * absent.
 */
static
PUSHORT
HalpPm1Port(
    _In_ ULONG BlockPort,
    _In_ ULONG Offset)
{
    if (BlockPort == 0)
    {
        return NULL;
    }

    return (PUSHORT)(ULONG_PTR)(BlockPort + Offset);
}

/**
 * @brief
 * Writes SLP_TYP and SLP_EN into the PM1 control registers, keeping the
 * enable bits that must survive the write.
 */
static
VOID
HalpWriteSleepType(
    _In_ ULONG Context)
{
    PUSHORT Control;
    USHORT Value;

    Control = HalpPm1Port(HalpFixedAcpiDescTable.pm1a_ctrl_blk_io_port, 0);
    if (Control != NULL)
    {
        Value = READ_PORT_USHORT(Control) & PM1_CNT_KEEP_MASK;
        Value |= (HALP_SLEEP_CONTEXT_TYPA(Context) << PM1_CNT_SLP_TYP_SHIFT) | PM1_CNT_SLP_EN;
        WRITE_PORT_USHORT(Control, Value);
    }

    Control = HalpPm1Port(HalpFixedAcpiDescTable.pm1b_ctrl_blk_io_port, 0);
    if (Control != NULL)
    {
        Value = READ_PORT_USHORT(Control) & PM1_CNT_KEEP_MASK;
        Value |= (HALP_SLEEP_CONTEXT_TYPB(Context) << PM1_CNT_SLP_TYP_SHIFT) | PM1_CNT_SLP_EN;
        WRITE_PORT_USHORT(Control, Value);
    }
}

/**
 * @brief
 * Clears the wake status bits so the sleep entry can wait for a fresh one.
 */
static
VOID
HalpClearWakeStatus(VOID)
{
    PUSHORT Status;

    Status = HalpPm1Port(HalpFixedAcpiDescTable.pm1a_evt_blk_io_port, 0);
    if (Status != NULL)
    {
        WRITE_PORT_USHORT(Status, PM1_STS_WAK);
    }

    Status = HalpPm1Port(HalpFixedAcpiDescTable.pm1b_evt_blk_io_port, 0);
    if (Status != NULL)
    {
        WRITE_PORT_USHORT(Status, PM1_STS_WAK);
    }
}

/**
 * @brief
 * Waits until the hardware reports the wake status in either PM1 block.
 */
static
VOID
HalpWaitForWakeStatus(VOID)
{
    PUSHORT StatusA, StatusB;

    StatusA = HalpPm1Port(HalpFixedAcpiDescTable.pm1a_evt_blk_io_port, 0);
    StatusB = HalpPm1Port(HalpFixedAcpiDescTable.pm1b_evt_blk_io_port, 0);

    for (;;)
    {
        if ((StatusA != NULL) && (READ_PORT_USHORT(StatusA) & PM1_STS_WAK))
        {
            break;
        }
        if ((StatusB != NULL) && (READ_PORT_USHORT(StatusB) & PM1_STS_WAK))
        {
            break;
        }
        YieldProcessor();
    }
}

/**
 * @brief
 * Puts the machine into a sleep state from the boot processor, with all
 * other processors already parked. S1 comes back through the wake status;
 * S5 never returns.
 */
static
NTSTATUS
HalpEnterSleepState(
    _In_ ULONG Context)
{
    PUSHORT ControlA, ControlB;
    USHORT SavedA = 0, SavedB = 0;
    ULONG State = HALP_SLEEP_CONTEXT_STATE(Context);

    ControlA = HalpPm1Port(HalpFixedAcpiDescTable.pm1a_ctrl_blk_io_port, 0);
    ControlB = HalpPm1Port(HalpFixedAcpiDescTable.pm1b_ctrl_blk_io_port, 0);
    if (ControlA == NULL)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (State == 5)
    {
        /* Soft off: the write is the last thing this machine does */
        HalpClearWakeStatus();
        HalpWriteSleepType(Context);
        for (;;)
        {
            __halt();
        }
    }

    /* Standby keeps the whole platform context; only the sleep type is
       written and undone once the wake status returns */
    SavedA = READ_PORT_USHORT(ControlA);
    if (ControlB != NULL)
    {
        SavedB = READ_PORT_USHORT(ControlB);
    }

    HalpClearWakeStatus();
    HalpWriteSleepType(Context);
    HalpWaitForWakeStatus();

    WRITE_PORT_USHORT(ControlA, SavedA);
    if (ControlB != NULL)
    {
        WRITE_PORT_USHORT(ControlB, SavedB);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * The sleep-state handler registered with the power manager. Every
 * processor arrives here; the boot processor performs the transition once
 * the others are parked, then releases them.
 */
static
NTSTATUS
NTAPI
HalpAcpiSleepHandler(
    _In_opt_ PVOID Context,
    _In_opt_ PENTER_STATE_SYSTEM_HANDLER SystemHandler,
    _In_opt_ PVOID SystemContext,
    _In_ LONG NumberProcessors,
    _In_opt_ LONG volatile *Number)
{
    ULONG SleepContext = PtrToUlong(Context);
    ULONG State = HALP_SLEEP_CONTEXT_STATE(SleepContext);
    ULONG_PTR Flags;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Number);

    /* Deeper states need firmware resume support this HAL does not have */
    if ((State != 1) && (State != 5))
    {
        return STATUS_NOT_SUPPORTED;
    }

    Flags = __readeflags();
    _disable();

    if (KeGetCurrentProcessorNumber() != 0)
    {
        /* Park until the boot processor is done */
        InterlockedIncrement(&HalpSleepArrivals);
        while (HalpSleepArrivals != 0)
        {
            YieldProcessor();
        }
    }
    else
    {
        InterlockedIncrement(&HalpSleepArrivals);
        while (HalpSleepArrivals != NumberProcessors)
        {
            YieldProcessor();
        }

        /* A system handler that takes over (a firmware resume path) means
           the hardware transition is skipped */
        if ((SystemHandler != NULL) && (SystemHandler(SystemContext) != STATUS_SUCCESS))
        {
            Status = STATUS_SUCCESS;
        }
        else
        {
            if ((HalpAcpiDriverDispatch != NULL) &&
                (HalpAcpiDriverDispatch->GpeEnableWakeEvents != NULL))
            {
                HalpAcpiDriverDispatch->GpeEnableWakeEvents(TRUE);
            }

            Status = HalpEnterSleepState(SleepContext);

            if ((HalpAcpiDriverDispatch != NULL) &&
                (HalpAcpiDriverDispatch->GpeEnableWakeEvents != NULL))
            {
                HalpAcpiDriverDispatch->GpeEnableWakeEvents(FALSE);
            }
        }

        /* Release the other processors */
        HalpSleepArrivals = 0;
    }

    __writeeflags(Flags);
    return Status;
}

/**
 * @brief
 * Registers one sleep state with the power manager.
 */
static
VOID
HalpRegisterSleepState(
    _In_ POWER_STATE_HANDLER_TYPE Type,
    _In_ ULONG State,
    _In_ PHAL_ACPI_SLEEP_STATE SleepState,
    _In_ BOOLEAN RtcWake)
{
    POWER_STATE_HANDLER Handler;
    NTSTATUS Status;

    RtlZeroMemory(&Handler, sizeof(Handler));
    Handler.Type = Type;
    Handler.RtcWake = RtcWake;
    Handler.Handler = HalpAcpiSleepHandler;
    Handler.Context = UlongToPtr(HALP_SLEEP_CONTEXT(State,
                                                    SleepState->SlpTypA,
                                                    SleepState->SlpTypB));

    Status = ZwPowerInformation(SystemPowerStateHandler,
                                &Handler,
                                sizeof(Handler),
                                NULL,
                                0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Sleep state S%lu not registered: 0x%lx\n", State, Status);
    }
}

/**
 * @brief
 * Programs (or clears) the RTC alarm registers for a wake time.
 */
static
VOID
HalpProgramRtcAlarm(
    _In_opt_ PTIME_FIELDS WakeTime)
{
    UCHAR RegB;

    HalpAcquireCmosSpinLock();

    RegB = HalpReadCmos(RTC_REGISTER_B);
    if (WakeTime == NULL)
    {
        HalpWriteCmos(RTC_REGISTER_B, RegB & ~RTC_REG_B_AIE);
    }
    else
    {
        HalpWriteCmos(RTC_ALARM_SECOND, INT_BCD(WakeTime->Second));
        HalpWriteCmos(RTC_ALARM_MINUTE, INT_BCD(WakeTime->Minute));
        HalpWriteCmos(RTC_ALARM_HOUR, INT_BCD(WakeTime->Hour));

        /* Chipsets with a day/month alarm say so in the FADT */
        if (HalpFixedAcpiDescTable.day_alarm_index != 0)
        {
            HalpWriteCmos(HalpFixedAcpiDescTable.day_alarm_index, INT_BCD(WakeTime->Day));
        }
        if (HalpFixedAcpiDescTable.month_alarm_index != 0)
        {
            HalpWriteCmos(HalpFixedAcpiDescTable.month_alarm_index, INT_BCD(WakeTime->Month));
        }

        HalpWriteCmos(RTC_REGISTER_B, RegB | RTC_REG_B_AIE);
    }

    /* Reading register C drops any alarm already pending */
    HalpReadCmos(RTC_REGISTER_C);

    HalpReleaseCmosSpinLock();
}

/**
 * @brief
 * Maps the firmware ACPI control structure the FADT points to.
 */
static
PFACS
HalpMapFacs(VOID)
{
    PHYSICAL_ADDRESS Address;
    PFACS Facs;

    if (HalpFacsMapping != NULL)
    {
        return HalpFacsMapping;
    }

    Address.QuadPart = HalpFixedAcpiDescTable.facs;
    if ((Address.QuadPart == 0) &&
        (HalpFixedAcpiDescTable.Header.Length >= FIELD_OFFSET(FADT, x_dsdt)))
    {
        Address = HalpFixedAcpiDescTable.x_firmware_ctrl;
    }
    if (Address.QuadPart == 0)
    {
        return NULL;
    }

    Facs = MmMapIoSpace(Address, sizeof(FACS), MmNonCached);
    if ((Facs != NULL) && (Facs->Signature != FACS_SIGNATURE))
    {
        DPRINT1("No FACS at %I64x\n", Address.QuadPart);
        MmUnmapIoSpace(Facs, sizeof(FACS));
        return NULL;
    }

    HalpFacsMapping = Facs;
    return Facs;
}

/**
 * @brief
 * Builds the list of every table the root system description table
 * refers to, minus the structural ones (RSDT, XSDT, FACS, SSDT). The
 * result is a count followed by that many table pointers.
 */
static
PULONG_PTR
HalpBuildAcpiTableList(VOID)
{
    PDESCRIPTION_HEADER Root, Header;
    PULONG_PTR List;
    PHYSICAL_ADDRESS Address;
    ULONG EntryCount, EntrySize, i, Count;
    PUCHAR Entries;

    if (HalpAcpiTableList != NULL)
    {
        return HalpAcpiTableList;
    }

    Root = HalAcpiGetTable(NULL, XSDT_SIGNATURE);
    EntrySize = sizeof(PHYSICAL_ADDRESS);
    if (Root == NULL)
    {
        Root = HalAcpiGetTable(NULL, RSDT_SIGNATURE);
        EntrySize = sizeof(ULONG);
    }
    if ((Root == NULL) || (Root->Length < sizeof(DESCRIPTION_HEADER)))
    {
        return NULL;
    }

    EntryCount = (Root->Length - sizeof(DESCRIPTION_HEADER)) / EntrySize;
    Entries = (PUCHAR)(Root + 1);

    List = ExAllocatePoolWithTag(PagedPool,
                                 (EntryCount + 1) * sizeof(ULONG_PTR),
                                 TAG_HAL);
    if (List == NULL)
    {
        return NULL;
    }

    Count = 0;
    for (i = 0; i < EntryCount; i++)
    {
        ULONG Signature;

        if (EntrySize == sizeof(ULONG))
        {
            Address.QuadPart = ((PULONG)Entries)[i];
        }
        else
        {
            Address = ((PPHYSICAL_ADDRESS)Entries)[i];
        }
        if (Address.QuadPart == 0)
        {
            continue;
        }

        /* Peek at the signature, then take the HAL's own cached copy */
        Header = MmMapIoSpace(Address, sizeof(DESCRIPTION_HEADER), MmNonCached);
        if (Header == NULL)
        {
            continue;
        }
        Signature = Header->Signature;
        MmUnmapIoSpace(Header, sizeof(DESCRIPTION_HEADER));

        if ((Signature == RSDT_SIGNATURE) || (Signature == XSDT_SIGNATURE) ||
            (Signature == FACS_SIGNATURE) || (Signature == SSDT_SIGNATURE))
        {
            continue;
        }

        Header = HalAcpiGetTable(NULL, Signature);
        if (Header != NULL)
        {
            List[1 + Count++] = (ULONG_PTR)Header;
        }
    }
    List[0] = Count;

    HalpAcpiTableList = List;
    return List;
}

/* DISPATCH ENTRIES ***********************************************************/

/**
 * @brief
 * Called by the driver when the PM timer overflowed. This HAL keeps time
 * with other counters, so there is nothing to carry.
 */
static
VOID
NTAPI
HalpAcpiTimerCarry(VOID)
{
    NOTHING;
}

/**
 * @brief
 * Receives the sleep-type values the driver evaluated for each S-state,
 * registers the states this HAL can enter with the power manager, and
 * reports which interrupt controller model the driver has to route for.
 */
static
VOID
NTAPI
HalpAcpiInitializeSleepStates(
    _In_ PHAL_ACPI_SLEEP_STATE SleepStates,
    _Out_ PULONG InterruptModel)
{
    PAGED_CODE();

    RtlCopyMemory(HalpSleepStates, SleepStates, sizeof(HalpSleepStates));

    /* Standby and soft-off are the transitions this HAL performs itself */
    if (HalpSleepStates[0].Supported)
    {
        HalpRegisterSleepState(PowerStateSleeping1, 1, &HalpSleepStates[0], TRUE);
    }
    if (HalpSleepStates[4].Supported)
    {
        HalpRegisterSleepState(PowerStateShutdownOff, 5, &HalpSleepStates[4], FALSE);
    }

    *InterruptModel = HalpInterruptModel;
    KeFlushWriteBuffer();
}

/**
 * @brief
 * Reports the HAL's ACPI capabilities; bit 0 tells the driver it runs
 * on an APIC machine.
 */
static
ULONG
NTAPI
HalpAcpiCapabilityFlags(VOID)
{
    return (HalpInterruptModel == HAL_ACPI_INTERRUPT_MODEL_APIC) ? 1 : 0;
}

/**
 * @brief
 * Tells the driver whether the interrupt controller kept its state
 * across the last sleep transition.
 */
static
BOOLEAN
NTAPI
HalpAcpiControllerStateIntact(VOID)
{
    return HalpInterruptControllerIntact;
}

/**
 * @brief
 * Reprograms the interrupt controller after a transition that lost it.
 */
static
VOID
NTAPI
HalpAcpiRestoreInterruptController(VOID)
{
    HalpRestoreInterruptController();
    HalpInterruptControllerIntact = TRUE;
}

/**
 * @brief
 * Raises the highest PCI bus number the legacy bus handlers cover.
 */
static
VOID
NTAPI
HalpAcpiSetMaxLegacyPciBusNumber(
    _In_ ULONG BusNumber)
{
    if (BusNumber > HalpMaxPciBus)
    {
        HalpMaxPciBus = BusNumber;
    }
}

/**
 * @brief
 * Returns a cached ACPI table by signature, optionally matched on the OEM
 * identifiers.
 */
static
PVOID
NTAPI
HalpAcpiGetTableEntry(
    _In_ ULONG Signature,
    _In_opt_ PCSTR OemId,
    _In_opt_ PCSTR OemTableId)
{
    PDESCRIPTION_HEADER Header;

    Header = HalAcpiGetTable(NULL, Signature);
    if (Header == NULL)
    {
        return NULL;
    }

    if ((OemId != NULL) &&
        (RtlCompareMemory(Header->OEMID, OemId, sizeof(Header->OEMID)) != sizeof(Header->OEMID)))
    {
        return NULL;
    }
    if ((OemTableId != NULL) &&
        (RtlCompareMemory(Header->OEMTableID,
                          OemTableId,
                          sizeof(Header->OEMTableID)) != sizeof(Header->OEMTableID)))
    {
        return NULL;
    }

    return Header;
}

/**
 * @brief
 * Returns the root pointer block the loader handed over.
 */
static
PVOID
NTAPI
HalpAcpiGetRsdpEntry(VOID)
{
    return HalpAcpiMultiNode;
}

/**
 * @brief
 * Returns the mapped firmware ACPI control structure.
 */
static
PVOID
NTAPI
HalpAcpiGetFacsEntry(VOID)
{
    PVOID Facs;

    ExAcquireFastMutex(&HalpAcpiDispatchLock);
    Facs = HalpMapFacs();
    ExReleaseFastMutex(&HalpAcpiDispatchLock);

    return Facs;
}

/**
 * @brief
 * Returns the list of secondary tables.
 */
static
PVOID
NTAPI
HalpAcpiGetAllTablesEntry(VOID)
{
    PVOID List;

    ExAcquireFastMutex(&HalpAcpiDispatchLock);
    List = HalpBuildAcpiTableList();
    ExReleaseFastMutex(&HalpAcpiDispatchLock);

    return List;
}

/**
 * @brief
 * Enables or disables waking through the RTC alarm; a change forgets any
 * alarm that was armed.
 */
static
VOID
NTAPI
HalpAcpiSetWakeEnable(
    _In_ BOOLEAN Enable)
{
    HalpWakeAlarmEnabled = Enable;
    HalpWakeAlarmArmed = FALSE;
    if (!Enable)
    {
        HalpProgramRtcAlarm(NULL);
    }
}

/**
 * @brief
 * Arms the RTC alarm for a wake time, or clears it when the time is zero.
 */
static
VOID
NTAPI
HalpAcpiSetWakeAlarm(
    _In_ ULONGLONG AlarmTime,
    _In_ PTIME_FIELDS WakeTime)
{
    TIME_FIELDS Now;
    LARGE_INTEGER NowTime, WakeAbsolute;

    if (AlarmTime == 0)
    {
        HalpWakeAlarmArmed = FALSE;
        HalpProgramRtcAlarm(NULL);
        return;
    }

    HalQueryRealTimeClock(&Now);
    RtlTimeFieldsToTime(&Now, &NowTime);
    RtlTimeFieldsToTime(WakeTime, &WakeAbsolute);

    /* An alarm this close would fire before the machine is asleep */
    if (WakeAbsolute.QuadPart - NowTime.QuadPart < HALP_MIN_WAKE_LEAD)
    {
        DPRINT1("Wake alarm too close, not armed\n");
        HalpWakeAlarmArmed = FALSE;
        return;
    }

    HalpWakeAlarmTime = *WakeTime;
    HalpWakeAlarmArmed = TRUE;
    HalpProgramRtcAlarm(WakeTime);
}

/**
 * @brief
 * Power-state callback the kernel raises around hibernation, telling the
 * HAL to preserve or drop the firmware NVS area. Hibernation is not
 * supported here, so both notifications are only recorded.
 */
static
VOID
NTAPI
HalpAcpiPowerStateCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,
    _In_ PVOID Argument2)
{
    UNREFERENCED_PARAMETER(CallbackContext);

    if (Argument1 != UlongToPtr(PO_CB_SYSTEM_STATE_LOCK))
    {
        return;
    }

    DPRINT("NVS area %s requested\n", Argument2 ? "release" : "preserve");
}

/* The services offered to the ACPI driver */
static const HAL_ACPI_DISPATCH_TABLE HalpAcpiDispatchTable =
{
    HAL_ACPI_DISPATCH_SIGNATURE,
    HAL_ACPI_DISPATCH_VERSION,
    HaliAcpiTimerInit,
    HalpAcpiTimerCarry,
    HalpAcpiInitializeSleepStates,
    HalpAcpiCapabilityFlags,
    HalpAcpiControllerStateIntact,
    HalpAcpiRestoreInterruptController,
    HaliPciInterfaceReadConfig,
    HaliPciInterfaceWriteConfig,
    HalpGetInterruptControllerVersion,
    HalpAcpiSetMaxLegacyPciBusNumber,
    HalpIsInterruptInputValid,
    HalpAcpiGetTableEntry,
    HalpAcpiGetRsdpEntry,
    HalpAcpiGetFacsEntry,
    HalpAcpiGetAllTablesEntry
};

/* The driver expects the entries at these positions */
C_ASSERT(FIELD_OFFSET(HAL_ACPI_DISPATCH_TABLE, MachineStateInit) ==
         2 * sizeof(ULONG) + 2 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(HAL_ACPI_DISPATCH_TABLE, PciReadConfig) ==
         2 * sizeof(ULONG) + 6 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(HAL_ACPI_DISPATCH_TABLE, GetAllTables) ==
         2 * sizeof(ULONG) + 14 * sizeof(PVOID));

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * The HalInitPowerManagement dispatch entry: exchanges dispatch tables with
 * the ACPI driver and installs the HAL's wake services.
 *
 * @param[in] PmDriverDispatchTable
 * The driver's callbacks (may be NULL for a driver that has none).
 *
 * @param[out] PmHalDispatchTable
 * Receives the HAL's table.
 */
NTSTATUS
NTAPI
HaliInitPowerManagement(
    _In_ PPM_DISPATCH_TABLE PmDriverDispatchTable,
    _Out_ PPM_DISPATCH_TABLE *PmHalDispatchTable)
{
    UNICODE_STRING CallbackName = RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    OBJECT_ATTRIBUTES ObjectAttributes;
    PCALLBACK_OBJECT CallbackObject;
    NTSTATUS Status;

    PAGED_CODE();

    ExInitializeFastMutex(&HalpAcpiDispatchLock);

    if ((PmDriverDispatchTable != NULL) &&
        (PmDriverDispatchTable->Signature == ACPI_DRIVER_DISPATCH_SIGNATURE))
    {
        HalpAcpiDriverDispatch = (PACPI_DRIVER_DISPATCH_TABLE)PmDriverDispatchTable;
    }
    else
    {
        DPRINT1("ACPI driver offered no callback table\n");
        HalpAcpiDriverDispatch = NULL;
    }

    HalSetWakeEnable = HalpAcpiSetWakeEnable;
    HalSetWakeAlarm = HalpAcpiSetWakeAlarm;

    *PmHalDispatchTable = (PPM_DISPATCH_TABLE)&HalpAcpiDispatchTable;

    InitializeObjectAttributes(&ObjectAttributes,
                               &CallbackName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
                               NULL,
                               NULL);
    Status = ExCreateCallback(&CallbackObject, &ObjectAttributes, FALSE, TRUE);
    if (NT_SUCCESS(Status))
    {
        ExRegisterCallback(CallbackObject, HalpAcpiPowerStateCallback, NULL);
    }

    return STATUS_SUCCESS;
}

/* EOF */
