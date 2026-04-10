/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/kd64/arm64/kdarm64.c
 * PURPOSE:         KD support routines for ARM64
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
KdpGetStateChange(IN PDBGKD_MANIPULATE_STATE64 State,
                  IN PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);

    if (!NT_SUCCESS(State->u.Continue2.ContinueStatus))
        return;

    if (State->u.Continue2.ControlSet.CurrentSymbolStart != 1)
    {
        KdpCurrentSymbolStart = State->u.Continue2.ControlSet.CurrentSymbolStart;
        KdpCurrentSymbolEnd = State->u.Continue2.ControlSet.CurrentSymbolEnd;
    }
}

VOID
NTAPI
KdpSetContextState(IN PDBGKD_ANY_WAIT_STATE_CHANGE WaitStateChange,
                   IN PCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);

    /* ARM64 has BVR/WVR plus instruction stream/count in its control report. */
    WaitStateChange->ControlReport.Bvr = 0;
    WaitStateChange->ControlReport.Wvr = 0;
}

NTSTATUS
NTAPI
KdpSysReadMsr(
    _In_ ULONG Msr,
    _Out_ PULONGLONG MsrValue)
{
    UNREFERENCED_PARAMETER(Msr);
    UNREFERENCED_PARAMETER(MsrValue);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
KdpSysWriteMsr(
    _In_ ULONG Msr,
    _In_ PULONGLONG MsrValue)
{
    UNREFERENCED_PARAMETER(Msr);
    UNREFERENCED_PARAMETER(MsrValue);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
KdpSysReadBusData(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ActualLength)
{
    *ActualLength = HalGetBusDataByOffset(BusDataType,
                                          BusNumber,
                                          SlotNumber,
                                          Buffer,
                                          Offset,
                                          Length);

    return (*ActualLength != 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysWriteBusData(
    _In_ BUS_DATA_TYPE BusDataType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ActualLength)
{
    *ActualLength = HalSetBusDataByOffset(BusDataType,
                                          BusNumber,
                                          SlotNumber,
                                          Buffer,
                                          Offset,
                                          Length);

    return (*ActualLength != 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysReadControlSpace(
    _In_ ULONG Processor,
    _In_ ULONG64 BaseAddress,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ActualLength)
{
    PVOID ControlStart;

    if ((Processor >= KeNumberProcessors) ||
        ((BaseAddress + Length) > sizeof(KPROCESSOR_STATE)))
    {
        *ActualLength = 0;
        return STATUS_UNSUCCESSFUL;
    }

    ControlStart = (PVOID)((ULONG_PTR)&KiProcessorBlock[Processor]->ProcessorState +
                           (ULONG_PTR)BaseAddress);

    return KdpCopyMemoryChunks((ULONG_PTR)ControlStart,
                               Buffer,
                               Length,
                               0,
                               MMDBG_COPY_UNSAFE | MMDBG_COPY_WRITE,
                               ActualLength);
}

NTSTATUS
NTAPI
KdpSysWriteControlSpace(
    _In_ ULONG Processor,
    _In_ ULONG64 BaseAddress,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ActualLength)
{
    PVOID ControlStart;

    if ((Processor >= KeNumberProcessors) ||
        ((BaseAddress + Length) > sizeof(KPROCESSOR_STATE)))
    {
        *ActualLength = 0;
        return STATUS_UNSUCCESSFUL;
    }

    ControlStart = (PVOID)((ULONG_PTR)&KiProcessorBlock[Processor]->ProcessorState +
                           (ULONG_PTR)BaseAddress);

    return KdpCopyMemoryChunks((ULONG_PTR)Buffer,
                               ControlStart,
                               Length,
                               0,
                               MMDBG_COPY_UNSAFE,
                               ActualLength);
}

NTSTATUS
NTAPI
KdpSysReadIoSpace(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ ULONG AddressSpace,
    _In_ ULONG64 IoAddress,
    _Out_writes_bytes_(DataSize) PVOID DataValue,
    _In_ ULONG DataSize,
    _Out_ PULONG ActualDataSize)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(AddressSpace);
    UNREFERENCED_PARAMETER(IoAddress);
    UNREFERENCED_PARAMETER(DataValue);
    UNREFERENCED_PARAMETER(DataSize);

    *ActualDataSize = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
KdpSysWriteIoSpace(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ ULONG AddressSpace,
    _In_ ULONG64 IoAddress,
    _In_reads_bytes_(DataSize) PVOID DataValue,
    _In_ ULONG DataSize,
    _Out_ PULONG ActualDataSize)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(AddressSpace);
    UNREFERENCED_PARAMETER(IoAddress);
    UNREFERENCED_PARAMETER(DataValue);
    UNREFERENCED_PARAMETER(DataSize);

    *ActualDataSize = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
KdpSysCheckLowMemory(IN ULONG Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdpAllowDisable(VOID)
{
    return STATUS_SUCCESS;
}
