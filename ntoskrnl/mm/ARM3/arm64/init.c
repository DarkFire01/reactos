
/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

PVOID MmNonPagedSystemStart;
PVOID MmNonPagedPoolStart;
PVOID MmNonPagedPoolExpansionStart;
PVOID MmPagedPoolEnd;
PVOID MiSessionSpaceEnd;
PVOID MiSessionImageEnd;
PVOID MiSessionImageStart;
PVOID MiSessionViewStart;
PVOID MiSessionPoolEnd;
PVOID MiSessionPoolStart;
PVOID MmSessionBase;
PVOID MiSystemViewStart;
PFN_NUMBER MmSystemPageDirectory[PPE_PER_PAGE];
PMMPDE MmSystemPagePtes;
ULONG MmNumberOfSystemPtes;
RTL_BITMAP MiPfnBitMap;
PPHYSICAL_MEMORY_DESCRIPTOR MmPhysicalMemoryBlock;
PMEMORY_ALLOCATION_DESCRIPTOR MxFreeDescriptor;
MEMORY_ALLOCATION_DESCRIPTOR MxOldFreeDescriptor;
ULONG MmNumberOfPhysicalPages;
PVOID MmHighestUserAddress;
PVOID MmSystemCacheStart;
PVOID MmSystemCacheEnd;
MMSUPPORT MmSystemCacheWs;
PVOID MmHyperSpaceEnd;

/* PRIVATE FUNCTIONS **********************************************************/

CODE_SEG("INIT")
NTSTATUS
NTAPI
MiInitMachineDependent(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    //
    // Always return success for now
    //
    UNIMPLEMENTED_FATAL("LINK START!\n");
    return STATUS_SUCCESS;
}

/* EOF */
