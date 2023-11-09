
/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/


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
