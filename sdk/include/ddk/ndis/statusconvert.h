/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Mapping between NDIS_STATUS and NTSTATUS
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

NDISAPI
NTSTATUS
NTAPI
NdisConvertNdisStatusToNtStatus(
    _In_ NDIS_STATUS NdisStatus);

NDISAPI
NDIS_STATUS
NTAPI
NdisConvertNtStatusToNdisStatus(
    _In_ NTSTATUS NtStatus);

#ifdef __cplusplus
}
#endif
