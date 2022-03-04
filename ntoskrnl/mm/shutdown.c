/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/mm/shutdown.c
 * PURPOSE:         Memory Manager Shutdown
 * PROGRAMMERS:
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "ARM3/miarm.h"

/* PRIVATE FUNCTIONS *********************************************************/

VOID
MiShutdownSystem(VOID)
{
    ULONG i;
    PFN_NUMBER Page;

    /* Loop through all the paging files */
    for (i = 0; i < MmNumberOfPagingFiles; i++)
    {
        /* Free page file name */
        ASSERT(MmPagingFile[i]->PageFileName.Buffer != NULL);
        ExFreePoolWithTag(MmPagingFile[i]->PageFileName.Buffer, TAG_MM);
        MmPagingFile[i]->PageFileName.Buffer = NULL;

        /* And close them */
        ZwClose(MmPagingFile[i]->FileHandle);
    }

    /* Loop through all the pages owned by the legacy Mm and flush them to disk. */
    while ((Page = MmGetLRUFirstModifiedPage()))
    {
        LARGE_INTEGER SegmentOffset;
        PMM_SECTION_SEGMENT Segment = MmGetSectionAssociation(Page, &SegmentOffset);

        if (Segment)
        {
            MmLockSectionSegment(Segment);
            MmCheckDirtySegment(Segment, &SegmentOffset, TRUE);
            MmUnlockSectionSegment(Segment);
            MmDereferenceSegment(Segment);
        }
    }
}

VOID
MmShutdownSystem(IN ULONG Phase)
{
    if (Phase == 0)
    {
        MiShutdownSystem();
    }
    else if (Phase == 1)
    {
        ULONG i;

        /* Loop through all the paging files */
        for (i = 0; i < MmNumberOfPagingFiles; i++)
        {
            /* And dereference them */
            ObDereferenceObject(MmPagingFile[i]->FileObject);
        }
    }
    else
    {
        ASSERT(Phase == 2);

        UNIMPLEMENTED;
    }
}
