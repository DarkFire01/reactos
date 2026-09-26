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

#define HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS 0x00000020

#define HV_X64_MSR_GUEST_OS_ID                    0x40000000
#define HV_X64_MSR_HYPERCALL                      0x40000001
#define HV_X64_MSR_VP_INDEX                       0x40000002

#define HV_X64_MSR_HYPERCALL_ENABLE               0x0000000000000001ULL

#define HV_STATUS_INVALID_HYPERCALL_CODE          0x0002
#define HV_STATUS_INVALID_HYPERCALL_INPUT         0x0003

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
static PMDL HvlpHypercallMdl;
static HVL_INTERRUPT_CALLBACK HvlpInterruptCallbacks[HVL_MAXIMUM_INTERRUPT_CALLBACKS];

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
 * Returns the count of processors the hypervisor keeps active.
 */
ULONG
NTAPI
HvlQueryActiveHypervisorProcessorCount(VOID)
{
    if (!HvlpHypervisorPresent)
        return 0;

    return KeNumberProcessors;
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
    if (HvlpHypercallPage == NULL)
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    return ((PHVL_HYPERCALL_ROUTINE)HvlpHypercallPage)(InputValue,
                                                       InputPa,
                                                       OutputPa);
}

/**
 * @brief
 * Makes a fast hypercall that carries its input and output in registers.
 *
 * @return
 * What the hypervisor returned, or HV_STATUS_INVALID_HYPERCALL_INPUT for a
 * call that would need the register form this does not implement yet.
 *
 * @remarks
 * Two input values fit in registers. Anything longer, and any output at all,
 * belongs in the XMM registers the extended form uses, which is not here yet.
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
    PULONG64 Input = InputBuffer;
    ULONG64 First = 0;
    ULONG64 Second = 0;

    if (HvlpHypercallPage == NULL)
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    if ((InputSize > (2 * sizeof(ULONG64))) || (OutputSize != 0))
    {
        UNREFERENCED_PARAMETER(OutputBuffer);
        return HV_STATUS_INVALID_HYPERCALL_INPUT;
    }

    if ((Input != NULL) && (InputSize >= sizeof(ULONG64)))
        First = Input[0];

    if ((Input != NULL) && (InputSize >= (2 * sizeof(ULONG64))))
        Second = Input[1];

    return ((PHVL_HYPERCALL_ROUTINE)HvlpHypercallPage)(InputValue,
                                                       First,
                                                       Second);
}

/**
 * @brief
 * Registers a routine to be called when the hypervisor signals the root.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES when there is no slot left.
 *
 * @remarks
 * The synthetic interrupt controller is not wired up yet, so a registered
 * callback is remembered and never called.
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
 * Tells the hypervisor that a virtual processor is about to be dispatched.
 *
 * @remarks
 * A scheduling hint, and the hypervisor is free to be told nothing.
 */
VOID
NTAPI
KePrepareToDispatchVirtualProcessor(VOID)
{
    NOTHING;
}

/* EOF */
