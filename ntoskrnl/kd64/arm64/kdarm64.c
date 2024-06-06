
/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#undef UNIMPLEMENTED
#define UNIMPLEMENTED KdpDprintf("%s is unimplemented\n", __FUNCTION__)

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
KdpGetStateChange(IN PDBGKD_MANIPULATE_STATE64 State,
                  IN PCONTEXT Context)
{
    DbgPrintEarly("KdpGetStateChange: entry\n");
    for(;;)
    {

    }
}
VOID
NTAPI
KdpSetContextState(IN PDBGKD_ANY_WAIT_STATE_CHANGE WaitStateChange,
                   IN PCONTEXT Context)
{
    DbgPrintEarly("KdpSetContextState: entry\n");
    for(;;)
    {

    }
}

NTSTATUS
NTAPI
KdpSysReadMsr(IN ULONG Msr,
              OUT PLARGE_INTEGER MsrValue)
{
    DbgPrintEarly("KdpSysReadMsr: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysWriteMsr(IN ULONG Msr,
               IN PLARGE_INTEGER MsrValue)
{
    DbgPrintEarly("KdpSysWriteMsr: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysReadBusData(IN ULONG BusDataType,
                  IN ULONG BusNumber,
                  IN ULONG SlotNumber,
                  IN ULONG Offset,
                  IN PVOID Buffer,
                  IN ULONG Length,
                  OUT PULONG ActualLength)
{
    DbgPrintEarly("KdpSysReadBusData: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysWriteBusData(IN ULONG BusDataType,
                   IN ULONG BusNumber,
                   IN ULONG SlotNumber,
                   IN ULONG Offset,
                   IN PVOID Buffer,
                   IN ULONG Length,
                   OUT PULONG ActualLength)
{
    DbgPrintEarly("KdpSysWriteBusData: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysReadControlSpace(IN ULONG Processor,
                       IN ULONG64 BaseAddress,
                       IN PVOID Buffer,
                       IN ULONG Length,
                       OUT PULONG ActualLength)
{
    DbgPrintEarly("KdpSysReadControlSpace: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysWriteControlSpace(IN ULONG Processor,
                        IN ULONG64 BaseAddress,
                        IN PVOID Buffer,
                        IN ULONG Length,
                        OUT PULONG ActualLength)
{
    DbgPrintEarly("KdpSysWriteControlSpace: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysReadIoSpace(IN ULONG InterfaceType,
                  IN ULONG BusNumber,
                  IN ULONG AddressSpace,
                  IN ULONG64 IoAddress,
                  IN PVOID DataValue,
                  IN ULONG DataSize,
                  OUT PULONG ActualDataSize)
{
    DbgPrintEarly("KdpSysReadIoSpace: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysWriteIoSpace(IN ULONG InterfaceType,
                   IN ULONG BusNumber,
                   IN ULONG AddressSpace,
                   IN ULONG64 IoAddress,
                   IN PVOID DataValue,
                   IN ULONG DataSize,
                   OUT PULONG ActualDataSize)
{
    DbgPrintEarly("KdpSysWriteIoSpace: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpSysCheckLowMemory(IN ULONG Flags)
{
    DbgPrintEarly("KdpSysCheckLowMemory: entry\n");
    for(;;)
    {

    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
KdpAllowDisable(VOID)
{
    DbgPrintEarly("KdpAllowDisable: entry\n");
    for(;;)
    {

    }
    return STATUS_ACCESS_DENIED;
}
