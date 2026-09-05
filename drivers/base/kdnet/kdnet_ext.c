/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kdnet extensibility initialization
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * The import table handed to a KDNET extensibility module.
 *
 * An extension such as kd_02_8086 owns one NIC and nothing else: it has no
 * imports of its own, and reaches the machine only through the table built
 * here. Every entry is a thin wrapper rather than the kernel routine itself,
 * because several of the KDNET slots do not match the routine ReactOS has for
 * them, and calling through a mismatched function pointer corrupts the stack.
 */

#include "kdnet.h"

#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>

/* For _vsnprintf, used to forward an extension's trace line */
#include <stdio.h>

PKDNET_EXTENSIBILITY_EXPORTS KdNetExtensibilityExports = NULL;

static NTSTATUS KdNetErrorStatus = STATUS_SUCCESS;

/* Read by kdnet_init.c to report why an extension's KdInitializeController failed */
PWCHAR KdNetErrorString = NULL;
ULONG KdNetHardwareId = 0;

/* Register and port traffic is far too noisy to leave on; switch it here */
#define KDNET_PORTLOG(fmt, ...)

/* Physical address lookups are logged, but only the first few */
#define KDNET_ADDRESSLOG_MAX 120
static ULONG KdNetAddressLogCount = 0;

static struct _DEBUG_DEVICE_DESCRIPTOR *KdNetDevice = NULL;

/* MEMORY-MAPPED REGISTER ACCESS **********************************************/

static
UCHAR
NTAPI
KdNetReadRegisterUChar(
    _In_ PUCHAR Register)
{
    UCHAR Value = READ_REGISTER_UCHAR(Register);

    KDNET_PORTLOG("kdnet: Rr8 [%p]=0x%02x\n", Register, Value);
    return Value;
}

static
USHORT
NTAPI
KdNetReadRegisterUShort(
    _In_ PUSHORT Register)
{
    USHORT Value = READ_REGISTER_USHORT(Register);

    KDNET_PORTLOG("kdnet: Rr16 [%p]=0x%04x\n", Register, Value);
    return Value;
}

static
ULONG
NTAPI
KdNetReadRegisterULong(
    _In_ PULONG Register)
{
    ULONG Value = READ_REGISTER_ULONG(Register);

    KDNET_PORTLOG("kdnet: Rr32 [%p]=0x%08lx\n", Register, Value);
    return Value;
}

/**
 * @brief
 * Reads a 64-bit memory-mapped register as two 32-bit halves.
 *
 * Neither this tree nor the x86 instruction set has a 64-bit MMIO access, so
 * the halves are read separately, low first. That is what a driver does with a
 * 64-bit BAR-relative register on this architecture, and it is what the
 * extensions actually use these slots for: the descriptor ring base addresses,
 * which the NIC does not latch until the high half is written.
 *
 * @param[in] Register
 * The register to read.
 *
 * @return
 * The register's value.
 */
static
ULONG64
NTAPI
KdNetReadRegisterULong64(
    _In_ ULONG64 *Register)
{
    PULONG Halves = (PULONG)Register;
    ULONG Low, High;

    Low = READ_REGISTER_ULONG(&Halves[0]);
    High = READ_REGISTER_ULONG(&Halves[1]);

    return ((ULONG64)High << 32) | Low;
}

static
VOID
NTAPI
KdNetWriteRegisterUChar(
    _In_ PUCHAR Register,
    _In_ UCHAR Value)
{
    KDNET_PORTLOG("kdnet: Wr8 [%p]<-0x%02x\n", Register, Value);
    WRITE_REGISTER_UCHAR(Register, Value);
}

static
VOID
NTAPI
KdNetWriteRegisterUShort(
    _In_ PUSHORT Register,
    _In_ USHORT Value)
{
    KDNET_PORTLOG("kdnet: Wr16 [%p]<-0x%04x\n", Register, Value);
    WRITE_REGISTER_USHORT(Register, Value);
}

static
VOID
NTAPI
KdNetWriteRegisterULong(
    _In_ PULONG Register,
    _In_ ULONG Value)
{
    KDNET_PORTLOG("kdnet: Wr32 [%p]<-0x%08lx\n", Register, Value);
    WRITE_REGISTER_ULONG(Register, Value);
}

/**
 * @brief
 * Writes a 64-bit memory-mapped register as two 32-bit halves, low first.
 *
 * The order matters. A NIC that takes a 64-bit ring base in two halves acts on
 * the write to the high half, so the low half has to be in place first.
 *
 * @param[in] Register
 * The register to write.
 *
 * @param[in] Value
 * The value to write.
 */
static
VOID
NTAPI
KdNetWriteRegisterULong64(
    _In_ ULONG64 *Register,
    _In_ ULONG64 Value)
{
    PULONG Halves = (PULONG)Register;

    WRITE_REGISTER_ULONG(&Halves[0], (ULONG)Value);
    WRITE_REGISTER_ULONG(&Halves[1], (ULONG)(Value >> 32));
}

/* PORT ACCESS ****************************************************************/

static
UCHAR
NTAPI
KdNetReadPortUChar(
    _In_ PUCHAR Port)
{
    UCHAR Value = READ_PORT_UCHAR(Port);

    KDNET_PORTLOG("kdnet: Rp8 [%p]=0x%02x\n", Port, Value);
    return Value;
}

static
USHORT
NTAPI
KdNetReadPortUShort(
    _In_ PUSHORT Port)
{
    USHORT Value = READ_PORT_USHORT(Port);

    KDNET_PORTLOG("kdnet: Rp16 [%p]=0x%04x\n", Port, Value);
    return Value;
}

static
ULONG
NTAPI
KdNetReadPortULong(
    _In_ PULONG Port)
{
    ULONG Value = READ_PORT_ULONG(Port);

    KDNET_PORTLOG("kdnet: Rp32 [%p]=0x%08lx\n", Port, Value);
    return Value;
}

/**
 * @brief
 * Stands in for a 64-bit port read, which x86 does not have.
 *
 * The widest form of IN is 32 bits, so there is nothing to issue here. The slot
 * still has to hold a correctly typed routine: an extension that calls it must
 * get a defined answer rather than a branch through a pointer of another shape.
 *
 * @param[in] Port
 * The port that was to be read. Unused.
 *
 * @return
 * Zero, always.
 */
static
ULONG64
NTAPI
KdNetReadPortULong64(
    _In_ ULONG64 *Port)
{
    UNREFERENCED_PARAMETER(Port);

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: 64-bit port reads do not exist on this architecture\n");

    return 0;
}

static
VOID
NTAPI
KdNetWritePortUChar(
    _In_ PUCHAR Port,
    _In_ UCHAR Value)
{
    KDNET_PORTLOG("kdnet: Wp8 [%p]<-0x%02x\n", Port, Value);
    WRITE_PORT_UCHAR(Port, Value);
}

static
VOID
NTAPI
KdNetWritePortUShort(
    _In_ PUSHORT Port,
    _In_ USHORT Value)
{
    KDNET_PORTLOG("kdnet: Wp16 [%p]<-0x%04x\n", Port, Value);
    WRITE_PORT_USHORT(Port, Value);
}

static
VOID
NTAPI
KdNetWritePortULong(
    _In_ PULONG Port,
    _In_ ULONG Value)
{
    KDNET_PORTLOG("kdnet: Wp32 [%p]<-0x%08lx\n", Port, Value);
    WRITE_PORT_ULONG(Port, Value);
}

/**
 * @brief
 * Stands in for a 64-bit port write, which x86 does not have.
 *
 * @param[in] Port
 * The port that was to be written. Unused.
 *
 * @param[in] Value
 * The value that was to be written. Unused.
 */
static
VOID
NTAPI
KdNetWritePortULong64(
    _In_ PULONG Port,
    _In_ ULONG64 Value)
{
    UNREFERENCED_PARAMETER(Port);
    UNREFERENCED_PARAMETER(Value);

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: 64-bit port writes do not exist on this architecture\n");
}

/* TIMING *********************************************************************/

/**
 * @brief
 * Busy-waits for a number of microseconds.
 *
 * KeStallExecutionProcessor cannot be used here. When KdInitializeController
 * runs we are inside KdInitSystem(0), called from KiSystemStartup before the
 * HAL calibrates KeGetPcr()->StallScaleFactor. The factor is still
 * INITIAL_STALL_COUNT, so that routine under-delays by roughly a thousandfold
 * and an extension's multi-second auto-negotiation wait expires in a few
 * milliseconds. The architecture layer's cycle counter is calibrated against
 * an independent hardware timer and does not have that problem.
 *
 * @param[in] Microseconds
 * How long to wait.
 */
static
VOID
NTAPI
KdNetStallExecutionProcessor(
    _In_ ULONG Microseconds)
{
    ULONG64 Target;

    Target = KdNetReadTimeStampCounter() +
             (ULONG64)Microseconds * KdNetGetTicksPerMicrosecond();

    while (KdNetReadTimeStampCounter() < Target)
        YieldProcessor();
}

static
ULONG64
NTAPI
KdNetReadCycleCounter(
    _Out_opt_ ULONG64 *Frequency)
{
    if (Frequency != NULL)
        *Frequency = KdNetGetTicksPerMicrosecond() * 1000000ULL;

    return KdNetReadTimeStampCounter();
}

/* PCI CONFIGURATION SPACE ****************************************************/

static
ULONG
NTAPI
KdNetGetPciDataByOffset(
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Out_ PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    return KdGetPciDataByOffset(BusNumber, SlotNumber, Buffer, Offset, Length);
}

static
ULONG
NTAPI
KdNetSetPciDataByOffset(
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_ PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    return KdSetPciDataByOffset(BusNumber, SlotNumber, Buffer, Offset, Length);
}

/* MEMORY *********************************************************************/

/*
 * The KDNET ABI passes a third BOOLEAN FlushCurrentTLB argument that the HAL
 * routines this tree has do not take. Assigning the two-argument HAL routine
 * straight into the three-argument slot leaves the caller pushing three
 * arguments and the callee popping two, which unbalances the stack and hangs
 * the machine. These wrappers present the wider ABI and drop the extra argument.
 */

static
PVOID
NTAPI
KdNetMapPhysicalMemory64(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ ULONG NumberPages,
    _In_ BOOLEAN FlushCurrentTLB)
{
    PVOID VirtualAddress;

    UNREFERENCED_PARAMETER(FlushCurrentTLB);

    VirtualAddress = KdMapPhysicalMemory64(PhysicalAddress, NumberPages);
    if (FrLdrDbgPrint)
    {
        FrLdrDbgPrint("kdnet: MapPhys64 pa=0x%08lx%08lx pages=%lu -> %p\n",
                      PhysicalAddress.HighPart, PhysicalAddress.LowPart,
                      NumberPages, VirtualAddress);
    }

    return VirtualAddress;
}

static
VOID
NTAPI
KdNetUnmapVirtualAddress(
    _In_ PVOID VirtualAddress,
    _In_ ULONG NumberPages,
    _In_ BOOLEAN FlushCurrentTLB)
{
    UNREFERENCED_PARAMETER(FlushCurrentTLB);

    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: Unmap %p pages=%lu\n", VirtualAddress, NumberPages);

    KdUnmapVirtualAddress(VirtualAddress, NumberPages);
}

/**
 * @brief
 * Resolves a virtual address the extension holds back to a physical one.
 *
 * An address inside the debug device's own BAR window is translated from the
 * window itself. MmGetPhysicalAddress would answer for it too, but only once
 * the memory manager is running, and this is called long before that.
 *
 * @param[in] VirtualAddress
 * The address to translate.
 *
 * @return
 * The physical address it maps to.
 */
static
PHYSICAL_ADDRESS
NTAPI
KdNetGetPhysicalAddress(
    _In_ PVOID VirtualAddress)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    PUCHAR WindowBase;

    if (KdNetDevice != NULL && KdNetDevice->Memory.VirtualAddress != NULL)
    {
        WindowBase = (PUCHAR)KdNetDevice->Memory.VirtualAddress;

        if ((PUCHAR)VirtualAddress >= WindowBase &&
            (PUCHAR)VirtualAddress < WindowBase + KdNetDevice->Memory.Length)
        {
            PhysicalAddress.QuadPart = KdNetDevice->Memory.Start.QuadPart +
                                       ((PUCHAR)VirtualAddress - WindowBase);

            if (FrLdrDbgPrint && KdNetAddressLogCount < KDNET_ADDRESSLOG_MAX)
            {
                FrLdrDbgPrint("kdnet: GetPA bar %p -> 0x%08lx%08lx\n", VirtualAddress,
                              PhysicalAddress.HighPart, PhysicalAddress.LowPart);
                KdNetAddressLogCount++;
            }

            return PhysicalAddress;
        }
    }

    PhysicalAddress = MmGetPhysicalAddress(VirtualAddress);
    if (FrLdrDbgPrint && KdNetAddressLogCount < KDNET_ADDRESSLOG_MAX)
    {
        FrLdrDbgPrint("kdnet: GetPA mm %p -> 0x%08lx%08lx\n", VirtualAddress,
                      PhysicalAddress.HighPart, PhysicalAddress.LowPart);
        KdNetAddressLogCount++;
    }

    return PhysicalAddress;
}

/* DIAGNOSTICS ****************************************************************/

/**
 * @brief
 * Carries an extension's trace line to the early boot debug output.
 *
 * FrLdrDbgPrint returns a count and takes a const format, so it does not have
 * the shape of this slot. Casting it into place would call it through a pointer
 * of the wrong type, so it is wrapped instead.
 *
 * @param[in] Format
 * A printf-style format string.
 */
static
VOID
NTAPI
KdNetDbgPrintf(
    _In_ PCHAR Format,
    ...)
{
    va_list Arguments;
    CHAR Line[256];

    if (!FrLdrDbgPrint)
        return;

    va_start(Arguments, Format);
    _vsnprintf(Line, sizeof(Line) - 1, Format, Arguments);
    va_end(Arguments);

    Line[sizeof(Line) - 1] = '\0';
    FrLdrDbgPrint("%s", Line);
}

/* INITIALIZATION *************************************************************/

/**
 * @brief
 * Builds the import table and hands it to the extensibility module.
 *
 * @param[in] LoaderOptions
 * The boot option string, passed through to the extension.
 *
 * @param[in,out] Device
 * The debug device the extension is to drive.
 *
 * @param[in] KdInitializeLibrary
 * The extension's entry point, resolved from its export table.
 *
 * @param[out] ExtensibilityExports
 * Receives the routines the extension publishes back.
 *
 * @param[out] SerialExtensibility
 * Unused. Serial extensions are a separate KDNET flavour.
 *
 * @return
 * STATUS_SUCCESS, or the extension's own failure status.
 */
NTSTATUS
KdNetInitializeExtensibility(
    _In_opt_ PCHAR LoaderOptions,
    _Inout_ struct _DEBUG_DEVICE_DESCRIPTOR *Device,
    _In_opt_ PKDNET_INITIALIZE_LIBRARY KdInitializeLibrary,
    _Out_ PKDNET_EXTENSIBILITY_EXPORTS ExtensibilityExports,
    _Out_opt_ PVOID SerialExtensibility)
{
    /*
     * Static because the extension keeps the pointer: KdInitializeLibrary
     * stores the table and calls back through it for the life of the session.
     */
    static KDNET_EXTENSIBILITY_IMPORTS Imports;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(SerialExtensibility);

    if (ExtensibilityExports == NULL)
        return STATUS_INVALID_PARAMETER;

    if (KdInitializeLibrary == NULL)
        return STATUS_PROCEDURE_NOT_FOUND;

    RtlZeroMemory(ExtensibilityExports, sizeof(*ExtensibilityExports));
    ExtensibilityExports->FunctionCount = KDNET_EXT_EXPORTS;

    RtlZeroMemory(&Imports, sizeof(Imports));
    Imports.FunctionCount = KDNET_EXT_IMPORTS;
    Imports.Exports = ExtensibilityExports;

    Imports.GetPciDataByOffset = KdNetGetPciDataByOffset;
    Imports.SetPciDataByOffset = KdNetSetPciDataByOffset;
    Imports.MapPhysicalMemory64 = KdNetMapPhysicalMemory64;
    Imports.UnmapVirtualAddress = KdNetUnmapVirtualAddress;

    KdNetDevice = Device;
    Imports.GetPhysicalAddress = KdNetGetPhysicalAddress;
    Imports.StallExecutionProcessor = KdNetStallExecutionProcessor;

    Imports.ReadRegisterUChar = KdNetReadRegisterUChar;
    Imports.ReadRegisterUShort = KdNetReadRegisterUShort;
    Imports.ReadRegisterULong = KdNetReadRegisterULong;
    Imports.ReadRegisterULong64 = KdNetReadRegisterULong64;
    Imports.WriteRegisterUChar = KdNetWriteRegisterUChar;
    Imports.WriteRegisterUShort = KdNetWriteRegisterUShort;
    Imports.WriteRegisterULong = KdNetWriteRegisterULong;
    Imports.WriteRegisterULong64 = KdNetWriteRegisterULong64;

    Imports.ReadPortUChar = KdNetReadPortUChar;
    Imports.ReadPortUShort = KdNetReadPortUShort;
    Imports.ReadPortULong = KdNetReadPortULong;
    Imports.ReadPortULong64 = KdNetReadPortULong64;
    Imports.WritePortUChar = KdNetWritePortUChar;
    Imports.WritePortUShort = KdNetWritePortUShort;
    Imports.WritePortULong = KdNetWritePortULong;
    Imports.WritePortULong64 = KdNetWritePortULong64;

    Imports.SetHiberRange = PoSetHiberRange;
    Imports.BugCheckEx = KeBugCheckEx;
    Imports.ReadCycleCounter = KdNetReadCycleCounter;
    Imports.KdNetDbgPrintf = KdNetDbgPrintf;

    Imports.KdNetErrorStatus = &KdNetErrorStatus;
    Imports.KdNetErrorString = &KdNetErrorString;
    Imports.KdNetHardwareID = &KdNetHardwareId;

    Status = KdInitializeLibrary(&Imports, LoaderOptions, Device);
    if (FrLdrDbgPrint)
    {
        FrLdrDbgPrint("kdnet: Ext KdInitializeLibrary=%p -> 0x%08lx\n",
                      KdInitializeLibrary, Status);
    }

    if (!NT_SUCCESS(Status))
        return Status;

    KdNetExtensibilityExports = ExtensibilityExports;
    return STATUS_SUCCESS;
}
