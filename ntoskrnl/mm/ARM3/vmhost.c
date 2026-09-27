/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Memory routines the second level translation host publishes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The kernel offers a host that the virtualization stack binds itself into,
 * and the host publishes a table of routines the driver calls by slot. These
 * are the ones about memory. The table itself is in ex/exthost.c, which is
 * also where a slot with nothing behind it is answered.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include "miarm.h"

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Maps a view of a section into the calling process so that it can be run.
 *
 * @param[in] SectionHandle
 * The section, which has to have been opened for execute.
 *
 * @param[in,out] BaseAddress
 * Where to map it, or NULL to be given somewhere. Receives where it went.
 *
 * @param[in,out] SectionOffset
 * How far into the section the view starts. Rounded down as usual.
 *
 * @param[in,out] ViewSize
 * How much to map. Receives how much was mapped.
 *
 * @remarks
 * The name says what it skips on Windows, which is the guard that stops a
 * process mapping memory it can execute. Nothing here guards that, so this is
 * an ordinary executable mapping.
 */
NTSTATUS
NTAPI
VmMapSectionExecuteNoAcg(
    _In_ HANDLE SectionHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PLARGE_INTEGER SectionOffset,
    _Inout_ PSIZE_T ViewSize)
{
    return ZwMapViewOfSection(SectionHandle,
                              ZwCurrentProcess(),
                              BaseAddress,
                              0,
                              0,
                              SectionOffset,
                              ViewSize,
                              ViewUnmap,
                              0,
                              PAGE_EXECUTE);
}

/* EOF */
