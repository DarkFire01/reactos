/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     User mode access to WMI data blocks
 */

#ifndef _WMIUM_
#define _WMIUM_

#include <guiddef.h>
#include <basetsd.h>
#include <wmistr.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WMIAPI
#define WMIAPI __stdcall
#endif

typedef PVOID WMIHANDLE, *PWMIHANDLE, MOFHANDLE, *PMOFHANDLE;

#define MOFCI_RESERVED0 0x00000001
#define MOFCI_RESERVED1 0x00000002
#define MOFCI_RESERVED2 0x00000004

typedef struct
{
    LPWSTR ImagePath;
    LPWSTR ResourceName;
    ULONG ResourceSize;
    PUCHAR ResourceBuffer;
} MOFRESOURCEINFOW, *PMOFRESOURCEINFOW;

typedef struct
{
    LPSTR ImagePath;
    LPSTR ResourceName;
    ULONG ResourceSize;
    PUCHAR ResourceBuffer;
} MOFRESOURCEINFOA, *PMOFRESOURCEINFOA;

#ifdef UNICODE
typedef MOFRESOURCEINFOW MOFRESOURCEINFO;
typedef PMOFRESOURCEINFOW PMOFRESOURCEINFO;
#else
typedef MOFRESOURCEINFOA MOFRESOURCEINFO;
typedef PMOFRESOURCEINFOA PMOFRESOURCEINFO;
#endif

typedef struct _WMIGUIDINFORMATION
{
    ULONG Size;
    BOOLEAN IsExpensive;
    BOOLEAN IsEventOnly;
} WMIGUIDINFORMATION, *PWMIGUIDINFORMATION;

#define NOTIFICATION_TRACE_FLAG         0x00010000
#define NOTIFICATION_CALLBACK_DIRECT    0x00000004
#define NOTIFICATION_CHECK_ACCESS       0x00000008
#define NOTIFICATION_LIGHTWEIGHT_FLAG   0x00000020

typedef VOID (WINAPI *NOTIFICATIONCALLBACK)(
    _In_ PWNODE_HEADER Wnode,
    _In_ UINT_PTR NotificationContext);

DEFINE_GUID(GUID_REGISTRATION_CHANGE_NOTIFICATION, 0xb48d49a1, 0xe777, 0x11d0, 0xa5, 0x0c, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10);
DEFINE_GUID(GUID_MOF_RESOURCE_ADDED_NOTIFICATION, 0xb48d49a2, 0xe777, 0x11d0, 0xa5, 0x0c, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10);
DEFINE_GUID(GUID_MOF_RESOURCE_REMOVED_NOTIFICATION, 0xb48d49a3, 0xe777, 0x11d0, 0xa5, 0x0c, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0x10);

#define WmiInsertTimestamp(WnodeHeader) \
    GetSystemTimeAsFileTime((FILETIME *)&((PWNODE_HEADER)(WnodeHeader))->TimeStamp)

ULONG
WMIAPI
WmiOpenBlock(
    _In_ GUID *Guid,
    _In_ ULONG DesiredAccess,
    _Out_ WMIHANDLE *DataBlockHandle);

ULONG
WMIAPI
WmiCloseBlock(
    _In_ WMIHANDLE DataBlockHandle);

ULONG
WMIAPI
WmiQueryAllDataA(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQueryAllDataW(
    _In_ WMIHANDLE DataBlockHandle,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQuerySingleInstanceA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiQuerySingleInstanceW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _Inout_ ULONG *BufferSize,
    _Out_writes_bytes_opt_(*BufferSize) LPVOID Buffer);

ULONG
WMIAPI
WmiSetSingleInstanceA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer);

ULONG
WMIAPI
WmiSetSingleInstanceW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer);

ULONG
WMIAPI
WmiSetSingleItemA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCSTR InstanceName,
    _In_ ULONG DataItemId,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer);

ULONG
WMIAPI
WmiSetSingleItemW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ LPCWSTR InstanceName,
    _In_ ULONG DataItemId,
    _In_ ULONG Reserved,
    _In_ ULONG ValueBufferSize,
    _In_reads_bytes_(ValueBufferSize) PVOID ValueBuffer);

ULONG
WMIAPI
WmiExecuteMethodA(
    _In_ WMIHANDLE MethodDataBlockHandle,
    _In_ LPCSTR MethodInstanceName,
    _In_ ULONG MethodId,
    _In_ ULONG InputValueBufferSize,
    _In_reads_bytes_opt_(InputValueBufferSize) PVOID InputValueBuffer,
    _Inout_ ULONG *OutputBufferSize,
    _Out_writes_bytes_opt_(*OutputBufferSize) PVOID OutputBuffer);

ULONG
WMIAPI
WmiExecuteMethodW(
    _In_ WMIHANDLE MethodDataBlockHandle,
    _In_ LPCWSTR MethodInstanceName,
    _In_ ULONG MethodId,
    _In_ ULONG InputValueBufferSize,
    _In_reads_bytes_opt_(InputValueBufferSize) PVOID InputValueBuffer,
    _Inout_ ULONG *OutputBufferSize,
    _Out_writes_bytes_opt_(*OutputBufferSize) PVOID OutputBuffer);

ULONG
WMIAPI
WmiNotificationRegistrationA(
    _In_ LPGUID Guid,
    _In_ BOOLEAN Enable,
    _In_ PVOID DeliveryInfo,
    _In_ ULONG_PTR DeliveryContext,
    _In_ ULONG Flags);

ULONG
WMIAPI
WmiNotificationRegistrationW(
    _In_ LPGUID Guid,
    _In_ BOOLEAN Enable,
    _In_ PVOID DeliveryInfo,
    _In_ ULONG_PTR DeliveryContext,
    _In_ ULONG Flags);

ULONG
WMIAPI
WmiMofEnumerateResourcesA(
    _In_ MOFHANDLE MofResourceHandle,
    _Out_ ULONG *MofResourceCount,
    _Outptr_result_buffer_(*MofResourceCount) PMOFRESOURCEINFOA *MofResourceInfo);

ULONG
WMIAPI
WmiMofEnumerateResourcesW(
    _In_ MOFHANDLE MofResourceHandle,
    _Out_ ULONG *MofResourceCount,
    _Outptr_result_buffer_(*MofResourceCount) PMOFRESOURCEINFOW *MofResourceInfo);

VOID
WMIAPI
WmiFreeBuffer(
    _In_ PVOID Buffer);

ULONG
WMIAPI
WmiQueryGuidInformation(
    _In_ WMIHANDLE GuidHandle,
    _Inout_ PWMIGUIDINFORMATION GuidInfo);

ULONG
WMIAPI
WmiEnumerateGuids(
    _Out_writes_to_(*GuidCount, *GuidCount) LPGUID GuidList,
    _Inout_ ULONG *GuidCount);

ULONG
WMIAPI
WmiFileHandleToInstanceNameA(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ HANDLE FileHandle,
    _Inout_ ULONG *NumberCharacters,
    _Out_writes_opt_(*NumberCharacters) PCHAR InstanceNames);

ULONG
WMIAPI
WmiFileHandleToInstanceNameW(
    _In_ WMIHANDLE DataBlockHandle,
    _In_ HANDLE FileHandle,
    _Inout_ ULONG *NumberCharacters,
    _Out_writes_opt_(*NumberCharacters) PWCHAR InstanceNames);

#ifdef UNICODE
#define WmiQueryAllData WmiQueryAllDataW
#define WmiQuerySingleInstance WmiQuerySingleInstanceW
#define WmiSetSingleInstance WmiSetSingleInstanceW
#define WmiSetSingleItem WmiSetSingleItemW
#define WmiExecuteMethod WmiExecuteMethodW
#define WmiNotificationRegistration WmiNotificationRegistrationW
#define WmiMofEnumerateResources WmiMofEnumerateResourcesW
#define WmiFileHandleToInstanceName WmiFileHandleToInstanceNameW
#else
#define WmiQueryAllData WmiQueryAllDataA
#define WmiQuerySingleInstance WmiQuerySingleInstanceA
#define WmiSetSingleInstance WmiSetSingleInstanceA
#define WmiSetSingleItem WmiSetSingleItemA
#define WmiExecuteMethod WmiExecuteMethodA
#define WmiNotificationRegistration WmiNotificationRegistrationA
#define WmiMofEnumerateResources WmiMofEnumerateResourcesA
#define WmiFileHandleToInstanceName WmiFileHandleToInstanceNameA
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WMIUM_ */
