/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kdnet extensibility initialization
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"

PKDNET_EXTENSIBILITY_EXPORTS KdNetExtensibilityExports = NULL;

#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>

static NTSTATUS g_KdNetErrorStatus = STATUS_SUCCESS;
/* Non-static: kdnet_init.c reads these to report why an extension's
 * KdInitializeController failed. */
PWCHAR   g_KdNetErrorString = NULL;
ULONG    g_KdNetHardwareId  = 0;

#define KDNET_PORTLOG(fmt, ...) //FrLdrDbgPrint(fmt, __VA_ARGS__);

static ULONG g_KdNetRegLog = 0;
#define KDNET_REGLOG_MAX 120

static UCHAR  NTAPI KdNetRrUchar(PUCHAR r)            { KDNET_PORTLOG("kdnet: Rr8  [%p]=0x%02x\n", r, READ_REGISTER_UCHAR(r)); return READ_REGISTER_UCHAR(r); }
static USHORT NTAPI KdNetRrUshort(PUSHORT r)          { KDNET_PORTLOG("kdnet: Rr16 [%p]=0x%04x\n", r, READ_REGISTER_USHORT(r)); return READ_REGISTER_USHORT(r); }
static ULONG  NTAPI KdNetRrUlong(PULONG r)
{
    ULONG v = READ_REGISTER_ULONG(r);
    KDNET_PORTLOG("kdnet: Rr32 [%p]=0x%08lx\n", r, v);
    return v;
}
static VOID   NTAPI KdNetWrUchar(PUCHAR r, UCHAR v)   { KDNET_PORTLOG("kdnet: Wr8  [%p]<-0x%02x\n", r, v); WRITE_REGISTER_UCHAR(r, v); }
static VOID   NTAPI KdNetWrUshort(PUSHORT r, USHORT v){ KDNET_PORTLOG("kdnet: Wr16 [%p]<-0x%04x\n", r, v); WRITE_REGISTER_USHORT(r, v); }
static VOID   NTAPI KdNetWrUlong(PULONG r, ULONG v)
{
    KDNET_PORTLOG("kdnet: Wr32 [%p]<-0x%08lx\n", r, v);
    WRITE_REGISTER_ULONG(r, v);
}

static UCHAR  NTAPI KdNetRpUchar(PUCHAR p)            { UCHAR v = READ_PORT_UCHAR(p);  KDNET_PORTLOG("kdnet: RpU8  [%p]=0x%02x\n", p, v); return v; }
static USHORT NTAPI KdNetRpUshort(PUSHORT p)          { USHORT v = READ_PORT_USHORT(p); KDNET_PORTLOG("kdnet: RpU16 [%p]=0x%04x\n", p, v); return v; }
static ULONG  NTAPI KdNetRpUlong(PULONG p)            { ULONG v = READ_PORT_ULONG(p);  KDNET_PORTLOG("kdnet: RpU32 [%p]=0x%08lx\n", p, v); return v; }
static VOID   NTAPI KdNetWpUchar(PUCHAR p, UCHAR v)   { KDNET_PORTLOG("kdnet: WpU8  [%p]<-0x%02x\n", p, v); WRITE_PORT_UCHAR(p, v); }
static VOID   NTAPI KdNetWpUshort(PUSHORT p, USHORT v){ KDNET_PORTLOG("kdnet: WpU16 [%p]<-0x%04x\n", p, v); WRITE_PORT_USHORT(p, v); }
static VOID   NTAPI KdNetWpUlong(PULONG p, ULONG v)   { KDNET_PORTLOG("kdnet: WpU32 [%p]<-0x%08lx\n", p, v); WRITE_PORT_ULONG(p, v); }
static struct _DEBUG_DEVICE_DESCRIPTOR *g_KdNetDevice = NULL;
/*
 * Wall-clock timing for the kdnet init path comes from the architecture layer
 * (see the per-arch subfolders i386/, amd64/): KdNetReadTimeStampCounter() reads
 * a monotonic cycle counter and KdNetGetTicksPerMicrosecond() returns its rate,
 * calibrated once against an independent hardware timer.
 *
 * We MUST NOT use KeStallExecutionProcessor here. When KdInitializeController
 * runs we are inside KdInitSystem(0), called from KiSystemStartup
 * (ke/i386/kiinit.c) BEFORE the HAL calibrates KeGetPcr()->StallScaleFactor.
 * The factor is still INITIAL_STALL_COUNT (100), so KeStallExecutionProcessor
 * under-delays by ~1000x and the extension's ~3.5s auto-negotiation wait expires
 * in a few milliseconds ("NIC auto-negotiation timed out").
 */
static VOID NTAPI KdNetStall(ULONG us)
{
    ULONG64 ticksPerUs = KdNetGetTicksPerMicrosecond();
    ULONG64 target = KdNetReadTimeStampCounter() + (ULONG64)us * ticksPerUs;

    while (KdNetReadTimeStampCounter() < target)
        YieldProcessor();
}

static ULONG64 NTAPI KdNetReadCycleCounter(ULONG64 *Frequency)
{
    if (Frequency)
        *Frequency = KdNetGetTicksPerMicrosecond() * 1000000ULL;
    return KdNetReadTimeStampCounter();
}

static ULONG NTAPI KdNetGetPci(ULONG Bus, ULONG Slot, PVOID Buf, ULONG Off, ULONG Len)
{
    KDNET_PORTLOG("kdnet: GetPci slot=0x%lx off=0x%lx len=%lu\n", Slot, Off, Len);
    ULONG r = KdGetPciDataByOffset(Bus, Slot, Buf, Off, Len);
    ULONG v = 0;
    RtlCopyMemory(&v, Buf, (Len < 4) ? Len : 4);
    KDNET_PORTLOG("kdnet: GetPci slot=0x%lx off=0x%lx len=%lu -> ret=%lu val=0x%08lx\n",
                      Slot, Off, Len, r, v);
    return r;
}
static ULONG NTAPI KdNetSetPci(ULONG Bus, ULONG Slot, PVOID Buf, ULONG Off, ULONG Len)
{
    KDNET_PORTLOG("kdnet: SetPci slot=0x%lx off=0x%lx len=%lu\n", Slot, Off, Len);
    return KdSetPciDataByOffset(Bus, Slot, Buf, Off, Len);
}
VOID
NTAPI
KdStopExecution()
{
    FrLdrDbgPrint("Umplemented\n");
}
/*
 * MapPhysicalMemory64 / UnmapVirtualAddress signature adapters. The KDNET (Win8)
 * ABI passes a 3rd BOOLEAN FlushCurrentTLB arg, but ReactOS targets WS03 where
 * the HAL's KdMapPhysicalMemory64/KdUnmapVirtualAddress take only 2 args.
 * Assigning the 2-arg HAL fn straight into the 3-arg slot makes the stub's
 * stdcall push 3 args while the callee pops 2 -> stack corruption -> hang. These
 * wrappers expose the 3-arg ABI and call the 2-arg HAL routine.
 */
static PVOID NTAPI KdNetMapPhysicalMemory64(PHYSICAL_ADDRESS Pa, ULONG Pages, BOOLEAN Flush)
{
    PVOID va = KdMapPhysicalMemory64(Pa, Pages);
    UNREFERENCED_PARAMETER(Flush);
    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: MapPhys64 pa=0x%08lx%08lx pages=%lu -> %p\n",
                      Pa.HighPart, Pa.LowPart, Pages, va);
    return va;
}
static VOID NTAPI KdNetUnmapVirtualAddress(PVOID Va, ULONG Pages, BOOLEAN Flush)
{
    UNREFERENCED_PARAMETER(Flush);
    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: Unmap %p pages=%lu\n", Va, Pages);
    KdUnmapVirtualAddress(Va, Pages);
}

static PHYSICAL_ADDRESS NTAPI KdNetGetPhysicalAddress(PVOID Va)
{
    PHYSICAL_ADDRESS pa;
    if (g_KdNetDevice && g_KdNetDevice->Memory.VirtualAddress &&
        (PUCHAR)Va >= (PUCHAR)g_KdNetDevice->Memory.VirtualAddress &&
        (PUCHAR)Va < (PUCHAR)g_KdNetDevice->Memory.VirtualAddress + g_KdNetDevice->Memory.Length)
    {
        pa.QuadPart = g_KdNetDevice->Memory.Start.QuadPart +
                      ((PUCHAR)Va - (PUCHAR)g_KdNetDevice->Memory.VirtualAddress);
        if (FrLdrDbgPrint && g_KdNetRegLog < KDNET_REGLOG_MAX)
            { FrLdrDbgPrint("kdnet: GetPA ctx %p -> 0x%08lx%08lx\n", Va, pa.HighPart, pa.LowPart); g_KdNetRegLog++; }
        return pa;
    }
    pa = MmGetPhysicalAddress(Va);
    if (FrLdrDbgPrint && g_KdNetRegLog < KDNET_REGLOG_MAX)
        { FrLdrDbgPrint("kdnet: GetPA mm  %p -> 0x%08lx%08lx\n", Va, pa.HighPart, pa.LowPart); g_KdNetRegLog++; }
    return pa;
}


NTSTATUS
KdNetInitializeExtensibility(
    _In_opt_ PCHAR LoaderOptions,
    _Inout_ struct _DEBUG_DEVICE_DESCRIPTOR *Device,
    _In_opt_ PKDNET_INITIALIZE_LIBRARY KdInitializeLibrary,
    _Out_ PKDNET_EXTENSIBILITY_EXPORTS ExtensibilityExports,
    _Out_opt_ void *SerialExtensibility)
{
    NTSTATUS Status;
    PKDNET_INITIALIZE_LIBRARY InitLib;
    static KDNET_EXTENSIBILITY_IMPORTS Imports;

    if (!ExtensibilityExports)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(ExtensibilityExports, sizeof(*ExtensibilityExports));
    ExtensibilityExports->FunctionCount = KDNET_EXT_EXPORTS;

    RtlZeroMemory(&Imports, sizeof(Imports));
    Imports.FunctionCount = KDNET_EXT_IMPORTS;
    Imports.Exports = ExtensibilityExports;
    Imports.GetPciDataByOffset = KdNetGetPci;
    Imports.SetPciDataByOffset = KdNetSetPci;
    Imports.MapPhysicalMemory64 = KdNetMapPhysicalMemory64;
    Imports.UnmapVirtualAddress = KdNetUnmapVirtualAddress;

    g_KdNetDevice = Device;
    Imports.GetPhysicalAddress = (KDNET_GET_PHYSICAL_ADDRESS)KdNetGetPhysicalAddress;
    Imports.StallExecutionProcessor = KdNetStall;

    Imports.ReadRegisterUChar = KdNetRrUchar;
    Imports.ReadRegisterUShort = KdNetRrUshort;
    Imports.ReadRegisterULong = KdNetRrUlong;
    Imports.WriteRegisterUChar = KdNetWrUchar;
    Imports.WriteRegisterUShort = KdNetWrUshort;
    Imports.WriteRegisterULong = KdNetWrUlong;

    Imports.ReadPortUChar = KdNetRpUchar;
    Imports.ReadPortUShort = KdNetRpUshort;
    Imports.ReadPortULong = KdNetRpUlong;
    Imports.WritePortUChar = KdNetWpUchar;
    Imports.WritePortUShort = KdNetWpUshort;
    Imports.WritePortULong = KdNetWpUlong;

    Imports.SetHiberRange = (KDNET_SET_HIBER_RANGE)PoSetHiberRange;
    Imports.BugCheckEx = (KDNET_BUGCHECK_EX)KeBugCheckEx;

    /* TODO: */
    Imports.ReadRegisterULong64 = (KDNET_READ_REGISTER_ULONG64)KdStopExecution;
    Imports.WriteRegisterULong64 = (KDNET_WRITE_REGISTER_ULONG64)KdStopExecution;
    Imports.ReadPortULong64 = (KDNET_READ_PORT_ULONG64)KdStopExecution;
    Imports.WritePortULong64 = (KDNET_WRITE_PORT_ULONG64)KdStopExecution;
    Imports.ReadCycleCounter = KdNetReadCycleCounter;
    Imports.KdNetDbgPrintf = (KDNET_DBGPRINT)FrLdrDbgPrint; //HACK-ish:

    Imports.KdNetErrorStatus = &g_KdNetErrorStatus;
    Imports.KdNetErrorString = &g_KdNetErrorString;
    Imports.KdNetHardwareID = &g_KdNetHardwareId;

    InitLib = KdInitializeLibrary;
    if (!InitLib)
        return STATUS_PROCEDURE_NOT_FOUND;

    Status = InitLib(&Imports, LoaderOptions, Device);
    if (FrLdrDbgPrint)
        FrLdrDbgPrint("kdnet: Ext KdInitializeLibrary=%p -> 0x%08lx\n", InitLib, Status);
    if (!NT_SUCCESS(Status))
        return Status;

    KdNetExtensibilityExports = ExtensibilityExports;
    return STATUS_SUCCESS;
}
