/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     D3DKMT dxgkrnl syscalls
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <gdi32_vista.h>
#include <d3dkmddi.h>
#include <winuser.h>

/*
 * <d3dkmthk.h> hides D3DKMT_OPENADAPTERFROMLUID and D3DKMT_QUERYVIDEOMEMORYINFO
 * behind DXGKDDI_INTERFACE_VERSION gates of WIN8 and WDDM2_2, while the tree
 * targets Vista. for now but honestly this can change.
 */

#if (DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN8)
typedef struct _D3DKMT_OPENADAPTERFROMLUID
{
    LUID            AdapterLuid;
    D3DKMT_HANDLE   hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;
#endif

/* NT6.2 syscall, appended to w32ksvc so ntgdi.h (Vista) does not declare it */
NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromLuid(
    _Inout_ D3DKMT_OPENADAPTERFROMLUID* unnamedParam1);

#if (DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WDDM2_2)
typedef struct _D3DKMT_QUERYVIDEOMEMORYINFO
{
    HANDLE                      hProcess;
    D3DKMT_HANDLE               hAdapter;
    D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
    UINT64                      Budget;
    UINT64                      CurrentUsage;
    UINT64                      CurrentReservation;
    UINT64                      AvailableForReservation;
    UINT                        PhysicalAdapterIndex;
} D3DKMT_QUERYVIDEOMEMORYINFO;
#endif

/* Same for these two, which dxgkrnl answers through its own interface slots */
NTSTATUS
APIENTRY
NtGdiDdDDICheckVidPnExclusiveOwnership(
    _In_ CONST D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP* unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryVideoMemoryInfo(
    _Inout_ D3DKMT_QUERYVIDEOMEMORYINFO* unnamedParam1);

#define D3DKMT_EMU_ADAPTER_TAG  0x0ada0000u
#define D3DKMT_EMU_DEVICE_TAG   0x0de00000u
#define D3DKMT_EMU_INDEX_MASK   0x0000ffffu

#define D3DKMT_EMU_MAX_ADAPTERS 16
#define D3DKMT_EMU_MAX_DEVICES  64

typedef struct _D3DKMT_EMU_ADAPTER
{
    LONG InUse;
    LUID AdapterLuid;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
} D3DKMT_EMU_ADAPTER;

typedef struct _D3DKMT_EMU_DEVICE
{
    LONG InUse;
    D3DKMT_HANDLE hAdapter;
} D3DKMT_EMU_DEVICE;

static D3DKMT_EMU_ADAPTER D3DKMTEmuAdapters[D3DKMT_EMU_MAX_ADAPTERS];
static D3DKMT_EMU_DEVICE D3DKMTEmuDevices[D3DKMT_EMU_MAX_DEVICES];

/* Emulated handles never reach dxgkrnl, and dxgkrnl handles never reach the emulation */
#define D3DKMT_EMU_IS_ADAPTER(h) (((h) & ~D3DKMT_EMU_INDEX_MASK) == D3DKMT_EMU_ADAPTER_TAG)
#define D3DKMT_EMU_IS_DEVICE(h)  (((h) & ~D3DKMT_EMU_INDEX_MASK) == D3DKMT_EMU_DEVICE_TAG)

static
D3DKMT_EMU_ADAPTER*
D3DKMTEmuGetAdapter(
    _In_ D3DKMT_HANDLE hAdapter)
{
    ULONG Index;

    if ((hAdapter & ~D3DKMT_EMU_INDEX_MASK) != D3DKMT_EMU_ADAPTER_TAG)
        return NULL;

    Index = hAdapter & D3DKMT_EMU_INDEX_MASK;
    if (Index == 0 || Index > D3DKMT_EMU_MAX_ADAPTERS)
        return NULL;

    if (!D3DKMTEmuAdapters[Index - 1].InUse)
        return NULL;

    return &D3DKMTEmuAdapters[Index - 1];
}

static
D3DKMT_EMU_DEVICE*
D3DKMTEmuGetDevice(
    _In_ D3DKMT_HANDLE hDevice)
{
    ULONG Index;

    if ((hDevice & ~D3DKMT_EMU_INDEX_MASK) != D3DKMT_EMU_DEVICE_TAG)
        return NULL;

    Index = hDevice & D3DKMT_EMU_INDEX_MASK;
    if (Index == 0 || Index > D3DKMT_EMU_MAX_DEVICES)
        return NULL;

    if (!D3DKMTEmuDevices[Index - 1].InUse)
        return NULL;

    return &D3DKMTEmuDevices[Index - 1];
}

static
D3DKMT_HANDLE
D3DKMTEmuOpenAdapter(
    _In_ const LUID* AdapterLuid,
    _In_ D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId)
{
    ULONG Index;

    for (Index = 0; Index < D3DKMT_EMU_MAX_ADAPTERS; Index++)
    {
        if (InterlockedCompareExchange(&D3DKMTEmuAdapters[Index].InUse, 1, 0) == 0)
        {
            D3DKMTEmuAdapters[Index].AdapterLuid = *AdapterLuid;
            D3DKMTEmuAdapters[Index].VidPnSourceId = VidPnSourceId;
            return D3DKMT_EMU_ADAPTER_TAG | (Index + 1);
        }
    }

    return 0;
}

/* fake LUID */
static
VOID
D3DKMTEmuMakeLuid(
    _In_ D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId,
    _Out_ LUID* AdapterLuid)
{
    AdapterLuid->HighPart = 0x524f5300; /* 'ROS\0' */
    AdapterLuid->LowPart = VidPnSourceId + 1;
}

/* Not just a syscall even in wine. */
NTSTATUS
WINAPI
D3DKMTOpenAdapterFromGdiDisplayName(_Inout_ D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME* unnamedParam1)
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId = 0;
    DISPLAY_DEVICEW DisplayDevice;
    D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    DWORD Index = 0;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    /* A display driven by WDDM gets its real dxgkrnl adapter */
    if (NT_SUCCESS(NtGdiDdDDIOpenAdapterFromGdiDisplayName(unnamedParam1)))
        return STATUS_SUCCESS;

    /* Locate the GDI display device with this name to obtain its index, which
       doubles as the VidPN source ID */
    DisplayDevice.cb = sizeof(DisplayDevice);
    while (EnumDisplayDevicesW(NULL, Index, &DisplayDevice, 0))
    {
        if (wcsncmp(DisplayDevice.DeviceName,
                    unnamedParam1->DeviceName,
                    ARRAYSIZE(unnamedParam1->DeviceName)) == 0)
        {
            VidPnSourceId = Index;
            break;
        }

        DisplayDevice.cb = sizeof(DisplayDevice);
        Index++;
    }

    if (!DisplayDevice.DeviceName[0] || VidPnSourceId != Index)
        return STATUS_INVALID_PARAMETER;

    D3DKMTEmuMakeLuid(VidPnSourceId, &AdapterLuid);

    hAdapter = D3DKMTEmuOpenAdapter(&AdapterLuid, VidPnSourceId);
    if (!hAdapter)
        return STATUS_INSUFFICIENT_RESOURCES;

    unnamedParam1->hAdapter = hAdapter;
    unnamedParam1->AdapterLuid = AdapterLuid;
    unnamedParam1->VidPnSourceId = VidPnSourceId;

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTOpenAdapterFromLuid(_Inout_ CONST D3DKMT_OPENADAPTERFROMLUID* unnamedParam1)
{
    D3DKMT_OPENADAPTERFROMLUID* Desc = (D3DKMT_OPENADAPTERFROMLUID*)unnamedParam1;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    D3DKMT_HANDLE hAdapter;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    /* Recover the source ID for LUIDs we minted ourselves; anything else is a
       caller-allocated LUID, for which the primary output is the best match. */
    if (unnamedParam1->AdapterLuid.HighPart == 0x524f5300 &&
        unnamedParam1->AdapterLuid.LowPart > 0)
    {
        VidPnSourceId = unnamedParam1->AdapterLuid.LowPart - 1;
    }
    else
    {
        if (NT_SUCCESS(NtGdiDdDDIOpenAdapterFromLuid(Desc)))
            return STATUS_SUCCESS;

        VidPnSourceId = 0;
    }

    hAdapter = D3DKMTEmuOpenAdapter(&unnamedParam1->AdapterLuid, VidPnSourceId);
    if (!hAdapter)
        return STATUS_INSUFFICIENT_RESOURCES;

    Desc->hAdapter = hAdapter;

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTCloseAdapter(_In_ const D3DKMT_CLOSEADAPTER* unnamedParam1)
{
    D3DKMT_EMU_ADAPTER* Adapter;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMT_EMU_IS_ADAPTER(unnamedParam1->hAdapter))
        return NtGdiDdDDICloseAdapter(unnamedParam1);

    Adapter = D3DKMTEmuGetAdapter(unnamedParam1->hAdapter);
    if (!Adapter)
        return STATUS_INVALID_PARAMETER;

    InterlockedExchange(&Adapter->InUse, 0);

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTCreateDevice(_Inout_ D3DKMT_CREATEDEVICE* unnamedParam1)
{
    ULONG Index;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMT_EMU_IS_ADAPTER(unnamedParam1->hAdapter))
        return NtGdiDdDDICreateDevice(unnamedParam1);

    if (!D3DKMTEmuGetAdapter(unnamedParam1->hAdapter))
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < D3DKMT_EMU_MAX_DEVICES; Index++)
    {
        if (InterlockedCompareExchange(&D3DKMTEmuDevices[Index].InUse, 1, 0) == 0)
        {
            D3DKMTEmuDevices[Index].hAdapter = unnamedParam1->hAdapter;

            unnamedParam1->hDevice = D3DKMT_EMU_DEVICE_TAG | (Index + 1);
            unnamedParam1->pCommandBuffer = NULL;
            unnamedParam1->CommandBufferSize = 0;
            unnamedParam1->pAllocationList = NULL;
            unnamedParam1->AllocationListSize = 0;
            unnamedParam1->pPatchLocationList = NULL;
            unnamedParam1->PatchLocationListSize = 0;

            return STATUS_SUCCESS;
        }
    }

    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS
WINAPI
D3DKMTDestroyDevice(_In_ const D3DKMT_DESTROYDEVICE* unnamedParam1)
{
    D3DKMT_EMU_DEVICE* Device;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMT_EMU_IS_DEVICE(unnamedParam1->hDevice))
        return NtGdiDdDDIDestroyDevice(unnamedParam1);

    Device = D3DKMTEmuGetDevice(unnamedParam1->hDevice);
    if (!Device)
        return STATUS_INVALID_PARAMETER;

    InterlockedExchange(&Device->InUse, 0);

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTSetVidPnSourceOwner(_In_ const D3DKMT_SETVIDPNSOURCEOWNER* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMT_EMU_IS_DEVICE(unnamedParam1->hDevice))
        return NtGdiDdDDISetVidPnSourceOwner(unnamedParam1);

    /* A zero VidPnSourceCount releases ownership, which always succeeds. */
    if (unnamedParam1->VidPnSourceCount == 0)
        return STATUS_SUCCESS;

    if (!D3DKMTEmuGetDevice(unnamedParam1->hDevice))
        return STATUS_INVALID_PARAMETER;

    return STATUS_PROCEDURE_NOT_FOUND;
}

NTSTATUS
WINAPI
D3DKMTCheckVidPnExclusiveOwnership(_In_ CONST D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    /* The emulation owns no VidPN source, so this is dxgkrnl's question alone */
    return NtGdiDdDDICheckVidPnExclusiveOwnership(unnamedParam1);
}

NTSTATUS
WINAPI
D3DKMTQueryVideoMemoryInfo(_Inout_ D3DKMT_QUERYVIDEOMEMORYINFO* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMT_EMU_IS_ADAPTER(unnamedParam1->hAdapter))
        return NtGdiDdDDIQueryVideoMemoryInfo(unnamedParam1);

    if (!D3DKMTEmuGetAdapter(unnamedParam1->hAdapter))
        return STATUS_INVALID_PARAMETER;

    /* The emulated adapter has no memory of its own to report */
    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * The trim and budget change notifications are a user mode registry: no syscall carries
 * them, and nothing here raises either event yet. A registration is kept so the caller
 * gets a handle it can unregister with, and the callback is never invoked.
 */

/* Spelled out the same way the two above are, the tree not being at WDDM 2.2 */
#if (DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WDDM2_2)

typedef VOID (APIENTRY *PFND3DKMT_TRIMNOTIFICATIONCALLBACK)(VOID*);
typedef VOID (APIENTRY *PFND3DKMT_BUDGETCHANGENOTIFICATIONCALLBACK)(VOID*);

typedef struct _D3DKMT_REGISTERTRIMNOTIFICATION
{
    LUID                               AdapterLuid;
    D3DKMT_HANDLE                      hDevice;
    PFND3DKMT_TRIMNOTIFICATIONCALLBACK Callback;
    VOID*                              Context;
    VOID*                              Handle;
} D3DKMT_REGISTERTRIMNOTIFICATION;

typedef struct _D3DKMT_UNREGISTERTRIMNOTIFICATION
{
    VOID*                              Handle;
    PFND3DKMT_TRIMNOTIFICATIONCALLBACK Callback;
} D3DKMT_UNREGISTERTRIMNOTIFICATION;

typedef struct _D3DKMT_REGISTERBUDGETCHANGENOTIFICATION
{
    D3DKMT_HANDLE                              hDevice;
    PFND3DKMT_BUDGETCHANGENOTIFICATIONCALLBACK Callback;
    VOID*                                      Context;
    VOID*                                      Handle;
} D3DKMT_REGISTERBUDGETCHANGENOTIFICATION;

typedef struct _D3DKMT_UNREGISTERBUDGETCHANGENOTIFICATION
{
    VOID* Handle;
} D3DKMT_UNREGISTERBUDGETCHANGENOTIFICATION;

#endif

#define D3DKMT_MAX_NOTIFICATIONS 32

typedef struct _D3DKMT_NOTIFICATION
{
    LONG InUse;
    PVOID Callback;
    PVOID Context;
} D3DKMT_NOTIFICATION;

static D3DKMT_NOTIFICATION D3DKMTTrimNotifications[D3DKMT_MAX_NOTIFICATIONS];
static D3DKMT_NOTIFICATION D3DKMTBudgetNotifications[D3DKMT_MAX_NOTIFICATIONS];

static
D3DKMT_NOTIFICATION*
D3DKMTAddNotification(
    _Inout_ D3DKMT_NOTIFICATION* Table,
    _In_ PVOID Callback,
    _In_opt_ PVOID Context)
{
    ULONG Index;

    for (Index = 0; Index < D3DKMT_MAX_NOTIFICATIONS; Index++)
    {
        if (InterlockedCompareExchange(&Table[Index].InUse, 1, 0) == 0)
        {
            Table[Index].Callback = Callback;
            Table[Index].Context = Context;
            return &Table[Index];
        }
    }

    return NULL;
}

static
BOOL
D3DKMTRemoveNotification(
    _Inout_ D3DKMT_NOTIFICATION* Table,
    _In_opt_ PVOID Handle,
    _In_opt_ PVOID Callback)
{
    ULONG Index;
    BOOL Removed = FALSE;

    for (Index = 0; Index < D3DKMT_MAX_NOTIFICATIONS; Index++)
    {
        D3DKMT_NOTIFICATION* Entry = &Table[Index];

        if (!Entry->InUse)
            continue;

        /* Without a handle every registration of that callback goes, which is how a
           DLL being unloaded drops the ones it can no longer name */
        if (Handle != NULL ? (Entry != Handle) : (Entry->Callback != Callback))
            continue;

        Entry->Callback = NULL;
        Entry->Context = NULL;
        InterlockedExchange(&Entry->InUse, 0);
        Removed = TRUE;

        if (Handle != NULL)
            break;
    }

    return Removed;
}

NTSTATUS
WINAPI
D3DKMTRegisterTrimNotification(_Inout_ D3DKMT_REGISTERTRIMNOTIFICATION* unnamedParam1)
{
    D3DKMT_NOTIFICATION* Entry;

    if (!unnamedParam1 || !unnamedParam1->Callback)
        return STATUS_INVALID_PARAMETER;

    Entry = D3DKMTAddNotification(D3DKMTTrimNotifications,
                                  (PVOID)unnamedParam1->Callback,
                                  unnamedParam1->Context);
    if (!Entry)
        return STATUS_INSUFFICIENT_RESOURCES;

    unnamedParam1->Handle = Entry;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTUnregisterTrimNotification(_Inout_ D3DKMT_UNREGISTERTRIMNOTIFICATION* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!unnamedParam1->Handle && !unnamedParam1->Callback)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMTRemoveNotification(D3DKMTTrimNotifications,
                                  unnamedParam1->Handle,
                                  (PVOID)unnamedParam1->Callback))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTRegisterBudgetChangeNotification(_Inout_ D3DKMT_REGISTERBUDGETCHANGENOTIFICATION* unnamedParam1)
{
    D3DKMT_NOTIFICATION* Entry;

    if (!unnamedParam1 || !unnamedParam1->Callback)
        return STATUS_INVALID_PARAMETER;

    Entry = D3DKMTAddNotification(D3DKMTBudgetNotifications,
                                  (PVOID)unnamedParam1->Callback,
                                  unnamedParam1->Context);
    if (!Entry)
        return STATUS_INSUFFICIENT_RESOURCES;

    unnamedParam1->Handle = Entry;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
D3DKMTUnregisterBudgetChangeNotification(_Inout_ D3DKMT_UNREGISTERBUDGETCHANGENOTIFICATION* unnamedParam1)
{
    if (!unnamedParam1 || !unnamedParam1->Handle)
        return STATUS_INVALID_PARAMETER;

    if (!D3DKMTRemoveNotification(D3DKMTBudgetNotifications, unnamedParam1->Handle, NULL))
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}
