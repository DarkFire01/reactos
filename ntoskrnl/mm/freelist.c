/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/freelist.c
 * PURPOSE:         Handle the list of free physical pages
 *
 * PROGRAMMERS:     David Welch (welch@cwcom.net)
 *                  Robert Bergkvist
 */

/* INCLUDES ****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include "ARM3/miarm.h"

/* GLOBALS ****************************************************************/

PMMPFN MmPfnDatabase;

PFN_NUMBER MmAvailablePages;
PFN_NUMBER MmResidentAvailablePages;
PFN_NUMBER MmResidentAvailableAtInit;

SIZE_T MmTotalCommittedPages;
SIZE_T MmSharedCommit;
SIZE_T MmDriverCommit;
SIZE_T MmProcessCommit;
SIZE_T MmPagedPoolCommit;
SIZE_T MmPeakCommitment;

/* FUNCTIONS *************************************************************/

/**
 * @brief Charges pages against the commit limit.
 *
 * @param[in] Pages
 * Number of pages to charge.
 *
 * @return TRUE if the pages fit in the limit, FALSE otherwise.
 *
 * @remarks A committed page needs a place to live, in memory or in a paging file.
 * Whoever takes a charge gives it back with MiReturnCommitment.
 */
BOOLEAN
NTAPI
MiChargeCommitment(
    _In_ PFN_NUMBER Pages)
{
    SIZE_T Committed;

    Committed = InterlockedExchangeAddSizeT(&MmTotalCommittedPages, Pages) + Pages;
    if (Committed > MmTotalCommitLimit)
    {
        InterlockedExchangeAddSizeT(&MmTotalCommittedPages, -(SSIZE_T)Pages);
        return FALSE;
    }

    if (Committed > MmPeakCommitment)
        MmPeakCommitment = Committed;

    return TRUE;
}

/**
 * @brief Gives charged pages back to the commit limit.
 *
 * @param[in] Pages
 * Number of pages to return.
 */
VOID
NTAPI
MiReturnCommitment(
    _In_ PFN_NUMBER Pages)
{
    ASSERT(MmTotalCommittedPages >= Pages);
    InterlockedExchangeAddSizeT(&MmTotalCommittedPages, -(SSIZE_T)Pages);
}

BOOLEAN
NTAPI
MiIsPfnFree(IN PMMPFN Pfn1)
{
    /* Must be a free or zero page, with no references, linked */
    return ((Pfn1->u3.e1.PageLocation <= FreePageList) &&
            (Pfn1->u1.Flink) &&
            (Pfn1->u2.Blink) &&
            !(Pfn1->u3.e2.ReferenceCount));
}

BOOLEAN
NTAPI
MiIsPfnInUse(IN PMMPFN Pfn1)
{
    /* Standby list or higher, unlinked, and with references */
    return !MiIsPfnFree(Pfn1);
}

PMDL
NTAPI
MiAllocatePagesForMdl(IN PHYSICAL_ADDRESS LowAddress,
                      IN PHYSICAL_ADDRESS HighAddress,
                      IN PHYSICAL_ADDRESS SkipBytes,
                      IN SIZE_T TotalBytes,
                      IN MI_PFN_CACHE_ATTRIBUTE CacheAttribute,
                      IN ULONG MdlFlags)
{
    PMDL Mdl;
    PFN_NUMBER PageCount, LowPage, HighPage, SkipPages, PagesFound = 0, Page;
    PPFN_NUMBER MdlPage, LastMdlPage;
    KIRQL OldIrql;
    PMMPFN Pfn1;
    INT LookForZeroedPages;
    BOOLEAN Trimmed = FALSE;

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    DPRINT("ARM3-DEBUG: Being called with %I64x %I64x %I64x %lx %d %lu\n", LowAddress, HighAddress, SkipBytes, TotalBytes, CacheAttribute, MdlFlags);

    //
    // Convert the low address into a PFN
    //
    LowPage = (PFN_NUMBER)(LowAddress.QuadPart >> PAGE_SHIFT);

    //
    // Convert, and normalize, the high address into a PFN
    //
    HighPage = (PFN_NUMBER)(HighAddress.QuadPart >> PAGE_SHIFT);
    if (HighPage > MmHighestPhysicalPage) HighPage = MmHighestPhysicalPage;

    //
    // Validate skipbytes and convert them into pages
    //
    if (BYTE_OFFSET(SkipBytes.LowPart)) return NULL;
    SkipPages = (PFN_NUMBER)(SkipBytes.QuadPart >> PAGE_SHIFT);

    /* This isn't supported at all */
    if (SkipPages) DPRINT1("WARNING: Caller requesting SkipBytes, MDL might be mismatched\n");

    //
    // Now compute the number of pages the MDL will cover
    //
    PageCount = (PFN_NUMBER)ADDRESS_AND_SIZE_TO_SPAN_PAGES(0, TotalBytes);
    do
    {
        //
        // Try creating an MDL for these many pages
        //
        Mdl = MmCreateMdl(NULL, NULL, PageCount << PAGE_SHIFT);
        if (Mdl) break;

        //
        // This function is not required to return the amount of pages requested
        // In fact, it can return as little as 1 page, and callers are supposed
        // to deal with this scenario. So re-attempt the allocation with less
        // pages than before, and see if it worked this time.
        //
        PageCount -= (PageCount >> 4);
    } while (PageCount);

    //
    // Wow, not even a single page was around!
    //
    if (!Mdl) return NULL;

    //
    // This is where the page array starts....
    //
    MdlPage = (PPFN_NUMBER)(Mdl + 1);

    //
    // Lock the PFN database
    //
    OldIrql = MiAcquirePfnLock();

    //
    // Are we looking for any pages, without discriminating?
    //
    if ((LowPage == 0) && (HighPage == MmHighestPhysicalPage))
    {
        //
        // Well then, let's go shopping
        //
        while (PagesFound < PageCount)
        {
            /* Grab a page */
            MI_SET_USAGE(MI_USAGE_MDL);
            MI_SET_PROCESS2("Kernel");

            /* Faults still need pages, a caller never gets the last ones */
            Page = 0;
            if (MmAvailablePages > MmMinimumFreePages)
                Page = MiRemoveAnyPage(0);

            if (Page == 0)
            {
                /* Out of pages, let the writer and the trimmers have one go at it */
                if (Trimmed || (OldIrql >= DISPATCH_LEVEL))
                {
                    /* The caller has to make do with what we found, even nothing */
                    break;
                }

                MiReleasePfnLock(OldIrql);
                MiWaitForFreePage();
                OldIrql = MiAcquirePfnLock();
                Trimmed = TRUE;
                continue;
            }

            /* Grab the page entry for it */
            Pfn1 = MiGetPfnEntry(Page);

            //
            // Make sure it's really free
            //
            ASSERT(Pfn1->u3.e2.ReferenceCount == 0);

            /* Now setup the page and mark it */
            Pfn1->u3.e2.ReferenceCount = 1;
            Pfn1->u2.ShareCount = 1;
            MI_SET_PFN_DELETED(Pfn1);
            Pfn1->u4.PteFrame = 0x1FFEDCB;
            Pfn1->u3.e1.StartOfAllocation = 1;
            Pfn1->u3.e1.EndOfAllocation = 1;
            Pfn1->u4.VerifierAllocation = 0;

            //
            // Save it into the MDL
            //
            *MdlPage++ = MiGetPfnEntryIndex(Pfn1);
            PagesFound++;
        }
    }
    else
    {
        //
        // You want specific range of pages. We'll do this in two runs
        //
        for (LookForZeroedPages = 1; LookForZeroedPages >= 0; LookForZeroedPages--)
        {
            //
            // Scan the range you specified
            //
            for (Page = LowPage; Page < HighPage; Page++)
            {
                //
                // Get the PFN entry for this page
                //
                Pfn1 = MiGetPfnEntry(Page);
                if (!Pfn1) continue;

                //
                // Make sure it's free and if this is our first pass, zeroed
                //
                if (MiIsPfnInUse(Pfn1)) continue;
                if ((Pfn1->u3.e1.PageLocation == ZeroedPageList) != LookForZeroedPages) continue;

                /* Remove the page from the free or zero list */
                ASSERT(Pfn1->u3.e1.ReadInProgress == 0);
                MI_SET_USAGE(MI_USAGE_MDL);
                MI_SET_PROCESS2("Kernel");
                MiUnlinkFreeOrZeroedPage(Pfn1);

                //
                // Sanity checks
                //
                ASSERT(Pfn1->u3.e2.ReferenceCount == 0);

                //
                // Now setup the page and mark it
                //
                Pfn1->u3.e2.ReferenceCount = 1;
                Pfn1->u2.ShareCount = 1;
                MI_SET_PFN_DELETED(Pfn1);
                Pfn1->u4.PteFrame = 0x1FFEDCB;
                Pfn1->u3.e1.StartOfAllocation = 1;
                Pfn1->u3.e1.EndOfAllocation = 1;
                Pfn1->u4.VerifierAllocation = 0;

                //
                // Save this page into the MDL
                //
                *MdlPage++ = Page;
                if (++PagesFound == PageCount) break;
            }

            //
            // If the first pass was enough, don't keep going, otherwise, go again
            //
            if (PagesFound == PageCount) break;
        }
    }

    //
    // Now release the PFN count
    //
    MiReleasePfnLock(OldIrql);

    //
    // We might've found less pages, but not more ;-)
    //
    if (PagesFound != PageCount) ASSERT(PagesFound < PageCount);
    if (!PagesFound)
    {
        //
        // If we didn' tfind any pages at all, fail
        //
        DPRINT1("NO MDL PAGES!\n");
        ExFreePoolWithTag(Mdl, TAG_MDL);
        return NULL;
    }

    //
    // Write out how many pages we found
    //
    Mdl->ByteCount = (ULONG)(PagesFound << PAGE_SHIFT);

    //
    // Terminate the MDL array if there's certain missing pages
    //
    if (PagesFound != PageCount) *MdlPage = LIST_HEAD;

    //
    // Now go back and loop over all the MDL pages
    //
    MdlPage = (PPFN_NUMBER)(Mdl + 1);
    LastMdlPage = MdlPage + PagesFound;
    while (MdlPage < LastMdlPage)
    {
        //
        // Check if we've reached the end
        //
        Page = *MdlPage++;
        if (Page == LIST_HEAD) break;

        //
        // Get the PFN entry for the page and check if we should zero it out
        //
        Pfn1 = MiGetPfnEntry(Page);
        ASSERT(Pfn1);
        if (Pfn1->u3.e1.PageLocation != ZeroedPageList) MiZeroPhysicalPage(Page);
        Pfn1->u3.e1.PageLocation = ActiveAndValid;
    }

    //
    // We're done, mark the pages as locked
    //
    Mdl->Process = NULL;
    Mdl->MdlFlags |= MDL_PAGES_LOCKED;
    return Mdl;
}

/* EOF */
