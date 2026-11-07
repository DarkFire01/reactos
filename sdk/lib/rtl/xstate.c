/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extended processor state in contexts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Finds the state of an extended processor feature in a context.
 *
 * @return
 * The feature state, or NULL when the context holds none for it.
 */
PVOID
NTAPI
RtlLocateExtendedFeature(
    _In_ struct _CONTEXT_EX *ContextEx,
    _In_ ULONG FeatureId,
    _Out_opt_ PULONG Length)
{
    UNREFERENCED_PARAMETER(ContextEx);
    UNREFERENCED_PARAMETER(FeatureId);
    UNREFERENCED_PARAMETER(Length);

    /* The system enables no extended state features, so contexts never carry one */
    return NULL;
}

/* EOF */
