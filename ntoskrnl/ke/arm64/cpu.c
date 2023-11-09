
/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
KeContextToTrapFrame(IN PCONTEXT Context,
                     IN OUT PKEXCEPTION_FRAME ExceptionFrame,
                     IN OUT PKTRAP_FRAME TrapFrame,
                     IN ULONG ContextFlags,
                     IN KPROCESSOR_MODE PreviousMode)
{
}

VOID
NTAPI
KiSaveProcessorControlState(OUT PKPROCESSOR_STATE ProcessorState)
{
    //
    // Save some critical stuff we use
    //
    __debugbreak();
#if 0
    ProcessorState->SpecialRegisters.ControlRegister = KeArmControlRegisterGet();
    ProcessorState->SpecialRegisters.LockdownRegister = KeArmLockdownRegisterGet();
    ProcessorState->SpecialRegisters.CacheRegister = KeArmCacheRegisterGet();
    ProcessorState->SpecialRegisters.StatusRegister = KeArmStatusRegisterGet();
#endif
}

VOID
NTAPI
KiRestoreProcessorControlState(PKPROCESSOR_STATE ProcessorState)
{
    __debugbreak();
#if 0
    KeArmControlRegisterSet(ProcessorState->SpecialRegisters.ControlRegister);
    KeArmLockdownRegisterSet(ProcessorState->SpecialRegisters.LockdownRegister);
    KeArmCacheRegisterSet(ProcessorState->SpecialRegisters.CacheRegister);
    KeArmStatusRegisterSet(ProcessorState->SpecialRegisters.StatusRegister);
#endif
}


ULONG
NTAPI
KeGetRecommendedSharedDataAlignment(VOID)
{
    /* Return the global variable */
    return 0;
}

VOID
NTAPI
KeFlushIoBuffers(
    _In_ PMDL Mdl,
    _In_ BOOLEAN ReadOperation,
    _In_ BOOLEAN DmaOperation)
{
    DbgBreakPoint();
}


BOOLEAN
NTAPI
KeInvalidateAllCaches(VOID)
{
    return FALSE;
}

VOID
NTAPI
KeFlushEntireTb(IN BOOLEAN Invalid,
                IN BOOLEAN AllProcessors)
{
    for(;;)
    {

    }
}

VOID
NTAPI
KeFlushCurrentTb(VOID)
{
    for(;;)
    {

    }
}

VOID
__cdecl
KeSaveStateForHibernate(IN PKPROCESSOR_STATE State)
{
}

KAFFINITY
NTAPI
KeQueryActiveProcessors(VOID)
{
    PAGED_CODE();

    /* Simply return the number of active processors */
    return KeActiveProcessors;
}
