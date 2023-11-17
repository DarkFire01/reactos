
/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

ULONG KiDmaIoCoherency = 0;
ULONG ProcessCount;

VOID
NTAPI
KiInitializeUserApc(IN PKEXCEPTION_FRAME ExceptionFrame,
                    IN PKTRAP_FRAME TrapFrame,
                    IN PKNORMAL_ROUTINE NormalRoutine,
                    IN PVOID NormalContext,
                    IN PVOID SystemArgument1,
                    IN PVOID SystemArgument2)
{
}

NTSTATUS
NTAPI
KiCallUserMode(
    IN PVOID *OutputBuffer,
    IN PULONG OutputLength)
{
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
NtCallbackReturn(
    _In_ PVOID Result,
    _In_ ULONG ResultLength,
    _In_ NTSTATUS CallbackStatus)
{
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

CODE_SEG("INIT")
VOID
NTAPI
KiInitMachineDependent(VOID)
{
    /* There is nothing to do on ARM */
    return;
}

NTSTATUS
NTAPI
NtSetLdtEntries(IN ULONG Selector1,
                IN LDT_ENTRY LdtEntry1,
                IN ULONG Selector2,
                IN LDT_ENTRY LdtEntry2)
{
    //
    // Does not exist on ARM
    //
    return STATUS_NOT_IMPLEMENTED;
}

VOID
NTAPI
KiDispatchException(IN PEXCEPTION_RECORD ExceptionRecord,
                    IN PKEXCEPTION_FRAME ExceptionFrame,
                    IN PKTRAP_FRAME TrapFrame,
                    IN KPROCESSOR_MODE PreviousMode,
                    IN BOOLEAN FirstChance)
{
}


NTSTATUS
NTAPI
NtVdmControl(IN ULONG ControlCode,
             IN PVOID ControlData)
{
    /* Not supported */
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
KeUserModeCallback(
    IN ULONG RoutineIndex,
    IN PVOID Argument,
    IN ULONG ArgumentLength,
    OUT PVOID *Result,
    OUT PULONG ResultLength)
{
    return 1;
}


VOID
NTAPI
KeSetDmaIoCoherency(IN ULONG Coherency)
{
    /* Save the coherency globally */
    KiDmaIoCoherency = Coherency;
}


NTSTATUS
NTAPI
KeRaiseUserException(IN NTSTATUS ExceptionCode)
{
    UNIMPLEMENTED;
    return STATUS_UNSUCCESSFUL;
}


/* threads ****************/
CODE_SEG("INIT")
VOID
NTAPI
KiInitializeContextThread(IN PKTHREAD Thread,
                          IN PKSYSTEM_ROUTINE SystemRoutine,
                          IN PKSTART_ROUTINE StartRoutine,
                          IN PVOID StartContext,
                          IN PCONTEXT ContextPointer)
{
    DPRINT1("KiInitializeContextThread: For ARM64 WIP\n");
}

