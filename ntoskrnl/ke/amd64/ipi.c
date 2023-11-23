


#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
KiIpiGenericCallTarget(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID BroadcastFunction,
    _In_ PVOID Argument,
    _In_ PULONG Count)
{
    __debugbreak();
}

VOID
NTAPI
KiIpiSendPacket(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
    _In_ ULONG_PTR Context,
    _In_ PULONG Count)
{
    __debugbreak();
}

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    /* Check if we can send the IPI directly */
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(TargetSet, 0x1F);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, 0x2F);
    }
    else
    {
        KiIpiSendPacket(TargetSet,
                        KiIpiGenericCallTarget,
                        NULL,
                        IpiRequest,
                        0);
    }
}
