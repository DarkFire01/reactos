/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A single buffer within a packet
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Packed into eight bytes, so a fragment ring stays dense. */
typedef struct _NET_FRAGMENT
{
    UINT64 ValidLength : 26;
    UINT64 Capacity : 26;
    UINT64 Offset : 10;
    UINT64 Scratch : 1;
    UINT64 OsReserved_Bounced : 1;
} NET_FRAGMENT;

C_ASSERT(sizeof(NET_FRAGMENT) == 8);

#ifdef __cplusplus
}
#endif
