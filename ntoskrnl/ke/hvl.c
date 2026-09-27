/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hypervisor library, the root side of the guest interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The interface is the one the Hypervisor Top Level Functional Specification
 * describes, so any hypervisor that implements it, ReactV or Hyper-V, is
 * talked to the same way and by the same names the root side drivers import.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DEFINES ********************************************************************/

#define HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION 0x40000000
#define HV_CPUID_FUNCTION_INTERFACE               0x40000001
#define HV_CPUID_FUNCTION_FEATURES                0x40000003

/* 'Microsoft Hv' across EBX, ECX and EDX, and 'Hv#1' in EAX */
#define HV_VENDOR_EBX                             0x7263694D
#define HV_VENDOR_ECX                             0x666F736F
#define HV_VENDOR_EDX                             0x76482074
#define HV_INTERFACE_SIGNATURE                    0x31237648

#define HV_PARTITION_PRIVILEGE_ACCESS_SYNIC_REGS  0x00000004
#define HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS 0x00000020

#define HV_X64_MSR_GUEST_OS_ID                    0x40000000
#define HV_X64_MSR_HYPERCALL                      0x40000001
#define HV_X64_MSR_VP_INDEX                       0x40000002
#define HV_X64_MSR_SCONTROL                       0x40000080
#define HV_X64_MSR_SINT0                          0x40000090

#define HV_SYNIC_CONTROL_ENABLE                   0x0000000000000001ULL

/*
 * The vector the hypervisor's own interrupt arrives on. It is the lowest of a
 * priority class of its own: above the software interrupts, and below anything
 * a device is ever given.
 */
#define HVL_INTERRUPT_VECTOR                      0x30

#define HV_X64_MSR_HYPERCALL_ENABLE               0x0000000000000001ULL

#define HV_STATUS_SUCCESS                         0x0000
#define HV_STATUS_INVALID_HYPERCALL_CODE          0x0002
#define HV_STATUS_INVALID_HYPERCALL_INPUT         0x0003

#define HV_HYPERCALL_STATUS_MASK                  0x000000000000FFFFULL

/*
 * A hypercall that travels in registers carries sixteen bytes in two general
 * ones and sixteen more in each of XMM0 through XMM5, so seven units in all.
 */
#define HVL_FAST_UNIT_SIZE                        16
#define HVL_FAST_EXTENDED_UNITS                   6
#define HVL_FAST_PAYLOAD_UNITS                    (HVL_FAST_EXTENDED_UNITS + 1)

/*
 * Who is calling, per the specification's guest identity layout. The top bit
 * says this is an open source operating system rather than one of the vendors
 * Microsoft keeps a number for.
 */
#define HV_GUEST_OS_VENDOR_REACTOS                0x8200
#define HvlpMakeGuestOsId(Major, Minor, Build) \
    (((ULONG64)HV_GUEST_OS_VENDOR_REACTOS << 48) | \
     ((ULONG64)(Major) << 32) | \
     ((ULONG64)(Minor) << 24) | \
     ((ULONG64)(Build)))

#define HVL_MAXIMUM_INTERRUPT_CALLBACKS 8

typedef
ULONG64
(NTAPI *PHVL_HYPERCALL_ROUTINE)(
    _In_ ULONG64 InputValue,
    _In_ ULONG64 InputPa,
    _In_ ULONG64 OutputPa);

typedef
VOID
(NTAPI *PHVL_INTERRUPT_ROUTINE)(
    _In_opt_ PVOID Context);

typedef struct _HVL_INTERRUPT_CALLBACK
{
    PVOID Callback;
    PVOID Context;
} HVL_INTERRUPT_CALLBACK, *PHVL_INTERRUPT_CALLBACK;

/* GLOBALS ********************************************************************/

BOOLEAN HvlpHypervisorPresent;
static ULONG HvlpMaximumLeaf;
static ULONG HvlpPrivileges;
static PVOID HvlpHypercallPage;

#ifdef _M_AMD64
/* ke/amd64/hvlcall.S, which is where the registers can be reached from */
ULONG64
NTAPI
HvlpExtendedHypercall(
    _In_ ULONG64 InputValue,
    _In_ PVOID Payload,
    _In_ ULONG64 Units,
    _Out_writes_bytes_(HVL_FAST_EXTENDED_UNITS * HVL_FAST_UNIT_SIZE) PVOID Extended,
    _In_ PVOID HypercallPage);
#endif
static PMDL HvlpHypercallMdl;
static PKEVENT HvlpWithdrawAllowedEvent;
static HVL_INTERRUPT_CALLBACK HvlpInterruptCallbacks[HVL_MAXIMUM_INTERRUPT_CALLBACKS];
static PVOID HvlpWheaCallback;
static ULONG HvlpInterrupts;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Looks for a hypervisor that implements the interface this library speaks.
 *
 * @remarks
 * CPUID alone, so this is safe to call before anything else is up.
 */
CODE_SEG("INIT")
VOID
NTAPI
HvlpDetectHypervisor(VOID)
{
    INT Registers[4];

    /* Nothing above the hypervisor present bit is worth reading without it */
    __cpuid(Registers, 1);
    if (!((ULONG)Registers[2] & 0x80000000))
        return;

    __cpuid(Registers, HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION);
    if (((ULONG)Registers[1] != HV_VENDOR_EBX) ||
        ((ULONG)Registers[2] != HV_VENDOR_ECX) ||
        ((ULONG)Registers[3] != HV_VENDOR_EDX))
    {
        DPRINT1("Hvl: a hypervisor is present but not one we know\n");
        return;
    }

    HvlpMaximumLeaf = (ULONG)Registers[0];

    __cpuid(Registers, HV_CPUID_FUNCTION_INTERFACE);
    if ((ULONG)Registers[0] != HV_INTERFACE_SIGNATURE)
    {
        DPRINT1("Hvl: interface signature is %lx\n", (ULONG)Registers[0]);
        return;
    }

    if (HvlpMaximumLeaf >= HV_CPUID_FUNCTION_FEATURES)
    {
        __cpuid(Registers, HV_CPUID_FUNCTION_FEATURES);
        HvlpPrivileges = (ULONG)Registers[0];
    }

    HvlpHypervisorPresent = TRUE;
    DPRINT1("Hvl: hypervisor found, leaves to %lx, privileges %lx\n",
            HvlpMaximumLeaf, HvlpPrivileges);
}

/**
 * @brief
 * Publishes the event that says memory may be taken back from the hypervisor.
 *
 * @remarks
 * The virtualization stack waits on this before it withdraws memory it had
 * deposited, and refuses to start if it is not there to wait on. Nothing here
 * ever defers a withdrawal, so it starts signalled and stays that way.
 */
CODE_SEG("INIT")
VOID
NTAPI
HvlInitSystemEvents(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\KernelObjects\\HvlWithdrawAllowed");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    NTSTATUS Status;

    if (!HvlpHypervisorPresent)
        return;

    InitializeObjectAttributes(&ObjectAttributes,
                               &Name,
                               OBJ_KERNEL_HANDLE | OBJ_PERMANENT,
                               NULL,
                               NULL);

    Status = ZwCreateEvent(&Handle,
                           EVENT_ALL_ACCESS,
                           &ObjectAttributes,
                           NotificationEvent,
                           TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hvl: could not publish the withdraw event, status %lx\n", Status);
        return;
    }

    Status = ObReferenceObjectByHandle(Handle,
                                       EVENT_MODIFY_STATE,
                                       ExEventObjectType,
                                       KernelMode,
                                       (PVOID *)&HvlpWithdrawAllowedEvent,
                                       NULL);
    ZwClose(Handle);

    if (!NT_SUCCESS(Status))
        DPRINT1("Hvl: the withdraw event would not be referenced, status %lx\n", Status);
}

#ifdef _M_AMD64
/**
 * @brief
 * What the hypervisor's own interrupt does, which is to let whoever registered
 * for it look at what arrived.
 *
 * @remarks
 * One vector covers everything the hypervisor has to say to this processor, so
 * which of it matters is for the registered routines to work out. The pages the
 * messages are written in belong to them rather than to this library, which is
 * why nothing here reads one.
 */
VOID
NTAPI
HvlInterruptHandler(VOID)
{
    ULONG Index;

    HvlpInterrupts++;

    if (HvlpInterrupts <= 2)
        DPRINT1("Hvl: the hypervisor raised its interrupt\n");

    for (Index = 0; Index < HVL_MAXIMUM_INTERRUPT_CALLBACKS; Index++)
    {
        PHVL_INTERRUPT_ROUTINE Routine;

        Routine = (PHVL_INTERRUPT_ROUTINE)HvlpInterruptCallbacks[Index].Callback;

        if (Routine != NULL)
            Routine(HvlpInterruptCallbacks[Index].Context);
    }
}

/**
 * @brief
 * Turns this processor's synthetic controller on and gives the hypervisor a
 * vector to raise its own interrupt on.
 *
 * @remarks
 * Only the first of the sixteen interrupts is this library's, because that is
 * the one the hypervisor's own messages arrive on. Where those messages are
 * written is not set here: a root is given its pages by the hypervisor, and
 * whoever reads a message maps them for itself.
 */
CODE_SEG("INIT")
static
VOID
NTAPI
HvlpConnectSynic(VOID)
{
    ULONG64 Control;

    if (!(HvlpPrivileges & HV_PARTITION_PRIVILEGE_ACCESS_SYNIC_REGS))
    {
        DPRINT1("Hvl: the controller registers are not ours to touch\n");
        return;
    }

    Control = __readmsr(HV_X64_MSR_SCONTROL);
    __writemsr(HV_X64_MSR_SCONTROL, Control | HV_SYNIC_CONTROL_ENABLE);

    /* The vector alone, so the interrupt is neither masked nor ended for us */
    __writemsr(HV_X64_MSR_SINT0, HVL_INTERRUPT_VECTOR);

    DPRINT1("Hvl: the controller is on, its own interrupt is vector %x\n",
            HVL_INTERRUPT_VECTOR);
}
#endif

/**
 * @brief
 * Tells the hypervisor who we are and takes the hypercall page it offers.
 *
 * @remarks
 * Needs the memory manager, so this belongs in phase 1 and no earlier.
 */
CODE_SEG("INIT")
VOID
NTAPI
HvlInitSystem(VOID)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    PHYSICAL_ADDRESS PageAddress;
    ULONG64 GuestOsId;

    HvlpDetectHypervisor();

    if (!HvlpHypervisorPresent)
        return;

    if (!(HvlpPrivileges & HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS))
    {
        DPRINT1("Hvl: the hypercall MSRs are not ours to touch\n");
        return;
    }

    /*
     * The identity comes first. A hypervisor keeps the hypercall page shut
     * until it knows who is asking, and drops it again if the identity is
     * cleared.
     */
    GuestOsId = HvlpMakeGuestOsId(NtMajorVersion, NtMinorVersion, NtBuildNumber);
    __writemsr(HV_X64_MSR_GUEST_OS_ID, GuestOsId);

    /* One page, and it has to be one the processor may execute from */
    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;

    HvlpHypercallMdl = MmAllocatePagesForMdlEx(Low,
                                               High,
                                               Skip,
                                               PAGE_SIZE,
                                               MmCached,
                                               MM_ALLOCATE_FULLY_REQUIRED);
    if (HvlpHypercallMdl == NULL)
    {
        DPRINT1("Hvl: no page for the hypercall stub\n");
        __writemsr(HV_X64_MSR_GUEST_OS_ID, 0);
        return;
    }

    HvlpHypercallPage = MmMapLockedPagesSpecifyCache(HvlpHypercallMdl,
                                                     KernelMode,
                                                     MmCached,
                                                     NULL,
                                                     FALSE,
                                                     NormalPagePriority);
    if (HvlpHypercallPage == NULL)
    {
        DPRINT1("Hvl: the hypercall page would not map\n");
        MmFreePagesFromMdl(HvlpHypercallMdl);
        ExFreePool(HvlpHypercallMdl);
        HvlpHypercallMdl = NULL;
        __writemsr(HV_X64_MSR_GUEST_OS_ID, 0);
        return;
    }

    /* The hypervisor writes the stub into the page as this takes effect */
    PageAddress.QuadPart = (LONGLONG)*MmGetMdlPfnArray(HvlpHypercallMdl) << PAGE_SHIFT;
    __writemsr(HV_X64_MSR_HYPERCALL,
               (ULONG64)PageAddress.QuadPart | HV_X64_MSR_HYPERCALL_ENABLE);

    if (!(__readmsr(HV_X64_MSR_HYPERCALL) & HV_X64_MSR_HYPERCALL_ENABLE))
    {
        DPRINT1("Hvl: the hypervisor refused the hypercall page\n");
        HvlpHypercallPage = NULL;
        return;
    }

    DPRINT1("Hvl: hypercall page at %p, physical %I64x\n",
            HvlpHypercallPage, PageAddress.QuadPart);

#ifdef _M_AMD64
    HvlpConnectSynic();
#endif
}

/**
 * @brief
 * Tells whether the system runs underneath a hypervisor.
 */
BOOLEAN
NTAPI
HvlIsAnyHypervisorPresent(VOID)
{
    return HvlpHypervisorPresent;
}

/**
 * @brief
 * Returns how many processors the hypervisor keeps active.
 *
 * @param[out] Count
 * Receives the count.
 */
NTSTATUS
NTAPI
HvlQueryActiveHypervisorProcessorCount(
    _Out_ PULONG Count)
{
    if (!HvlpHypervisorPresent)
        return STATUS_UNSUCCESSFUL;

    *Count = (ULONG)KeNumberProcessors;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes a hypercall.
 *
 * @param[in] InputValue
 * The control value: the call code in its low word, the fast and nested bits,
 * and the repetition count for a call that takes one.
 *
 * @param[in] InputPa
 * Physical address of the input page, or the first value of a fast call.
 *
 * @param[in] OutputPa
 * Physical address of the output page, or the second value of a fast call.
 *
 * @return
 * What the hypervisor returned, with the status in the low word.
 */
ULONG64
NTAPI
HvlInvokeHypercall(
    _In_ ULONG64 InputValue,
    _In_ ULONG64 InputPa,
    _In_ ULONG64 OutputPa)
{
    ULONG64 Result;

    if (HvlpHypercallPage == NULL)
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    Result = ((PHVL_HYPERCALL_ROUTINE)HvlpHypercallPage)(InputValue,
                                                        InputPa,
                                                        OutputPa);

    if ((Result & HV_HYPERCALL_STATUS_MASK) != HV_STATUS_SUCCESS)
        DPRINT1("Hvl: call %I64x answered %I64x\n", InputValue, Result);

    return Result;
}

/**
 * @brief
 * Makes a hypercall that carries its input and output in registers.
 *
 * @param[in] InputValue
 * The control value, which has to have the fast bit set for the hypervisor to
 * look in the registers at all.
 *
 * @param[in] InputBuffer
 * The payload. Its first sixteen bytes travel in two general registers and
 * each further sixteen in one of XMM0 through XMM5, which is as much as the
 * form can carry.
 *
 * @param[in] InputSize
 * How long the payload is, in bytes.
 *
 * @param[out] OutputBuffer
 * Receives the answer.
 *
 * @param[in] OutputSize
 * How long the answer is, in whole sixteen byte units.
 *
 * @return
 * What the hypervisor returned, or HV_STATUS_INVALID_HYPERCALL_INPUT for a
 * payload or an answer too long for the registers to hold.
 *
 * @remarks
 * The answer comes back in the registers that follow the ones the payload
 * used, so how long the payload was decides where to read it from.
 */
ULONG64
NTAPI
HvlInvokeFastExtendedHypercall(
    _In_ ULONG64 InputValue,
    _In_reads_bytes_opt_(InputSize) PVOID InputBuffer,
    _In_ SIZE_T InputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID OutputBuffer,
    _In_ SIZE_T OutputSize)
{
#ifdef _M_AMD64
    ULONG64 Payload[HVL_FAST_PAYLOAD_UNITS * 2];
    ULONG64 Extended[HVL_FAST_EXTENDED_UNITS * 2];
    ULONG64 Units, First, Result;

    if (HvlpHypercallPage == NULL)
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    if ((InputSize > sizeof(Payload)) || (OutputSize > sizeof(Extended)))
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    if ((InputSize != 0) && (InputBuffer == NULL))
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    if ((OutputSize != 0) && (OutputBuffer == NULL))
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    /* A unit that is only partly used still travels whole, so it is padded */
    RtlZeroMemory(Payload, sizeof(Payload));
    if (InputSize != 0)
        RtlCopyMemory(Payload, InputBuffer, InputSize);

    Units = (InputSize + (HVL_FAST_UNIT_SIZE - 1)) / HVL_FAST_UNIT_SIZE;

    Result = HvlpExtendedHypercall(InputValue,
                                   Payload,
                                   Units,
                                   Extended,
                                   HvlpHypercallPage);

    if ((Result & HV_HYPERCALL_STATUS_MASK) != HV_STATUS_SUCCESS)
    {
        DPRINT1("Hvl: call %I64x answered %I64x\n", InputValue, Result);
        return Result;
    }

    if (OutputSize == 0)
        return Result;

    /*
     * The first unit of the answer is in the register the payload's last unit
     * travelled in. A payload short enough to need no XMM register at all
     * would have its answer back in the two general ones, which nothing asks
     * for, so it is refused rather than read from the wrong place.
     */
    if (Units == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    First = Units - 1;

    if ((First + ((OutputSize + HVL_FAST_UNIT_SIZE - 1) / HVL_FAST_UNIT_SIZE)) >
        HVL_FAST_EXTENDED_UNITS)
    {
        return HV_STATUS_INVALID_HYPERCALL_INPUT;
    }

    RtlCopyMemory(OutputBuffer, &Extended[First * 2], OutputSize);

    return Result;
#else
    UNREFERENCED_PARAMETER(InputValue);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputSize);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputSize);

    /* The registers this form uses are not part of the 32 bit calling convention */
    return HV_STATUS_INVALID_HYPERCALL_INPUT;
#endif
}

/**
 * @brief
 * Registers a routine to be called when the hypervisor signals the root.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES when there is no slot left.
 *
 * @remarks
 * Every registered routine is called on the hypervisor's own interrupt, however
 * many there are, because one vector carries all of it. The type is what the
 * caller is registering for and what it may unregister.
 */
NTSTATUS
NTAPI
HvlRegisterInterruptCallback(
    _In_ ULONG Type,
    _In_ PVOID Callback,
    _In_opt_ PVOID Context)
{
    if (Type >= HVL_MAXIMUM_INTERRUPT_CALLBACKS)
        return STATUS_INVALID_PARAMETER;

    if (HvlpInterruptCallbacks[Type].Callback != NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    HvlpInterruptCallbacks[Type].Context = Context;
    HvlpInterruptCallbacks[Type].Callback = Callback;

    DPRINT1("Hvl: %p takes the hypervisor's interrupt, kind %lu\n", Callback, Type);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes back a routine registered with HvlRegisterInterruptCallback.
 */
NTSTATUS
NTAPI
HvlUnregisterInterruptCallback(
    _In_ ULONG Type,
    _In_ PVOID Callback)
{
    if (Type >= HVL_MAXIMUM_INTERRUPT_CALLBACKS)
        return STATUS_INVALID_PARAMETER;

    if (HvlpInterruptCallbacks[Type].Callback != Callback)
        return STATUS_NOT_FOUND;

    HvlpInterruptCallbacks[Type].Callback = NULL;
    HvlpInterruptCallbacks[Type].Context = NULL;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reports what a virtual processor dispatch has to take care of.
 *
 * @param[in] Flags
 * What the caller is about to dispatch.
 *
 * @param[out] FlushTlb
 * Whether the dispatch has to flush the address space behind it.
 *
 * @param[out] FlushCache
 * Whether it has to write the caches back.
 *
 * @param[out] FlushBranchPredictor
 * Whether it has to clear the branch predictor.
 *
 * @param[out] RestoreSpeculationControl
 * Whether the caller has to put the speculation control register back
 * afterwards, using the value in @p SpeculationControl.
 *
 * @param[in,out] SpeculationControl
 * The speculation control value to put back.
 *
 * @remarks
 * The kernel keeps no per dispatch mitigation state, so nothing is asked of
 * the caller. Every answer still has to be written, because the caller reads
 * all of them and never sets them itself.
 */
VOID
NTAPI
KePrepareToDispatchVirtualProcessor(
    _In_ ULONG Flags,
    _Out_ PBOOLEAN FlushTlb,
    _Out_ PBOOLEAN FlushCache,
    _Out_ PBOOLEAN FlushBranchPredictor,
    _Out_ PBOOLEAN RestoreSpeculationControl,
    _Inout_ PULONG64 SpeculationControl)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(SpeculationControl);

    *FlushTlb = FALSE;
    *FlushCache = FALSE;
    *FlushBranchPredictor = FALSE;
    *RestoreSpeculationControl = FALSE;
}

/**
 * @brief
 * Lists the processors the hypervisor is running on.
 *
 * @param[in,out] Count
 * How many entries @p ProcessorIndexes holds on the way in, and how many
 * processors there are on the way out.
 *
 * @param[out] ProcessorIndexes
 * Receives the index of each of them, or NULL to only ask how many there are.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_BUFFER_TOO_SMALL when there was room for only
 * some of them, which the caller answers by asking again with more.
 */
NTSTATUS
NTAPI
HvlQueryActiveProcessors(
    _Inout_ PULONG Count,
    _Out_writes_opt_(*Count) PULONG ProcessorIndexes)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Active = (ULONG)KeNumberProcessors;
    ULONG Index;

    if (!HvlpHypervisorPresent)
        return STATUS_NOT_SUPPORTED;

    if (Count == NULL)
        return STATUS_INVALID_PARAMETER;

    if (ProcessorIndexes != NULL)
    {
        /* Every processor that exists is one the hypervisor is running on */
        for (Index = 0; (Index < *Count) && (Index < Active); Index++)
            ProcessorIndexes[Index] = Index;

        if (*Count < Active)
            Status = STATUS_BUFFER_TOO_SMALL;
    }

    *Count = Active;

    return Status;
}

/**
 * @brief
 * Returns the cost of reaching one node's memory from another.
 *
 * @return
 * STATUS_SUCCESS with the distance every node has to itself, because there is
 * only ever one node here.
 */
NTSTATUS
NTAPI
HvlQueryNumaDistance(
    _In_ USHORT FromNode,
    _In_ USHORT ToNode,
    _Out_ PULONGLONG Distance)
{
    if (!HvlpHypervisorPresent)
        return STATUS_NOT_SUPPORTED;

    if ((FromNode != 0) || (ToNode != 0))
        return STATUS_INVALID_PARAMETER;

    /* The ACPI SLIT calls a node's distance to itself ten */
    *Distance = 10;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns where a processor sits in the topology of the machine.
 *
 * @param[in] ProcessorIndex
 * The processor being asked about.
 *
 * @param[out] NodeNumber
 * Receives the node the processor belongs to.
 *
 * @param[out] PackageId
 * Receives the index of the first processor of its package.
 *
 * @param[out] CoreId
 * Receives the index of the first processor of its core.
 *
 * @param[out] Index
 * Receives the index of the processor itself.
 */
NTSTATUS
NTAPI
HvlQueryProcessorTopologyEx(
    _In_ ULONG ProcessorIndex,
    _Out_opt_ PUSHORT NodeNumber,
    _Out_opt_ PULONG PackageId,
    _Out_opt_ PULONG CoreId,
    _Out_opt_ PULONG Index)
{
    PKPRCB Prcb;

    if (!HvlpHypervisorPresent)
        return STATUS_NOT_SUPPORTED;

    if (ProcessorIndex >= (ULONG)KeNumberProcessors)
        return STATUS_INVALID_PARAMETER;

    Prcb = KiProcessorBlock[ProcessorIndex];
    if (Prcb == NULL)
        return STATUS_INVALID_PARAMETER;

    if (NodeNumber != NULL)
        *NodeNumber = 0;

    /* The machine is one package, so the first processor of it stands for it */
    if (PackageId != NULL)
        *PackageId = 0;

    /* A core is known by the thread of it that leads the rest */
    if (CoreId != NULL)
        *CoreId = Prcb->MultiThreadSetMaster->Number;

    if (Index != NULL)
        *Index = ProcessorIndex;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Returns where a processor sits in the topology of the machine.
 */
NTSTATUS
NTAPI
HvlQueryProcessorTopology(
    _In_ ULONG ProcessorIndex,
    _Out_opt_ PUSHORT NodeNumber,
    _Out_opt_ PULONG PackageId,
    _Out_opt_ PULONG CoreId)
{
    return HvlQueryProcessorTopologyEx(ProcessorIndex,
                                       NodeNumber,
                                       PackageId,
                                       CoreId,
                                       NULL);
}

/**
 * @brief
 * Returns the highest processor and node the machine has.
 */
NTSTATUS
NTAPI
HvlQueryProcessorTopologyHighestId(
    _Out_opt_ PULONG HighestProcessor,
    _Out_opt_ PULONG HighestNode)
{
    if (!HvlpHypervisorPresent)
        return STATUS_NOT_SUPPORTED;

    if (HighestProcessor != NULL)
        *HighestProcessor = (ULONG)KeNumberProcessors - 1;

    if (HighestNode != NULL)
        *HighestNode = 0;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Asks to be told when the hypervisor reports a hardware error.
 *
 * @remarks
 * Remembered and never called, as nothing routes hypervisor error reports yet.
 */
NTSTATUS
NTAPI
HvlRegisterWheaErrorNotification(
    _In_ PVOID Callback)
{
    if (!HvlpHypervisorPresent)
        return STATUS_NOT_SUPPORTED;

    HvlpWheaCallback = Callback;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HvlUnregisterWheaErrorNotification(
    _In_ PVOID Callback)
{
    if (HvlpWheaCallback != Callback)
        return STATUS_NOT_FOUND;

    HvlpWheaCallback = NULL;
    return STATUS_SUCCESS;
}

/* EOF */
