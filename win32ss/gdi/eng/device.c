/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS kernel
 * PURPOSE:           GDI Driver Device Functions
 * FILE:              win32ss/gdi/eng/device.c
 * PROGRAMER:         Jason Filby
 *                    Timo Kreuzer
 */

#include <win32k.h>
#include <ntddvdeo.h>
#include <reactos/rddm/rddm_private.h>

DBG_DEFAULT_CHANNEL(EngDev);

static PGRAPHICS_DEVICE gpPrimaryGraphicsDevice;
static PGRAPHICS_DEVICE gpVgaGraphicsDevice;

static PGRAPHICS_DEVICE gpGraphicsDeviceFirst = NULL;
static PGRAPHICS_DEVICE gpGraphicsDeviceLast = NULL;
static HSEMAPHORE ghsemGraphicsDeviceList;
static ULONG giDevNum = 1;

/* \Device\VideoN numbers that failed to open. They are not tried again, the way Windows skips them */
#define ENGP_MAX_VIDEO_NUMBERS  256
static RTL_BITMAP gFailedVideoNumbers;
static ULONG gFailedVideoNumberBits[ENGP_MAX_VIDEO_NUMBERS / 32];

/* TRUE once dxgkrnl applied a display configuration, which then decides the desktop's displays */
static BOOLEAN gbDisplayConfigApplied;
CODE_SEG("INIT")
NTSTATUS
NTAPI
InitDeviceImpl(VOID)
{
    ghsemGraphicsDeviceList = EngCreateSemaphore();
    if (!ghsemGraphicsDeviceList)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitializeBitMap(&gFailedVideoNumbers, gFailedVideoNumberBits, ENGP_MAX_VIDEO_NUMBERS);
    RtlClearAllBits(&gFailedVideoNumbers);

    return STATUS_SUCCESS;
}

static
BOOLEAN
EngpHasVgaDriver(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    WCHAR awcDeviceKey[256], awcServiceName[100];
    PWSTR lastBkSlash;
    NTSTATUS Status;
    ULONG cbValue;
    HKEY hkey;

    /* Open the key for the adapters */
    Status = RegOpenKey(L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\VIDEO", &hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open HARDWARE\\DEVICEMAP\\VIDEO registry key: 0x%08lx\n", Status);
        return FALSE;
    }

    /* Read the name of the device key */
    cbValue = sizeof(awcDeviceKey);
    Status = RegQueryValue(hkey, pGraphicsDevice->szNtDeviceName, REG_SZ, awcDeviceKey, &cbValue);
    ZwClose(hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not read '%S' registry value: 0x%08lx\n", Status);
        return FALSE;
    }

    /* Replace 'DeviceN' by 'Video' */
    lastBkSlash = wcsrchr(awcDeviceKey, L'\\');
    if (!lastBkSlash)
    {
        ERR("Invalid registry key '%S'\n", lastBkSlash);
        return FALSE;
    }
    if (!NT_SUCCESS(RtlStringCchCopyW(lastBkSlash + 1,
                                      ARRAYSIZE(awcDeviceKey) - (lastBkSlash + 1 - awcDeviceKey),
                                      L"Video")))
    {
        ERR("Failed to add 'Video' to registry key '%S'\n", awcDeviceKey);
        return FALSE;
    }

    /* Open device key */
    Status = RegOpenKey(awcDeviceKey, &hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open %S registry key: 0x%08lx\n", awcDeviceKey, Status);
        return FALSE;
    }

    /* Read service name */
    cbValue = sizeof(awcServiceName);
    Status = RegQueryValue(hkey, L"Service", REG_SZ, awcServiceName, &cbValue);
    ZwClose(hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not read Service registry value in %S: 0x%08lx\n", awcDeviceKey, Status);
        return FALSE;
    }

    /* Device is using VGA driver if service name is 'VGASave' (case insensitive) */
    return (_wcsicmp(awcServiceName, L"VGASave") == 0);
}

/*
 * Add a device to gpGraphicsDeviceFirst/gpGraphicsDeviceLast list (if not already present).
 */
_Requires_lock_held_(ghsemGraphicsDeviceList)
static
VOID
EngpLinkGraphicsDevice(
    _In_ PGRAPHICS_DEVICE pToAdd)
{
    PGRAPHICS_DEVICE pGraphicsDevice;

    TRACE("EngLinkGraphicsDevice(%p)\n", pToAdd);

    /* Search if device is not already linked */
    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if (pGraphicsDevice == pToAdd)
            return;
    }

    pToAdd->pNextGraphicsDevice = NULL;
    if (gpGraphicsDeviceLast)
        gpGraphicsDeviceLast->pNextGraphicsDevice = pToAdd;
    gpGraphicsDeviceLast = pToAdd;
    if (!gpGraphicsDeviceFirst)
        gpGraphicsDeviceFirst = pToAdd;
}

/*
 * Remove a device from gpGraphicsDeviceFirst/gpGraphicsDeviceLast list.
 */
_Requires_lock_held_(ghsemGraphicsDeviceList)
static
VOID
EngpUnlinkGraphicsDevice(
    _In_ PGRAPHICS_DEVICE pToDelete)
{
    PGRAPHICS_DEVICE pPrevGraphicsDevice = NULL;
    PGRAPHICS_DEVICE pGraphicsDevice = gpGraphicsDeviceFirst;

    TRACE("EngpUnlinkGraphicsDevice('%S')\n", pToDelete->szNtDeviceName);

    while (pGraphicsDevice)
    {
        if (pGraphicsDevice != pToDelete)
        {
            /* Keep current device */
            pPrevGraphicsDevice = pGraphicsDevice;
            pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice;
        }
        else
        {
            /* At first, link again associated VGA Device */
            if (pGraphicsDevice->pVgaDevice)
                EngpLinkGraphicsDevice(pGraphicsDevice->pVgaDevice);

            /* We need to remove current device */
            pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice;

            /* Unlink chain */
            if (!pPrevGraphicsDevice)
                gpGraphicsDeviceFirst = pToDelete->pNextGraphicsDevice;
            else
                pPrevGraphicsDevice->pNextGraphicsDevice = pToDelete->pNextGraphicsDevice;
            if (gpGraphicsDeviceLast == pToDelete)
                gpGraphicsDeviceLast = pPrevGraphicsDevice;
        }
    }
}

/* Goal of this function is to:
 * - detect new graphic devices (from registry) and initialize them
 * - link primary device and VGA device (if available) using pVgaDevice field
 * - handle gbBaseVideo global flag
 * - set DISPLAY_DEVICE_PRIMARY_DEVICE on at least one device
 * - set gpPrimaryGraphicsDevice
 * - set gpVgaGraphicsDevice
 */
NTSTATUS
EngpUpdateGraphicsDeviceList(VOID)
{
    ULONG iDevNum, ulMaxObjectNumber = 0;
    WCHAR awcDeviceName[20];
    UNICODE_STRING ustrDeviceName;
    WCHAR awcBuffer[256];
    NTSTATUS Status;
    PGRAPHICS_DEVICE pGraphicsDevice, pNewPrimaryGraphicsDevice = NULL;
    ULONG cbValue;
    HKEY hkey;

    /* Open the key for the adapters */
    Status = RegOpenKey(L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\VIDEO", &hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open HARDWARE\\DEVICEMAP\\VIDEO registry key:0x%lx\n", Status);
        return Status;
    }

    /* Get the maximum number of adapters */
    if (!RegReadDWORD(hkey, L"MaxObjectNumber", &ulMaxObjectNumber))
    {
        ERR("Could not read MaxObjectNumber, defaulting to 0.\n");
    }

    TRACE("Found %lu devices\n", ulMaxObjectNumber + 1);

    /* Loop through all adapters, to detect new ones */
    for (iDevNum = 0; iDevNum <= ulMaxObjectNumber; iDevNum++)
    {
        /* A number that failed once stays skipped, same as a WDDM adapter nobody started */
        if ((iDevNum < ENGP_MAX_VIDEO_NUMBERS) && RtlCheckBit(&gFailedVideoNumbers, iDevNum))
            continue;

        /* Create the adapter's key name */
        _swprintf(awcDeviceName, L"\\Device\\Video%lu", iDevNum);
        RtlInitUnicodeString(&ustrDeviceName, awcDeviceName);

        /* Check if the device exists already */
        if (EngpFindGraphicsDeviceByNtName(&ustrDeviceName) != NULL)
            continue;

        /* Read the reg key name */
        cbValue = sizeof(awcBuffer);
        Status = RegQueryValue(hkey, awcDeviceName, REG_SZ, awcBuffer, &cbValue);
        if (!NT_SUCCESS(Status))
        {
            ERR("failed to query the registry path:0x%lx\n", Status);
            continue;
        }

        /* Initialize the driver for this device */
        pGraphicsDevice = InitDisplayDriver(awcDeviceName, awcBuffer);
        if (!pGraphicsDevice)
        {
            if (iDevNum < ENGP_MAX_VIDEO_NUMBERS)
                RtlSetBit(&gFailedVideoNumbers, iDevNum);
            continue;
        }
    }

    /* Close the device map registry key */
    ZwClose(hkey);

    /* Choose a VGA device */
    /* Try a device with DISPLAY_DEVICE_VGA_COMPATIBLE flag. If not found,
     * fall back to current VGA device */
    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if (pGraphicsDevice == gpVgaGraphicsDevice)
            continue;
        if (pGraphicsDevice->StateFlags & DISPLAY_DEVICE_VGA_COMPATIBLE && EngpHasVgaDriver(pGraphicsDevice))
        {
            gpVgaGraphicsDevice = pGraphicsDevice;
            break;
        }
    }

    /* Handle gbBaseVideo */
    if (gbBaseVideo)
    {
        PGRAPHICS_DEVICE pToDelete;

        /* Lock list */
        EngAcquireSemaphore(ghsemGraphicsDeviceList);

        /* Remove every device from linked list, except base-video one */
        pGraphicsDevice = gpGraphicsDeviceFirst;
        while (pGraphicsDevice)
        {
            if (!EngpHasVgaDriver(pGraphicsDevice))
            {
                /* Not base-video device. Remove it */
                pToDelete = pGraphicsDevice;
                TRACE("Removing non-base-video device %S (%S)\n", pToDelete->szWinDeviceName, pToDelete->szNtDeviceName);

                EngpUnlinkGraphicsDevice(pGraphicsDevice);
                pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice;

                /* Free memory */
                ExFreePoolWithTag(pToDelete->pDiplayDrivers, GDITAG_DRVSUP);
                ExFreePoolWithTag(pToDelete, GDITAG_GDEVICE);
            }
            else
            {
                pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice;
            }
        }

        /* Unlock list */
        EngReleaseSemaphore(ghsemGraphicsDeviceList);
    }

    /* Choose a primary device (if none already exists) */
    if (!gpPrimaryGraphicsDevice)
    {
        for (pGraphicsDevice = gpGraphicsDeviceFirst;
             pGraphicsDevice;
             pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
        {
            if (!EngpHasVgaDriver(pGraphicsDevice))
            {
                pNewPrimaryGraphicsDevice = pGraphicsDevice;
                break;
            }
        }
        if (!pNewPrimaryGraphicsDevice)
            pNewPrimaryGraphicsDevice = gpGraphicsDeviceFirst;
        if (pNewPrimaryGraphicsDevice)
        {
            pNewPrimaryGraphicsDevice->StateFlags |= DISPLAY_DEVICE_PRIMARY_DEVICE;
            gpPrimaryGraphicsDevice = pNewPrimaryGraphicsDevice;
        }
    }

    /* Can we link VGA device to primary device? */
    if (gpPrimaryGraphicsDevice &&
        gpVgaGraphicsDevice &&
        gpPrimaryGraphicsDevice != gpVgaGraphicsDevice &&
        !gpPrimaryGraphicsDevice->pVgaDevice)
    {
        /* Yes. Remove VGA device from global list, and attach it to primary device */
        TRACE("Linking VGA device %S to primary device %S\n", gpVgaGraphicsDevice->szNtDeviceName, gpPrimaryGraphicsDevice->szNtDeviceName);
        EngAcquireSemaphore(ghsemGraphicsDeviceList);
        EngpUnlinkGraphicsDevice(gpVgaGraphicsDevice);
        gpPrimaryGraphicsDevice->pVgaDevice = gpVgaGraphicsDevice;
        EngReleaseSemaphore(ghsemGraphicsDeviceList);
    }

    return STATUS_SUCCESS;
}

/* Open display settings registry key
 * Returns NULL in case of error. */
static HKEY
EngpGetRegistryHandleFromDeviceMap(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    static const PWCHAR KEY_VIDEO = L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\VIDEO";
    HKEY hKey;
    WCHAR szDeviceKey[256];
    ULONG cbSize;
    NTSTATUS Status;

    /* Open the device map registry key */
    Status = RegOpenKey(KEY_VIDEO, &hKey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open HARDWARE\\DEVICEMAP\\VIDEO registry key: status 0x%08x\n", Status);
        return NULL;
    }

    /* Query the registry path */
    cbSize = sizeof(szDeviceKey);
    RegQueryValue(hKey,
                  pGraphicsDevice->szNtDeviceName,
                  REG_SZ,
                  szDeviceKey,
                  &cbSize);
    ZwClose(hKey);

    /* Open the registry key */
    Status = RegOpenKey(szDeviceKey, &hKey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open registry key '%S': status 0x%08x\n", szDeviceKey, Status);
        return NULL;
    }

    return hKey;
}

/**
 * @brief
 * Reads the name an adapter shows as. The PnP driver key is tried first with DriverDesc, the
 * \Device\VideoN key starts at Device Description. First non-empty string wins.
 * Reference win32kbase DrvGetDeviceConfigurationInformation.
 *
 * @param[in] hKey
 * The PnP driver key, or the key DEVICEMAP\VIDEO names.
 *
 * @param[in] bDriverKey
 * TRUE for the PnP driver key.
 *
 * @param[out] pwszDescription
 * Receives the NUL terminated name.
 *
 * @param[in] cbDescription
 * Size of pwszDescription in bytes.
 *
 * @return
 * TRUE when a name was found.
 */
BOOLEAN
NTAPI
EngpQueryDeviceDescription(
    _In_ HANDLE hKey,
    _In_ BOOLEAN bDriverKey,
    _Out_writes_bytes_(cbDescription) PWSTR pwszDescription,
    _In_ ULONG cbDescription)
{
    static const PCWSTR apwszValueNames[] =
    {
        L"DriverDesc",
        L"Device Description",
        L"HardwareInformation.AdapterString",
        L"HardwareInformation.ChipType"
    };
    NTSTATUS Status;
    ULONG cbValue;
    ULONG i;

    if (cbDescription < 2 * sizeof(WCHAR))
        return FALSE;

    for (i = bDriverKey ? 0 : 1; i < RTL_NUMBER_OF(apwszValueNames); i++)
    {
        cbValue = cbDescription - sizeof(WCHAR);
        Status = RegQueryValue(hKey, apwszValueNames[i], REG_SZ, pwszDescription, &cbValue);
        if (Status == STATUS_OBJECT_TYPE_MISMATCH)
        {
            cbValue = cbDescription - sizeof(WCHAR);
            Status = RegQueryValue(hKey, apwszValueNames[i], REG_MULTI_SZ, pwszDescription, &cbValue);
        }

        if (!NT_SUCCESS(Status) || (cbValue < sizeof(WCHAR)) || (pwszDescription[0] == UNICODE_NULL))
            continue;

        pwszDescription[cbValue / sizeof(WCHAR)] = UNICODE_NULL;
        return TRUE;
    }

    pwszDescription[0] = UNICODE_NULL;
    return FALSE;
}

/**
 * @brief
 * Does EnumDisplaySettings hide the modes the monitor cannot show? Only an explicit
 * PruningMode of 0 turns it off. Reference win32kbase DrvGetPruneFlag.
 */
BOOLEAN
NTAPI
EngpGetPruneFlag(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    DWORD dwPruningMode;
    HKEY hKey;
    BOOLEAN bPrune = TRUE;

    hKey = EngpGetRegistryHandleFromDeviceMap(pGraphicsDevice);
    if (hKey == NULL)
        return TRUE;

    if (RegReadDWORD(hKey, L"PruningMode", &dwPruningMode) && (dwPruningMode == 0))
        bPrune = FALSE;

    ZwClose(hKey);
    return bPrune;
}

NTSTATUS
EngpGetDisplayDriverParameters(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _Out_ PDEVMODEW pdm)
{
    HKEY hKey;
    NTSTATUS Status;
    RTL_QUERY_REGISTRY_TABLE DisplaySettingsTable[] =
    {
#define READ(field, str) \
        { \
            NULL, \
            RTL_QUERY_REGISTRY_DIRECT, \
            L ##str, \
            &pdm->field, \
            REG_NONE, NULL, 0 \
        },
    READ(dmBitsPerPel, "DefaultSettings.BitsPerPel")
    READ(dmPelsWidth, "DefaultSettings.XResolution")
    READ(dmPelsHeight, "DefaultSettings.YResolution")
    READ(dmDisplayFlags, "DefaultSettings.Flags")
    READ(dmDisplayFrequency, "DefaultSettings.VRefresh")
    READ(dmPanningWidth, "DefaultSettings.XPanning")
    READ(dmPanningHeight, "DefaultSettings.YPanning")
    READ(dmDisplayOrientation, "DefaultSettings.Orientation")
    READ(dmDisplayFixedOutput, "DefaultSettings.FixedOutput")
    READ(dmPosition.x, "Attach.RelativeX")
    READ(dmPosition.y, "Attach.RelativeY")
#undef READ
        {0}
    };

    hKey = EngpGetRegistryHandleFromDeviceMap(pGraphicsDevice);
    if (!hKey)
        return STATUS_UNSUCCESSFUL;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                    (PWSTR)hKey,
                                    DisplaySettingsTable,
                                    NULL,
                                    NULL);

    ZwClose(hKey);
    return Status;
}

DWORD
EngpGetDisplayDriverAccelerationLevel(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    HKEY hKey;
    DWORD dwAccelerationLevel = 0;
    RTL_QUERY_REGISTRY_TABLE DisplaySettingsTable[] =
    {
        {
            NULL,
            RTL_QUERY_REGISTRY_DIRECT,
            L"Acceleration.Level",
            &dwAccelerationLevel,
            REG_NONE, NULL, 0
        },
        {0}
    };

    hKey = EngpGetRegistryHandleFromDeviceMap(pGraphicsDevice);
    if (!hKey)
        return 0;

    RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                           (PWSTR)hKey,
                           DisplaySettingsTable,
                           NULL,
                           NULL);
    ZwClose(hKey);

    return dwAccelerationLevel;
}

/* Sends a TargetDeviceRelation request to PDO
 * On success, caller needs to free pDeviceRelations with ExFreePool()
 */
static
NTSTATUS
EngpPnPTargetRelationRequest(
    _In_ PDEVICE_OBJECT pDeviceObject,
    _Out_ PDEVICE_RELATIONS *pDeviceRelations)
{
    PIO_STACK_LOCATION IrpSp;
    KEVENT Event;
    PIRP pIrp;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    /* Initialize an event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Build IRP */
    pIrp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                        pDeviceObject,
                                        NULL, 0,
                                        NULL,
                                        &Event,
                                        &Iosb);
    if (!pIrp)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Initialize IRP */
    pIrp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IrpSp = IoGetNextIrpStackLocation(pIrp);
    IrpSp->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
    IrpSp->Parameters.QueryDeviceRelations.Type = TargetDeviceRelation;

    /* Call the driver */
    Status = IoCallDriver(pDeviceObject, pIrp);

    /* Wait if neccessary */
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, 0);
        Status = Iosb.Status;
    }

    /* Return information to the caller about the operation. */
    if (NT_SUCCESS(Status))
        *pDeviceRelations = (PDEVICE_RELATIONS)Iosb.Information;

    return Status;
}

NTSTATUS
NTAPI
EngpUpdateMonitorDevices(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    PDEVICE_RELATIONS pDeviceRelations;
    PVIDEO_MONITOR_DEVICE pMonitorDevices;
    ULONG i, bytesWritten, monitorCount;
    NTSTATUS Status;
    HANDLE hkRegistry;

    /* Request right PDO for device relations */
    Status = EngpPnPTargetRelationRequest(pGraphicsDevice->DeviceObject, &pDeviceRelations);
    if (!NT_SUCCESS(Status))
    {
        ERR("EngpPnPTargetRelationRequest() failed with status 0x%08x\n", Status);
        return Status;
    }
    ASSERT(pDeviceRelations->Count == 1);

    /* Invalidate relations, so that videoprt reenumerates its monitors.
     * Only do this for valid PDOs - check by trying to open registry key. */
    Status = IoOpenDeviceRegistryKey(pDeviceRelations->Objects[0],
                                     PLUGPLAY_REGKEY_DRIVER,
                                     KEY_READ,
                                     &hkRegistry);
    if (NT_SUCCESS(Status))
    {
        ZwClose(hkRegistry);
        IoSynchronousInvalidateDeviceRelations(pDeviceRelations->Objects[0], BusRelations);
    }
    else
    {
        /* Legacy device without valid PDO - skip invalidation.
         * This is expected for non-PnP devices like VGA. */
    }

    /* Free returned structure */
    for (i = 0; i < pDeviceRelations->Count; i++)
        ObDereferenceObject(pDeviceRelations->Objects[i]);
    ExFreePool(pDeviceRelations);

    /* Now, get list of monitor PDOs */
    Status = EngDeviceIoControl(pGraphicsDevice->DeviceObject,
                                IOCTL_VIDEO_ENUM_MONITOR_PDO,
                                NULL, 0,
                                &pMonitorDevices, sizeof(pMonitorDevices),
                                &bytesWritten);
    if (Status != ERROR_SUCCESS)
    {
        ERR("EngDeviceIoControl(IOCTL_VIDEO_ENUM_MONITOR_PDO) failed with status 0x%08x\n", Status);
        return Status;
    }
    ASSERT(bytesWritten == sizeof(pMonitorDevices));

    /* Count number of available monitors */
    for (monitorCount = 0; pMonitorDevices[monitorCount].pdo; ++monitorCount)
        ;

    if (pGraphicsDevice->pvMonDev)
    {
        /* Erase everything */
        for (i = 0; i < pGraphicsDevice->dwMonCnt; i++)
            ObDereferenceObject(pGraphicsDevice->pvMonDev[i].pdo);
        ExFreePoolWithTag(pGraphicsDevice->pvMonDev, GDITAG_GDEVICE);
        pGraphicsDevice->pvMonDev = NULL;
        pGraphicsDevice->dwMonCnt = 0;
    }

    if (monitorCount > 0)
    {
        pGraphicsDevice->pvMonDev = ExAllocatePoolZero(PagedPool,
                                                       monitorCount * sizeof(VIDEO_MONITOR_DEVICE),
                                                       GDITAG_GDEVICE);
        if (!pGraphicsDevice->pvMonDev)
        {
            for (i = 0; pMonitorDevices[i].pdo; ++i)
                ObDereferenceObject(pMonitorDevices[i].pdo);
            ExFreePool(pMonitorDevices);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* Copy data */
        for (i = 0; i < monitorCount; i++)
        {
            TRACE("%S\\Monitor%u: PDO %p HwID %u\n", pGraphicsDevice->szWinDeviceName, i, pMonitorDevices[i].pdo, pMonitorDevices[i].HwID);
            pGraphicsDevice->pvMonDev[pGraphicsDevice->dwMonCnt++] = pMonitorDevices[i];
        }
    }

    ExFreePool(pMonitorDevices);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Asks a display device whether a WDDM adapter drives it.
 *
 * @param[in] pDeviceObject
 * The \Device\VideoN device object.
 *
 * @param[out] pViewInformation
 * Receives the adapter and its LUID.
 *
 * @return
 * TRUE when dxgkrnl answered with an adapter.
 */
static
BOOLEAN
EngpQueryWddmAdapter(
    _In_ PDEVICE_OBJECT pDeviceObject,
    _Out_ PDXGK_GDI_VIEW_INFORMATION pViewInformation)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP pIrp;
    NTSTATUS Status;

    RtlZeroMemory(pViewInformation, sizeof(*pViewInformation));

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    pIrp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_QUERY_GDI_VIEW_INFORMATION,
                                         pDeviceObject,
                                         NULL,
                                         0,
                                         pViewInformation,
                                         sizeof(*pViewInformation),
                                         TRUE,
                                         &Event,
                                         &Iosb);
    if (pIrp == NULL)
        return FALSE;

    Status = IoCallDriver(pDeviceObject, pIrp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    return NT_SUCCESS(Status) && (pViewInformation->Adapter != NULL);
}

/**
 * @brief
 * Registers VideoPortCallout with a display device and learns its physical device object, the way
 * Windows win32k does. Reference win32kbase DrvUpdateGraphicsDeviceList.
 *
 * @param[in] pDeviceObject
 * The \Device\VideoN device object.
 *
 * @param[in,out] pWin32kCallbacks
 * Carries the callout in, receives what the device reports.
 *
 * @return
 * The status the device completed the request with.
 */
static
NTSTATUS
EngpInitWin32kCallbacks(
    _In_ PDEVICE_OBJECT pDeviceObject,
    _Inout_ PVIDEO_WIN32K_CALLBACKS pWin32kCallbacks)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP pIrp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    pIrp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_GDI_INIT_WIN32K_CALLBACKS,
                                         pDeviceObject,
                                         pWin32kCallbacks,
                                         sizeof(*pWin32kCallbacks),
                                         pWin32kCallbacks,
                                         sizeof(*pWin32kCallbacks),
                                         TRUE,
                                         &Event,
                                         &Iosb);
    if (pIrp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(pDeviceObject, pIrp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    return Status;
}

/**
 * @brief
 * Claims a WDDM display device for this session, or gives it back.
 *
 * @param[in] pDeviceObject
 * The \Device\VideoN device object.
 *
 * @param[in] bEnable
 * TRUE to claim the device, FALSE to release it.
 *
 * @return
 * TRUE when the device now belongs to this session, FALSE when another session holds it.
 */
static
BOOL
EngpSetDeviceSessionUsage(
    _In_ PDEVICE_OBJECT pDeviceObject,
    _In_ BOOL bEnable)
{
    DXGK_SESSION_USAGE SessionUsage;
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP pIrp;
    NTSTATUS Status;

    SessionUsage.Enable = bEnable ? 1 : 0;
    SessionUsage.Succeeded = 0;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    pIrp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_SET_SESSION_USAGE,
                                         pDeviceObject,
                                         &SessionUsage,
                                         sizeof(SessionUsage),
                                         &SessionUsage,
                                         sizeof(SessionUsage),
                                         TRUE,
                                         &Event,
                                         &Iosb);
    if (pIrp == NULL)
        return FALSE;

    Status = IoCallDriver(pDeviceObject, pIrp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    return NT_SUCCESS(Status) && (SessionUsage.Succeeded != 0);
}

PGRAPHICS_DEVICE
NTAPI
EngpRegisterGraphicsDevice(
    _In_ PUNICODE_STRING pustrDeviceName,
    _In_ PUNICODE_STRING pustrDiplayDrivers,
    _In_ PUNICODE_STRING pustrDescription)
{
    /* A REG_MULTI_SZ list, so it ends with a second NUL */
    UNICODE_STRING ustrCddDriver = RTL_CONSTANT_STRING(L"cdd\0\0");
    DXGK_GDI_VIEW_INFORMATION ViewInformation;
    PGRAPHICS_DEVICE pGraphicsDevice;
    PDEVICE_OBJECT pDeviceObject;
    PFILE_OBJECT pFileObject;
    NTSTATUS Status;
    VIDEO_WIN32K_CALLBACKS Win32kCallbacks;
    UNICODE_STRING ustrDriverDescription;
    WCHAR awcDescription[128];
    HANDLE hDriverKey;
    PWSTR pwsz;
    ULONG cj;

    TRACE("EngpRegisterGraphicsDevice(%wZ)\n", pustrDeviceName);

    /* Allocate a GRAPHICS_DEVICE structure */
    pGraphicsDevice = ExAllocatePoolZero(PagedPool,
                                         sizeof(GRAPHICS_DEVICE),
                                         GDITAG_GDEVICE);
    if (!pGraphicsDevice)
    {
        ERR("ExAllocatePoolWithTag failed\n");
        return NULL;
    }

    /* Try to open and enable the device */
    Status = IoGetDeviceObjectPointer(pustrDeviceName,
                                      FILE_READ_DATA | FILE_WRITE_DATA,
                                      &pFileObject,
                                      &pDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not open device %wZ, 0x%lx\n", pustrDeviceName, Status);
        ExFreePoolWithTag(pGraphicsDevice, GDITAG_GDEVICE);
        return NULL;
    }

    /* Copy the device and file object pointers */
    pGraphicsDevice->DeviceObject = pDeviceObject;
    pGraphicsDevice->FileObject = pFileObject;

    /* Initialize and register the device with videoprt for Win32k callbacks */
    Win32kCallbacks.PhysDisp = pGraphicsDevice;
    Win32kCallbacks.Callout = VideoPortCallout;
    // Reset the data being returned prior to the call.
    Win32kCallbacks.bACPI = FALSE;
    Win32kCallbacks.pPhysDeviceObject = NULL;
    Win32kCallbacks.DualviewFlags = 0;
    Status = EngpInitWin32kCallbacks(pDeviceObject, &Win32kCallbacks);
    if (!NT_SUCCESS(Status))
    {
        ERR("IOCTL_VIDEO_GDI_INIT_WIN32K_CALLBACKS to %wZ failed, Status 0x%lx\n",
            pustrDeviceName, Status);
    }
    // TODO: Set flags according to the results.
    // if (Win32kCallbacks.bACPI)
    // if (Win32kCallbacks.DualviewFlags & ???)
    pGraphicsDevice->PhysDeviceHandle = Win32kCallbacks.pPhysDeviceObject;

    /* A PnP adapter is named after its driver key, DriverDesc first, like Windows does */
    if (pGraphicsDevice->PhysDeviceHandle != NULL)
    {
        Status = IoOpenDeviceRegistryKey(pGraphicsDevice->PhysDeviceHandle,
                                         PLUGPLAY_REGKEY_DRIVER,
                                         KEY_READ,
                                         &hDriverKey);
        if (NT_SUCCESS(Status))
        {
            if (EngpQueryDeviceDescription(hDriverKey, TRUE, awcDescription, sizeof(awcDescription)))
            {
                RtlInitUnicodeString(&ustrDriverDescription, awcDescription);
                pustrDescription = &ustrDriverDescription;
            }
            ZwClose(hDriverKey);
        }
    }

    /* A WDDM adapter names no display driver of its own, the CDD drives it */
    if (EngpQueryWddmAdapter(pDeviceObject, &ViewInformation))
    {
        TRACE("%wZ is WDDM, adapter %p\n", pustrDeviceName, ViewInformation.Adapter);
        pGraphicsDevice->DxgAdapter = ViewInformation.Adapter;
        pGraphicsDevice->DxgAdapterLuid = ViewInformation.AdapterLuid;
        pGraphicsDevice->VidPnSourceId = ViewInformation.VidPnSourceId;
        pustrDiplayDrivers = &ustrCddDriver;

        /* The CDD only finds its adapter once the session has claimed the device */
        if (!EngpSetDeviceSessionUsage(pDeviceObject, TRUE))
            ERR("%wZ is already in use by another session\n", pustrDeviceName);
    }
    else if (pustrDiplayDrivers->Length == 0)
    {
        ERR("No display driver for %wZ\n", pustrDeviceName);
        ObDereferenceObject(pFileObject);
        ExFreePoolWithTag(pGraphicsDevice, GDITAG_GDEVICE);
        return NULL;
    }

    /* Copy the device name */
    RtlStringCbCopyNW(pGraphicsDevice->szNtDeviceName,
                      sizeof(pGraphicsDevice->szNtDeviceName),
                      pustrDeviceName->Buffer,
                      pustrDeviceName->Length);

    /* Create a Win32 device name (FIXME: virtual devices!) */
    RtlStringCbPrintfW(pGraphicsDevice->szWinDeviceName,
                       sizeof(pGraphicsDevice->szWinDeviceName),
                       L"\\\\.\\DISPLAY%d",
                       (int)giDevNum);

    /* Allocate a buffer for the strings */
    cj = pustrDiplayDrivers->Length + pustrDescription->Length + sizeof(WCHAR);
    pwsz = ExAllocatePoolWithTag(PagedPool, cj, GDITAG_DRVSUP);
    if (!pwsz)
    {
        ERR("Could not allocate string buffer\n");
        ASSERT(FALSE); // FIXME
        ExFreePoolWithTag(pGraphicsDevice, GDITAG_GDEVICE);
        return NULL;
    }

    /* Copy the display driver names */
    pGraphicsDevice->pDiplayDrivers = pwsz;
    RtlCopyMemory(pGraphicsDevice->pDiplayDrivers,
                  pustrDiplayDrivers->Buffer,
                  pustrDiplayDrivers->Length);

    /* Copy the description */
    pGraphicsDevice->pwszDescription = pwsz + pustrDiplayDrivers->Length / sizeof(WCHAR);
    RtlCopyMemory(pGraphicsDevice->pwszDescription,
                  pustrDescription->Buffer,
                  pustrDescription->Length);
    pGraphicsDevice->pwszDescription[pustrDescription->Length/sizeof(WCHAR)] = 0;

    /* Update list of connected monitors */
    EngpUpdateMonitorDevices(pGraphicsDevice);

    /* Lock loader */
    EngAcquireSemaphore(ghsemGraphicsDeviceList);

    /* Insert the device into the global list */
    EngpLinkGraphicsDevice(pGraphicsDevice);

    /* Increment the device number */
    giDevNum++;

    /* Unlock loader */
    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    /* HACK: already in graphic mode; display wallpaper on this new display. A WDDM device waits
     * for the display configuration to put it on the desktop instead. */
    if (ScreenDeviceContext && (pGraphicsDevice->DxgAdapter == NULL))
    {
        UNICODE_STRING DriverName = RTL_CONSTANT_STRING(L"DISPLAY");
        UNICODE_STRING DisplayName;
        HDC hdc;
        RtlInitUnicodeString(&DisplayName, pGraphicsDevice->szWinDeviceName);
        hdc = IntGdiCreateDC(&DriverName, &DisplayName, NULL, NULL, FALSE);
        IntPaintDesktop(hdc);
    }

    return pGraphicsDevice;
}

/**
 * @brief
 * Finds the graphics device a device handle belongs to. This is the handle GDI
 * passes to DrvEnablePDEV, which display drivers hand back to identify themselves.
 *
 * @param[in] hDevObj
 * The device handle.
 *
 * @return
 * The graphics device, or NULL.
 */
PGRAPHICS_DEVICE
NTAPI
EngpFindGraphicsDeviceByHandle(
    _In_ HANDLE hDevObj)
{
    PGRAPHICS_DEVICE pGraphicsDevice;

    EngAcquireSemaphoreShared(ghsemGraphicsDeviceList);

    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if ((HANDLE)pGraphicsDevice->DeviceObject == hDevObj)
            break;
    }

    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    return pGraphicsDevice;
}

PGRAPHICS_DEVICE
NTAPI
EngpFindGraphicsDevice(
    _In_opt_ PUNICODE_STRING pustrDevice,
    _In_ ULONG iDevNum)
{
    UNICODE_STRING ustrCurrent;
    PGRAPHICS_DEVICE pGraphicsDevice;
    ULONG i;
    TRACE("EngpFindGraphicsDevice('%wZ', %lu)\n",
           pustrDevice, iDevNum);

    /* Lock list */
    EngAcquireSemaphoreShared(ghsemGraphicsDeviceList);

    if (pustrDevice && pustrDevice->Buffer)
    {
        /* Find specified video adapter by name */
        for (pGraphicsDevice = gpGraphicsDeviceFirst;
             pGraphicsDevice;
             pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
        {
            /* Compare the device name */
            RtlInitUnicodeString(&ustrCurrent, pGraphicsDevice->szWinDeviceName);
            if (RtlEqualUnicodeString(&ustrCurrent, pustrDevice, FALSE))
            {
                break;
            }
        }

        if (pGraphicsDevice)
        {
            /* Validate selected monitor number */
#if 0
            if (iDevNum >= pGraphicsDevice->dwMonCnt)
                pGraphicsDevice = NULL;
#else
            /* FIXME: dwMonCnt not initialized, see EngpRegisterGraphicsDevice */
#endif
        }
    }
    else
    {
        /* Select video adapter by device number */
        for (pGraphicsDevice = gpGraphicsDeviceFirst, i = 0;
             pGraphicsDevice && i < iDevNum;
             pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice, i++);
    }

    /* Unlock list */
    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    return pGraphicsDevice;
}

/**
 * @brief
 * Finds a graphics device by the \Device\VideoN object it was opened from.
 *
 * @param[in] pustrNtDeviceName
 * The \Device\VideoN name.
 *
 * @return
 * The graphics device, or NULL.
 */
PGRAPHICS_DEVICE
NTAPI
EngpFindGraphicsDeviceByNtName(
    _In_ PCUNICODE_STRING pustrNtDeviceName)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    UNICODE_STRING ustrCurrent;

    EngAcquireSemaphoreShared(ghsemGraphicsDeviceList);

    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        RtlInitUnicodeString(&ustrCurrent, pGraphicsDevice->szNtDeviceName);
        if (RtlEqualUnicodeString(&ustrCurrent, pustrNtDeviceName, TRUE))
            break;
    }

    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    return pGraphicsDevice;
}

/**
 * @brief
 * Has dxgkrnl applied a display configuration that now decides which devices make up the desktop?
 */
BOOLEAN
NTAPI
EngpIsDisplayConfigApplied(VOID)
{
    return gbDisplayConfigApplied && !gbBaseVideo;
}

_Requires_lock_held_(ghsemGraphicsDeviceList)
static
PGRAPHICS_DEVICE
EngpFindDisplayConfigDevice(
    _In_ const ENGP_DISPLAY_PATH *pPath)
{
    PGRAPHICS_DEVICE pGraphicsDevice;

    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if ((pGraphicsDevice->DxgAdapter == NULL) || pGraphicsDevice->bRemoved)
            continue;

        if ((pGraphicsDevice->DxgAdapterLuid.LowPart == pPath->AdapterLuid.LowPart) &&
            (pGraphicsDevice->DxgAdapterLuid.HighPart == pPath->AdapterLuid.HighPart) &&
            (pGraphicsDevice->VidPnSourceId == pPath->VidPnSourceId))
        {
            return pGraphicsDevice;
        }
    }

    return NULL;
}

_Requires_lock_held_(ghsemGraphicsDeviceList)
static
VOID
EngpMoveGraphicsDeviceToFront(
    _In_ PGRAPHICS_DEVICE pToMove)
{
    PGRAPHICS_DEVICE pPrevious = NULL;
    PGRAPHICS_DEVICE pGraphicsDevice;

    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice && (pGraphicsDevice != pToMove);
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        pPrevious = pGraphicsDevice;
    }

    if ((pGraphicsDevice == NULL) || (pPrevious == NULL))
        return;

    pPrevious->pNextGraphicsDevice = pToMove->pNextGraphicsDevice;
    if (gpGraphicsDeviceLast == pToMove)
        gpGraphicsDeviceLast = pPrevious;

    pToMove->pNextGraphicsDevice = gpGraphicsDeviceFirst;
    gpGraphicsDeviceFirst = pToMove;
}

static
BOOLEAN
EngpIsDisplayConfigOrigin(
    _In_ PDEVMODEW pdm)
{
    if (!(pdm->dmFields & DM_POSITION))
        return TRUE;

    return (pdm->dmPosition.x == 0) && (pdm->dmPosition.y == 0);
}

/**
 * @brief
 * Records which WDDM sources the display configuration dxgkrnl applied turned on, and in what mode.
 * The desktop is then built from exactly those, with the source at the desktop origin as primary.
 * Reference win32kbase DrvCreateMDEV, walking the functionalized paths.
 *
 * @param[in] cPaths
 * Number of entries in pPaths.
 *
 * @param[in] pPaths
 * The active paths, one per clone group, in dxgkrnl's order.
 *
 * @return
 * STATUS_UNSUCCESSFUL when no path reaches a device GDI knows, which leaves the previous
 * configuration in place.
 */
NTSTATUS
NTAPI
EngpSetDisplayConfig(
    _In_ ULONG cPaths,
    _In_reads_(cPaths) const ENGP_DISPLAY_PATH *pPaths)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PGRAPHICS_DEVICE pPrimary = NULL;
    PGRAPHICS_DEVICE pOrigin = NULL;
    PGRAPHICS_DEVICE pFirst = NULL;
    PDEVMODEW *ppdmNew;
    ULONG cAttached = 0;
    ULONG cjDevMode;
    ULONG i;

    ppdmNew = ExAllocatePoolZero(PagedPool, (cPaths ? cPaths : 1) * sizeof(*ppdmNew), GDITAG_TEMP);
    if (ppdmNew == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    EngAcquireSemaphore(ghsemGraphicsDeviceList);

    /* Copy each mode first, so a failure leaves the devices as they were */
    for (i = 0; i < cPaths; i++)
    {
        pGraphicsDevice = EngpFindDisplayConfigDevice(&pPaths[i]);
        if (pGraphicsDevice == NULL)
        {
            WARN("No graphics device for adapter %lx:%lx source %lu\n",
                 pPaths[i].AdapterLuid.HighPart, pPaths[i].AdapterLuid.LowPart,
                 pPaths[i].VidPnSourceId);
            continue;
        }

        cjDevMode = pPaths[i].pdm->dmSize + pPaths[i].pdm->dmDriverExtra;
        ppdmNew[i] = ExAllocatePoolWithTag(PagedPool, cjDevMode, GDITAG_DEVMODE);
        if (ppdmNew[i] == NULL)
            break;

        RtlCopyMemory(ppdmNew[i], pPaths[i].pdm, cjDevMode);
        cAttached++;
    }

    if ((i < cPaths) || (cAttached == 0))
    {
        EngReleaseSemaphore(ghsemGraphicsDeviceList);

        for (i = 0; i < cPaths; i++)
        {
            if (ppdmNew[i] != NULL)
                ExFreePoolWithTag(ppdmNew[i], GDITAG_DEVMODE);
        }
        ExFreePoolWithTag(ppdmNew, GDITAG_TEMP);

        return (cAttached == 0) ? STATUS_UNSUCCESSFUL : STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Every WDDM source starts out off the desktop, the paths turn theirs back on */
    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if (pGraphicsDevice->DxgAdapter == NULL)
            continue;

        if (pGraphicsDevice->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            pPrimary = pGraphicsDevice;

        if (pGraphicsDevice->pdmDisplayConfig != NULL)
        {
            ExFreePoolWithTag(pGraphicsDevice->pdmDisplayConfig, GDITAG_DEVMODE);
            pGraphicsDevice->pdmDisplayConfig = NULL;
        }

        pGraphicsDevice->StateFlags &= ~(DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE);
    }

    for (i = 0; i < cPaths; i++)
    {
        if (ppdmNew[i] == NULL)
            continue;

        pGraphicsDevice = EngpFindDisplayConfigDevice(&pPaths[i]);
        pGraphicsDevice->pdmDisplayConfig = ppdmNew[i];

        if (pFirst == NULL)
            pFirst = pGraphicsDevice;
        if ((pOrigin == NULL) && EngpIsDisplayConfigOrigin(ppdmNew[i]))
            pOrigin = pGraphicsDevice;
    }

    /* The primary stays where it was if it kept a path, otherwise it moves to the desktop origin */
    if ((pPrimary == NULL) || (pPrimary->pdmDisplayConfig == NULL))
        pPrimary = (pOrigin != NULL) ? pOrigin : pFirst;

    if (gpPrimaryGraphicsDevice != NULL)
        gpPrimaryGraphicsDevice->StateFlags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;

    pPrimary->StateFlags |= DISPLAY_DEVICE_PRIMARY_DEVICE;
    gpPrimaryGraphicsDevice = pPrimary;
    EngpMoveGraphicsDeviceToFront(pPrimary);

    gbDisplayConfigApplied = TRUE;

    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    ExFreePoolWithTag(ppdmNew, GDITAG_TEMP);

    TRACE("Display configuration: %lu source(s), primary %S\n", cAttached, pPrimary->szNtDeviceName);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Marks every graphics device of an adapter dxgkrnl reported gone, so the next display
 * configuration leaves them out. Reference win32kbase Win32kPnpNotify.
 *
 * @param[in] PhysDisp
 * The adapter's physical device object, as the callout names it.
 *
 * @return
 * TRUE when at least one device was marked.
 */
BOOLEAN
NTAPI
EngpMarkGraphicsDevicesRemoved(
    _In_ PVOID PhysDisp)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    BOOLEAN bFound = FALSE;

    EngAcquireSemaphore(ghsemGraphicsDeviceList);

    for (pGraphicsDevice = gpGraphicsDeviceFirst;
         pGraphicsDevice;
         pGraphicsDevice = pGraphicsDevice->pNextGraphicsDevice)
    {
        if (pGraphicsDevice->PhysDeviceHandle == PhysDisp)
        {
            pGraphicsDevice->bRemoved = TRUE;
            bFound = TRUE;
        }
    }

    EngReleaseSemaphore(ghsemGraphicsDeviceList);

    return bFound;
}

static
VOID
EngpFreeGraphicsDevice(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    ULONG i;

    if (pGraphicsDevice->DxgAdapter != NULL)
        EngpSetDeviceSessionUsage(pGraphicsDevice->DeviceObject, FALSE);

    if (pGraphicsDevice->pvMonDev != NULL)
    {
        for (i = 0; i < pGraphicsDevice->dwMonCnt; i++)
            ObDereferenceObject(pGraphicsDevice->pvMonDev[i].pdo);
        ExFreePoolWithTag(pGraphicsDevice->pvMonDev, GDITAG_GDEVICE);
    }

    if (pGraphicsDevice->pdmDisplayConfig != NULL)
        ExFreePoolWithTag(pGraphicsDevice->pdmDisplayConfig, GDITAG_DEVMODE);

    if (pGraphicsDevice->pDiplayDrivers != NULL)
        ExFreePoolWithTag(pGraphicsDevice->pDiplayDrivers, GDITAG_DRVSUP);

    if (pGraphicsDevice->FileObject != NULL)
        ObDereferenceObject(pGraphicsDevice->FileObject);

    ExFreePoolWithTag(pGraphicsDevice, GDITAG_GDEVICE);
}

/**
 * @brief
 * Drops the graphics devices of an adapter that is gone, once the desktop no longer uses them.
 * Reference win32kbase DrvCleanupGraphicsDevices.
 *
 * @param[in] PhysDisp
 * The adapter's physical device object.
 */
VOID
NTAPI
EngpCleanupGraphicsDevices(
    _In_ PVOID PhysDisp)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PGRAPHICS_DEVICE pPrevious = NULL;
    PGRAPHICS_DEVICE pNext;

    EngAcquireSemaphore(ghsemGraphicsDeviceList);

    for (pGraphicsDevice = gpGraphicsDeviceFirst; pGraphicsDevice; pGraphicsDevice = pNext)
    {
        pNext = pGraphicsDevice->pNextGraphicsDevice;

        if (pGraphicsDevice->PhysDeviceHandle != PhysDisp)
        {
            pPrevious = pGraphicsDevice;
            continue;
        }

        /* A PDEV still pointing at it keeps it alive, it is only taken off the list */
        if (pPrevious != NULL)
            pPrevious->pNextGraphicsDevice = pNext;
        else
            gpGraphicsDeviceFirst = pNext;
        if (gpGraphicsDeviceLast == pGraphicsDevice)
            gpGraphicsDeviceLast = pPrevious;

        if (gpPrimaryGraphicsDevice == pGraphicsDevice)
            gpPrimaryGraphicsDevice = NULL;
        if (gpVgaGraphicsDevice == pGraphicsDevice)
            gpVgaGraphicsDevice = NULL;

        if (PDEVOBJ_bIsGraphicsDeviceInUse(pGraphicsDevice))
        {
            WARN("%S is gone but a PDEV still uses it\n", pGraphicsDevice->szNtDeviceName);
            continue;
        }

        TRACE("Dropping graphics device %S\n", pGraphicsDevice->szNtDeviceName);
        EngpFreeGraphicsDevice(pGraphicsDevice);
    }

    EngReleaseSemaphore(ghsemGraphicsDeviceList);
}

static
NTSTATUS
EngpFileIoRequest(
    _In_ PFILE_OBJECT pFileObject,
    _In_ ULONG ulMajorFunction,
    _In_reads_(nBufferSize) PVOID lpBuffer,
    _In_ SIZE_T nBufferSize,
    _In_ ULONGLONG ullStartOffset,
    _Out_ PULONG_PTR lpInformation)
{
    PDEVICE_OBJECT pDeviceObject;
    KEVENT Event;
    PIRP pIrp;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    LARGE_INTEGER liStartOffset;

    /* Get corresponding device object */
    pDeviceObject = IoGetRelatedDeviceObject(pFileObject);
    if (!pDeviceObject)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Initialize an event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Build IRP */
    liStartOffset.QuadPart = ullStartOffset;
    pIrp = IoBuildSynchronousFsdRequest(ulMajorFunction,
                                        pDeviceObject,
                                        lpBuffer,
                                        (ULONG)nBufferSize,
                                        &liStartOffset,
                                        &Event,
                                        &Iosb);
    if (!pIrp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Call the driver */
    Status = IoCallDriver(pDeviceObject, pIrp);

    /* Wait if neccessary. The IRP owns the event on our stack until it completes */
    if (STATUS_PENDING == Status)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, 0);
        Status = Iosb.Status;
    }

    /* Return information to the caller about the operation. */
    *lpInformation = Iosb.Information;

    /* Return NTSTATUS */
    return Status;
}

VOID
APIENTRY
EngFileWrite(
    _In_ PFILE_OBJECT pFileObject,
    _In_reads_(nLength) PVOID lpBuffer,
    _In_ SIZE_T nLength,
    _Out_ PSIZE_T lpBytesWritten)
{
    NTSTATUS status;

    status = EngpFileIoRequest(pFileObject,
                               IRP_MJ_WRITE,
                               lpBuffer,
                               nLength,
                               0,
                               lpBytesWritten);
    if (!NT_SUCCESS(status))
    {
        *lpBytesWritten = 0;
    }
}

_Success_(return>=0)
NTSTATUS
APIENTRY
EngFileIoControl(
    _In_ PFILE_OBJECT pFileObject,
    _In_ DWORD dwIoControlCode,
    _In_reads_(nInBufferSize) PVOID lpInBuffer,
    _In_ SIZE_T nInBufferSize,
    _Out_writes_(nOutBufferSize) PVOID lpOutBuffer,
    _In_ SIZE_T nOutBufferSize,
    _Out_ PULONG_PTR lpInformation)
{
    PDEVICE_OBJECT pDeviceObject;
    KEVENT Event;
    PIRP pIrp;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    /* Get corresponding device object */
    pDeviceObject = IoGetRelatedDeviceObject(pFileObject);
    if (!pDeviceObject)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Initialize an event */
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    /* Build IO control IRP */
    pIrp = IoBuildDeviceIoControlRequest(dwIoControlCode,
                                         pDeviceObject,
                                         lpInBuffer,
                                         (ULONG)nInBufferSize,
                                         lpOutBuffer,
                                         (ULONG)nOutBufferSize,
                                         FALSE,
                                         &Event,
                                         &Iosb);
    if (!pIrp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Call the driver */
    Status = IoCallDriver(pDeviceObject, pIrp);

    /* Wait if neccessary. The IRP owns the event on our stack until it completes */
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, 0);
        Status = Iosb.Status;
    }

    /* Return information to the caller about the operation. */
    *lpInformation = Iosb.Information;

    /* This function returns NTSTATUS */
    return Status;
}

/*
 * @implemented
 */
_Success_(return==0)
DWORD
APIENTRY
EngDeviceIoControl(
    _In_ HANDLE hDevice,
    _In_ DWORD dwIoControlCode,
    _In_reads_bytes_opt_(cjInBufferSize) LPVOID lpInBuffer,
    _In_ DWORD cjInBufferSize,
    _Out_writes_bytes_opt_(cjOutBufferSize) LPVOID lpOutBuffer,
    _In_ DWORD cjOutBufferSize,
    _Out_ LPDWORD lpBytesReturned)
{
    PIRP Irp;
    NTSTATUS Status;
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PDEVICE_OBJECT DeviceObject;

    TRACE("EngDeviceIoControl() called\n");

    if (!hDevice)
    {
        return ERROR_INVALID_HANDLE;
    }

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    DeviceObject = (PDEVICE_OBJECT) hDevice;

    Irp = IoBuildDeviceIoControlRequest(dwIoControlCode,
                                        DeviceObject,
                                        lpInBuffer,
                                        cjInBufferSize,
                                        lpOutBuffer,
                                        cjOutBufferSize,
                                        FALSE,
                                        &Event,
                                        &Iosb);
    if (!Irp) return ERROR_NOT_ENOUGH_MEMORY;

    Status = IoCallDriver(DeviceObject, Irp);

    if (Status == STATUS_PENDING)
    {
        /* An alert leaves the IRP holding the event on our stack, so keep waiting */
        do
        {
            Status = KeWaitForSingleObject(&Event, Executive, KernelMode, TRUE, 0);
        } while (Status == STATUS_ALERTED);

        Status = Iosb.Status;
    }

    TRACE("EngDeviceIoControl(): Returning %X/%X\n", Iosb.Status,
           Iosb.Information);

    /* Return information to the caller about the operation. */
    *lpBytesReturned = (DWORD)Iosb.Information;

    /* Convert NT status values to win32 error codes. */
    switch (Status)
    {
        case STATUS_INSUFFICIENT_RESOURCES:
            return ERROR_NOT_ENOUGH_MEMORY;

        case STATUS_BUFFER_OVERFLOW:
            return ERROR_MORE_DATA;

        case STATUS_NOT_IMPLEMENTED:
            return ERROR_INVALID_FUNCTION;

        case STATUS_INVALID_PARAMETER:
            return ERROR_INVALID_PARAMETER;

        case STATUS_BUFFER_TOO_SMALL:
            return ERROR_INSUFFICIENT_BUFFER;

        case STATUS_DEVICE_DOES_NOT_EXIST:
            return ERROR_DEV_NOT_EXIST;

        case STATUS_PENDING:
            return ERROR_IO_PENDING;
    }

    return Status;
}

/*
 * GDI drivers that import win32k (e.g. the CDD) may not also import ntoskrnl (see the
 * GdiLink/NormalLink check in MiResolveImageReferences), so win32k does the \Device\DxgKrnl
 * open on their behalf; callers then use EngDeviceIoControl() with the returned handle.
 */
HANDLE
APIENTRY
EngOpenDxgkrnl(
    _Out_ HANDLE *phFileObject)
{
    UNICODE_STRING DeviceName;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;

    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    Status = IoGetDeviceObjectPointer(&DeviceName,
                                      GENERIC_READ | GENERIC_WRITE,
                                      &FileObject,
                                      &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        *phFileObject = NULL;
        return NULL;
    }

    *phFileObject = (HANDLE)FileObject;
    return (HANDLE)DeviceObject;
}

/*
 * @brief Release a device handle obtained from EngOpenDxgkrnl().
 */
VOID
APIENTRY
EngCloseDxgkrnl(
    _In_ HANDLE hFileObject)
{
    if (hFileObject)
        ObDereferenceObject((PFILE_OBJECT)hFileObject);
}

/* EOF */
