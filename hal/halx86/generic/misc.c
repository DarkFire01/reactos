/*
 * PROJECT:         ReactOS Hardware Abstraction Layer (HAL)
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * PURPOSE:         I/O Mapping and x86 Subs
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "smp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS  *******************************************************************/

/*
 * What the firmware said about the interrupt controllers. An ACPI HAL fills
 * this in while parsing the MADT; a HAL built without ACPI leaves it zeroed,
 * and the APIC code then assumes the one standard I/O APIC.
 *
 * It lives in the generic component because the producer (acpi/madt.c) and the
 * consumer (apic/apic.c) are in components that are not always linked together.
 */
HALP_APIC_INFO_TABLE HalpApicInfoTable;

/*
 * Secondary interrupts are not lines on the primary controller. They stand for
 * a pin that a secondary controller (a GPIO controller, say) demultiplexes out
 * of one of its own inputs, so nothing programs an I/O APIC redirection entry
 * for one and the number only has to be unique.
 *
 * The base sits far above any GSI a platform can describe - an I/O APIC tops
 * out well below this, and the 8259 at sixteen - so the two number spaces
 * cannot collide.
 */
#define HALP_SECONDARY_GSIV_BASE    0x1000
#define HALP_SECONDARY_GSIV_LIMIT   0x2000

static volatile LONG HalpNextSecondaryGsiv = HALP_SECONDARY_GSIV_BASE;

UCHAR HalpSerialLen;
CHAR HalpSerialNumber[31];

/* PRIVATE FUNCTIONS **********************************************************/

#ifndef _MINIHAL_
CODE_SEG("INIT")
VOID
NTAPI
HalpReportSerialNumber(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING KeyString;
    HANDLE Handle;

    /* Make sure there is a serial number */
    if (!HalpSerialLen) return;

    /* Open the system key */
    RtlInitUnicodeString(&KeyString, L"\\Registry\\Machine\\Hardware\\Description\\System");
    Status = HalpOpenRegistryKey(&Handle, 0, &KeyString, KEY_ALL_ACCESS, FALSE);
    if (NT_SUCCESS(Status))
    {
        /* Add the serial number */
        RtlInitUnicodeString(&KeyString, L"Serial Number");
        ZwSetValueKey(Handle,
                      &KeyString,
                      0,
                      REG_BINARY,
                      HalpSerialNumber,
                      HalpSerialLen);

        /* Close the handle */
        ZwClose(Handle);
    }
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
HalpMarkAcpiHal(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING KeyString;
    HANDLE KeyHandle;
    HANDLE Handle;
    ULONG Value = HalDisableFirmwareMapper ? 1 : 0;

    /* Open the control set key */
    RtlInitUnicodeString(&KeyString,
                         L"\\REGISTRY\\MACHINE\\SYSTEM\\CURRENTCONTROLSET");
    Status = HalpOpenRegistryKey(&Handle, 0, &KeyString, KEY_ALL_ACCESS, FALSE);
    if (NT_SUCCESS(Status))
    {
        /* Open the PNP key */
        RtlInitUnicodeString(&KeyString, L"Control\\Pnp");
        Status = HalpOpenRegistryKey(&KeyHandle,
                                     Handle,
                                     &KeyString,
                                     KEY_ALL_ACCESS,
                                     TRUE);
        /* Close root key */
        ZwClose(Handle);

        /* Check if PNP BIOS key exists */
        if (NT_SUCCESS(Status))
        {
            /* Set the disable value to false -- we need the mapper */
            RtlInitUnicodeString(&KeyString, L"DisableFirmwareMapper");
            Status = ZwSetValueKey(KeyHandle,
                                   &KeyString,
                                   0,
                                   REG_DWORD,
                                   &Value,
                                   sizeof(Value));

            /* Close subkey */
            ZwClose(KeyHandle);
        }
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpOpenRegistryKey(IN PHANDLE KeyHandle,
                    IN HANDLE RootKey,
                    IN PUNICODE_STRING KeyName,
                    IN ACCESS_MASK DesiredAccess,
                    IN BOOLEAN Create)
{
    NTSTATUS Status;
    ULONG Disposition;
    OBJECT_ATTRIBUTES ObjectAttributes;

    /* Setup the attributes we received */
    InitializeObjectAttributes(&ObjectAttributes,
                               KeyName,
                               OBJ_CASE_INSENSITIVE,
                               RootKey,
                               NULL);

    /* What to do? */
    if ( Create )
    {
        /* Create the key */
        Status = ZwCreateKey(KeyHandle,
                             DesiredAccess,
                             &ObjectAttributes,
                             0,
                             NULL,
                             REG_OPTION_VOLATILE,
                             &Disposition);
    }
    else
    {
        /* Open the key */
        Status = ZwOpenKey(KeyHandle, DesiredAccess, &ObjectAttributes);
    }

    /* We're done */
    return Status;
}
#endif /* !_MINIHAL_ */

VOID
NTAPI
HalpCheckPowerButton(VOID)
{
    //
    // Nothing to do on non-ACPI
    //
    return;
}

VOID
NTAPI
HalpFlushTLB(VOID)
{
    ULONG_PTR Flags, Cr4;
    INT CpuInfo[4];
    ULONG_PTR PageDirectory;

    //
    // Disable interrupts
    //
    Flags = __readeflags();
    _disable();

    //
    // Get page table directory base
    //
    PageDirectory = __readcr3();

    //
    // Check for CPUID support
    //
    if (KeGetCurrentPrcb()->CpuID)
    {
        //
        // Check for global bit in CPU features
        //
        __cpuid(CpuInfo, 1);
        if (CpuInfo[3] & 0x2000)
        {
            //
            // Get current CR4 value
            //
            Cr4 = __readcr4();

            //
            // Disable global bit
            //
            __writecr4(Cr4 & ~CR4_PGE);

            //
            // Flush TLB and re-enable global bit
            //
            __writecr3(PageDirectory);
            __writecr4(Cr4);

            //
            // Restore interrupts
            //
            __writeeflags(Flags);
            return;
        }
    }

    //
    // Legacy: just flush TLB
    //
    __writecr3(PageDirectory);
    __writeeflags(Flags);
}

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
UCHAR
FASTCALL
HalSystemVectorDispatchEntry(IN ULONG Vector,
                             OUT PKINTERRUPT_ROUTINE **FlatDispatch,
                             OUT PKINTERRUPT_ROUTINE *NoConnection)
{
    //
    // Not implemented on x86
    //
    return 0;
}

/*
 * @implemented
 */
VOID
NTAPI
KeFlushWriteBuffer(VOID)
{
    //
    // Not implemented on x86
    //
    return;
}

#ifdef _M_IX86
/* x86 fastcall wrappers */

#undef KeRaiseIrql
/*
 * @implemented
 */
VOID
NTAPI
KeRaiseIrql(KIRQL NewIrql,
            PKIRQL OldIrql)
{
    /* Call the fastcall function */
    *OldIrql = KfRaiseIrql(NewIrql);
}

#undef KeLowerIrql
/*
 * @implemented
 */
VOID
NTAPI
KeLowerIrql(KIRQL NewIrql)
{
    /* Call the fastcall function */
    KfLowerIrql(NewIrql);
}

#undef KeAcquireSpinLock
/*
 * @implemented
 */
VOID
NTAPI
KeAcquireSpinLock(PKSPIN_LOCK SpinLock,
                  PKIRQL OldIrql)
{
    /* Call the fastcall function */
    *OldIrql = KfAcquireSpinLock(SpinLock);
}

#undef KeReleaseSpinLock
/*
 * @implemented
 */
VOID
NTAPI
KeReleaseSpinLock(PKSPIN_LOCK SpinLock,
                  KIRQL NewIrql)
{
    /* Call the fastcall function */
    KfReleaseSpinLock(SpinLock, NewIrql);
}

#endif /* _M_IX86 */

/**
 * @brief
 * Mints a GSIV for an interrupt that a secondary controller demultiplexes.
 *
 * The caller is whoever knows a firmware descriptor names a pin rather than a
 * line: on this system the ACPI driver, on behalf of the resource hub, when it
 * translates a GpioInt.
 *
 * @param[in] OwnerName
 * Name of the descriptor the GSIV is for. Diagnostics only, and optional.
 *
 * @param[in] OwnerNameLength
 * Length of that name in bytes.
 *
 * @param[out] Gsiv
 * Receives the allocated number.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES once the range is spent.
 */
NTSTATUS
NTAPI
HalpAllocateGsivForSecondaryInterrupt(
    _In_reads_bytes_(OwnerNameLength) PCCHAR OwnerName,
    _In_ USHORT OwnerNameLength,
    _Out_ PULONG Gsiv)
{
    LONG Allocated;

    UNREFERENCED_PARAMETER(OwnerName);
    UNREFERENCED_PARAMETER(OwnerNameLength);

    Allocated = InterlockedIncrement(&HalpNextSecondaryGsiv) - 1;
    if (Allocated >= HALP_SECONDARY_GSIV_LIMIT)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Gsiv = (ULONG)Allocated;
    return STATUS_SUCCESS;
}
