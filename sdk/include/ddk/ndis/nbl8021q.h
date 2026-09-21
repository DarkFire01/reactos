/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     802.1Q tag carried in a NET_BUFFER_LIST info slot
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The tag sits in the slot itself rather than behind a pointer. Wireless
 * frames reuse four of the reserved bits for WMM access category information.
 */
typedef struct _NDIS_NET_BUFFER_LIST_8021Q_INFO
{
    union
    {
        struct
        {
            UINT32 UserPriority : 3;
            UINT32 CanonicalFormatId : 1;
            UINT32 VlanId : 12;
            UINT32 Reserved : 16;
        } TagHeader;
        struct
        {
            UINT32 UserPriority : 3;
            UINT32 CanonicalFormatId : 1;
            UINT32 VlanId : 12;
            UINT32 WMMInfo : 4;
            UINT32 Reserved : 12;
        } WLanTagHeader;
        PVOID Value;
    } DUMMYUNIONNAME;
} NDIS_NET_BUFFER_LIST_8021Q_INFO, *PNDIS_NET_BUFFER_LIST_8021Q_INFO;

C_ASSERT(sizeof(NDIS_NET_BUFFER_LIST_8021Q_INFO) == sizeof(PVOID));

#define NDIS_GET_NET_BUFFER_LIST_VLAN_ID(_NBL)     (((PNDIS_NET_BUFFER_LIST_8021Q_INFO)         &NET_BUFFER_LIST_INFO((_NBL), Ieee8021QNetBufferListInfo))->TagHeader.VlanId)

#ifdef __cplusplus
}
#endif
