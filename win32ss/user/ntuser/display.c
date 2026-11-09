/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Video initialization and display settings
 * FILE:             win32ss/user/ntuser/display.c
 * PROGRAMER:        Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>
DBG_DEFAULT_CHANNEL(UserDisplay);

/* DPRINT1 for what bring-up logs need to show in release builds too */
#define NDEBUG
#include <debug.h>

PCHAR NTAPI PsGetProcessImageFileName(_In_ PEPROCESS Process);

BOOL gbBaseVideo = FALSE;
static PPROCESSINFO gpFullscreen = NULL;

/*
 * A WDDM mode saved but not set (CDS_UPDATEREGISTRY with CDS_NORESET or CDS_TEST) only sits in
 * dxgkrnl's CDS journal, which the next apply of the saved configuration commits. The saved modes
 * GDI keeps do not know it yet, so going back to the saved mode is not skipped meanwhile.
 * Changed under the exclusive user lock.
 */
static BOOLEAN gbCdsJournalPending = FALSE;

/*
 * The mode of a display device that is not part of the desktop: no size, at no position. Filled
 * once in InitVideo and never written again, because NtUserEnumDisplaySettings copies it out after
 * it has left the user lock, which it only held shared.
 */
static DEVMODEW gdmDetached;

/* TRUE once InitVideo succeeded. Display callouts before that are turned away */
BOOL gbVideoInitialized = FALSE;

/* Signaled once the input desktop exists, which PnP and display switch callouts wait for */
static KEVENT gVideoPortCalloutReady;

static const PWCHAR KEY_VIDEO = L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\VIDEO";

VOID
RegWriteDisplaySettings(HKEY hkey, PDEVMODEW pdm)
{
    RegWriteDWORD(hkey, L"DefaultSettings.BitsPerPel", pdm->dmBitsPerPel);
    RegWriteDWORD(hkey, L"DefaultSettings.XResolution", pdm->dmPelsWidth);
    RegWriteDWORD(hkey, L"DefaultSettings.YResolution", pdm->dmPelsHeight);
    RegWriteDWORD(hkey, L"DefaultSettings.Flags", pdm->dmDisplayFlags);
    RegWriteDWORD(hkey, L"DefaultSettings.VRefresh", pdm->dmDisplayFrequency);
    RegWriteDWORD(hkey, L"DefaultSettings.XPanning", pdm->dmPanningWidth);
    RegWriteDWORD(hkey, L"DefaultSettings.YPanning", pdm->dmPanningHeight);
    RegWriteDWORD(hkey, L"DefaultSettings.Orientation", pdm->dmDisplayOrientation);
    RegWriteDWORD(hkey, L"DefaultSettings.FixedOutput", pdm->dmDisplayFixedOutput);
    RegWriteDWORD(hkey, L"Attach.RelativeX", pdm->dmPosition.x);
    RegWriteDWORD(hkey, L"Attach.RelativeY", pdm->dmPosition.y);
//    RegWriteDWORD(hkey, L"Attach.ToDesktop, pdm->dmBitsPerPel", pdm->);
}

VOID
RegReadDisplaySettings(HKEY hkey, PDEVMODEW pdm)
{
    DWORD dwValue;

    /* Zero out the structure */
    RtlZeroMemory(pdm, sizeof(DEVMODEW));

/* Helper macro */
#define READ(field, str, flag) \
    if (RegReadDWORD(hkey, L##str, &dwValue)) \
    { \
        pdm->field = dwValue; \
        pdm->dmFields |= flag; \
    }

    /* Read all present settings */
    READ(dmBitsPerPel, "DefaultSettings.BitsPerPel", DM_BITSPERPEL);
    READ(dmPelsWidth, "DefaultSettings.XResolution", DM_PELSWIDTH);
    READ(dmPelsHeight, "DefaultSettings.YResolution", DM_PELSHEIGHT);
    READ(dmDisplayFlags, "DefaultSettings.Flags", DM_DISPLAYFLAGS);
    READ(dmDisplayFrequency, "DefaultSettings.VRefresh", DM_DISPLAYFREQUENCY);
    READ(dmPanningWidth, "DefaultSettings.XPanning", DM_PANNINGWIDTH);
    READ(dmPanningHeight, "DefaultSettings.YPanning", DM_PANNINGHEIGHT);
    READ(dmDisplayOrientation, "DefaultSettings.Orientation", DM_DISPLAYORIENTATION);
    READ(dmDisplayFixedOutput, "DefaultSettings.FixedOutput", DM_DISPLAYFIXEDOUTPUT);
    READ(dmPosition.x, "Attach.RelativeX", DM_POSITION);
    READ(dmPosition.y, "Attach.RelativeY", DM_POSITION);
}

PGRAPHICS_DEVICE
NTAPI
InitDisplayDriver(
    IN PWSTR pwszDeviceName,
    IN PWSTR pwszRegKey)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    UNICODE_STRING ustrDeviceName, ustrDisplayDrivers, ustrDescription;
    NTSTATUS Status;
    WCHAR awcBuffer[128];
    ULONG cbSize;
    HKEY hkey;
    DWORD dwVga;

    TRACE("InitDisplayDriver(%S, %S);\n",
          pwszDeviceName, pwszRegKey);

    /* Open the driver's registry key */
    Status = RegOpenKey(pwszRegKey, &hkey);
    if (!NT_SUCCESS(Status))
    {
        ERR("Failed to open registry key: %ls\n", pwszRegKey);
        return NULL;
    }

    /*
     * Query the diplay drivers. A WDDM adapter names none, the CDD drives it instead,
     * so leave the decision to EngpRegisterGraphicsDevice.
     */
    cbSize = sizeof(awcBuffer) - 10;
    Status = RegQueryValue(hkey,
                           L"InstalledDisplayDrivers",
                           REG_MULTI_SZ,
                           awcBuffer,
                           &cbSize);
    if (!NT_SUCCESS(Status))
    {
        TRACE("Didn't find 'InstalledDisplayDrivers', status = 0x%lx\n", Status);
        cbSize = 0;
    }

    /* Initialize the UNICODE_STRING */
    ustrDisplayDrivers.Buffer = awcBuffer;
    ustrDisplayDrivers.MaximumLength = (USHORT)cbSize;
    ustrDisplayDrivers.Length = (USHORT)cbSize;

    /* Set Buffer for description and size of remaining buffer */
    ustrDescription.Buffer = awcBuffer + (cbSize / sizeof(WCHAR));
    cbSize = sizeof(awcBuffer) - cbSize;

    /* Query the device string. A PnP adapter's driver key takes precedence once it is known */
    if (EngpQueryDeviceDescription(hkey, FALSE, ustrDescription.Buffer, cbSize))
        RtlInitUnicodeString(&ustrDescription, ustrDescription.Buffer);
    else
        RtlInitUnicodeString(&ustrDescription, L"<unknown>");

    /* Query if this is a VGA compatible driver */
    cbSize = sizeof(DWORD);
    Status = RegQueryValue(hkey, L"VgaCompatible", REG_DWORD, &dwVga, &cbSize);
    if (!NT_SUCCESS(Status)) dwVga = 0;

    /* Close the registry key */
    ZwClose(hkey);

    /* Register the device with GDI */
    RtlInitUnicodeString(&ustrDeviceName, pwszDeviceName);
    pGraphicsDevice = EngpRegisterGraphicsDevice(&ustrDeviceName,
                                                 &ustrDisplayDrivers,
                                                 &ustrDescription);
    if (pGraphicsDevice && dwVga)
    {
        pGraphicsDevice->StateFlags |= DISPLAY_DEVICE_VGA_COMPATIBLE;
    }

    return pGraphicsDevice;
}

/* WDDM bootstrap (win32ss/gdi/eng/dxgkrnl.c) - loads dxgkrnl + acquires the D3DKMT callback table. */
NTSTATUS NTAPI DlInitDxgkrnl(VOID);
NTSTATUS NTAPI DlApplyDisplayConfig(_Out_ PMDEVOBJ *ppmdev);

/**
 * @brief
 * Builds the desktop's first MDEV. With WDDM it comes out of the display configuration dxgkrnl
 * applies, otherwise from the display drivers, falling back to base video when those fail.
 */
static
NTSTATUS
UserpCreateDesktopMdev(VOID)
{
    PMDEVOBJ pmdev = NULL;
    LONG lRet;

    /* The session owns its adapters now, so dxgkrnl can commit a VidPn for them */
    if (NT_SUCCESS(DlApplyDisplayConfig(&pmdev)) && (pmdev != NULL))
    {
        gpmdev = pmdev;
        return STATUS_SUCCESS;
    }

    lRet = PDEVOBJ_lChangeDisplaySettings(NULL, NULL, NULL, &gpmdev, TRUE);
    if (lRet != DISP_CHANGE_SUCCESSFUL && !gbBaseVideo)
    {
        ERR("Failed to initialize graphics, switching to base video\n");
        gbBaseVideo = TRUE;
        EngpUpdateGraphicsDeviceList();
        lRet = PDEVOBJ_lChangeDisplaySettings(NULL, NULL, NULL, &gpmdev, TRUE);
        gbBaseVideo = FALSE;
        EngpUpdateGraphicsDeviceList();
    }
    if (lRet != DISP_CHANGE_SUCCESSFUL)
    {
        ERR("PDEVOBJ_lChangeDisplaySettings() failed %ld\n", lRet);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
InitVideo(VOID)
{
    NTSTATUS Status;
    HKEY hkey;

    TRACE("----------------------------- InitVideo() -------------------------------\n");

    KeInitializeEvent(&gVideoPortCalloutReady, NotificationEvent, FALSE);

    gdmDetached.dmSize = sizeof(gdmDetached);
    gdmDetached.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                           DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS | DM_DISPLAYORIENTATION;

    /* Check if VGA mode is requested, by finding the special volatile key created by VIDEOPRT */
    Status = RegOpenKey(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\GraphicsDrivers\\BaseVideo", &hkey);
    if (NT_SUCCESS(Status))
        ZwClose(hkey);
    gbBaseVideo = NT_SUCCESS(Status);
    if (gbBaseVideo)
        ERR("VGA mode requested.\n");

    /*
     * Bring up the WDDM stack: load dxgkrnl and acquire the D3DKMT callback table. Non-fatal - if
     * dxgkrnl is absent or unsupported (no WDDM miniport), win32k continues on the legacy XPDM path.
     */
    Status = DlInitDxgkrnl();
    if (!NT_SUCCESS(Status))
        ERR("WDDM dxgkrnl init failed (0x%lX); using legacy display path.\n", Status);

    /* Initialize all display devices */
    Status = EngpUpdateGraphicsDeviceList();
    if (!NT_SUCCESS(Status))
        return Status;

    /* The desktop's displays are settled here, before the first GUI thread needs them */
    Status = UserpCreateDesktopMdev();
    if (!NT_SUCCESS(Status))
        return Status;

    PDEVOBJ_vGetDeviceCaps(gpmdev->ppdevGlobal, &GdiHandleTable->DevCaps);

    InitSysParams();

    return STATUS_SUCCESS;
}

VOID
UserRefreshDisplay(IN PPDEVOBJ ppdev)
{
    ULONG_PTR ulResult;
    // PVOID pvOldCursor;

    // TODO: Re-enable the cursor reset code once this function becomes called
    // from within a Win32 thread... Indeed UserSetCursor() requires this, but
    // at the moment this function is directly called from a separate thread
    // from within videoprt, instead of by a separate win32k system thread.

    if (!ppdev)
        return;

    PDEVOBJ_vReference(ppdev);

    /* Remove mouse pointer */
    // pvOldCursor = UserSetCursor(NULL, TRUE);

    /* Do the mode switch -- Use the actual same current mode */
    ulResult = PDEVOBJ_bSwitchMode(ppdev, ppdev->pdmwDev);
    ASSERT(ulResult);

    /* Restore mouse pointer, no hooks called */
    // pvOldCursor = UserSetCursor(pvOldCursor, TRUE);
    // ASSERT(pvOldCursor == NULL);

    /* Update the system metrics */
    InitMetrics();

    /* Set new size of the monitor */
    // UserUpdateMonitorSize((HDEV)ppdev);

    //co_IntShowDesktop(pdesk, ppdev->gdiinfo.ulHorzRes, ppdev->gdiinfo.ulVertRes);
    UserRedrawDesktop();

    PDEVOBJ_vRelease(ppdev);
}

NTSTATUS
NTAPI
UserEnumDisplayDevices(
    PUNICODE_STRING pustrDevice,
    DWORD iDevNum,
    PDISPLAY_DEVICEW pdispdev,
    DWORD dwFlags)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PDEVICE_OBJECT pdo;
    PWCHAR pHardwareId;
    ULONG cbSize, dwLength;
    HKEY hkey;
    NTSTATUS Status;

    if (!pustrDevice)
    {
        /* Check if some devices have been added since last time */
        EngpUpdateGraphicsDeviceList();
    }

    /* Ask gdi for the GRAPHICS_DEVICE */
    pGraphicsDevice = EngpFindGraphicsDevice(pustrDevice, iDevNum);
    if (!pGraphicsDevice)
    {
        /* No device found */
        if (iDevNum == 0)
            ERR("No GRAPHICS_DEVICE found for '%wZ', iDevNum %lu\n", pustrDevice, iDevNum);
        else
            TRACE("No GRAPHICS_DEVICE found for '%wZ', iDevNum %lu\n", pustrDevice, iDevNum);
        return STATUS_UNSUCCESSFUL;
    }

    if (!pustrDevice)
    {
        pdo = pGraphicsDevice->PhysDeviceHandle;
    }
    else
    {
        EngpUpdateMonitorDevices(pGraphicsDevice);
        if (iDevNum >= pGraphicsDevice->dwMonCnt)
        {
            TRACE("No monitor #%u for '%wZ'\n", iDevNum + 1, pustrDevice);
            return STATUS_UNSUCCESSFUL;
        }
        pdo = pGraphicsDevice->pvMonDev[iDevNum].pdo;
    }


    /* Open the device map registry key */
    Status = RegOpenKey(KEY_VIDEO, &hkey);
    if (!NT_SUCCESS(Status))
    {
        /* No device found */
        ERR("Could not open reg key\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* Query the registry path */
    cbSize = sizeof(pdispdev->DeviceKey);
    RegQueryValue(hkey,
                  pGraphicsDevice->szNtDeviceName,
                  REG_SZ,
                  pdispdev->DeviceKey,
                  &cbSize);

    /* Close registry key */
    ZwClose(hkey);

    /* Copy device name, device string and StateFlags */
    if (!pustrDevice)
    {
        RtlStringCbCopyW(pdispdev->DeviceName, sizeof(pdispdev->DeviceName), pGraphicsDevice->szWinDeviceName);
        RtlStringCbCopyW(pdispdev->DeviceString, sizeof(pdispdev->DeviceString), pGraphicsDevice->pwszDescription);

        /* Only the public bits leave win32k, with whether modes are pruned. */
        pdispdev->StateFlags = pGraphicsDevice->StateFlags & ~DISPLAY_DEVICE_MODESPRUNED;
        if (EngpGetPruneFlag(pGraphicsDevice))
            pdispdev->StateFlags |= DISPLAY_DEVICE_MODESPRUNED;
        pdispdev->StateFlags &= (dwFlags & 2) ? 0x0FFFFFFF : 0x0F2FFFFF;
    }
    else
    {
        _swprintf(pdispdev->DeviceName, L"%ws\\Monitor%u", pGraphicsDevice->szWinDeviceName, iDevNum);
        if (pdo)
        {
            Status = IoGetDeviceProperty(pdo,
                                         DevicePropertyDeviceDescription,
                                         sizeof(pdispdev->DeviceString),
                                         pdispdev->DeviceString,
                                         &dwLength);
            if (!NT_SUCCESS(Status))
                pdispdev->DeviceString[0] = UNICODE_NULL;
        }
        pdispdev->StateFlags = pGraphicsDevice->pvMonDev[iDevNum].flag;
    }
    pdispdev->DeviceID[0] = UNICODE_NULL;

    /* Fill in DeviceID */
    if (pdo != NULL)
    {
        Status = IoGetDeviceProperty(pdo,
                                     DevicePropertyHardwareID,
                                     0,
                                     NULL,
                                     &dwLength);

        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            pHardwareId = ExAllocatePoolWithTag(PagedPool,
                                                dwLength,
                                                USERTAG_DISPLAYINFO);
            if (!pHardwareId)
            {
                EngSetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            Status = IoGetDeviceProperty(pdo,
                                         DevicePropertyHardwareID,
                                         dwLength,
                                         pHardwareId,
                                         &dwLength);

            if (!NT_SUCCESS(Status))
            {
                ERR("IoGetDeviceProperty() failed with status 0x%08lx\n", Status);
            }
            else
            {
                /* For video adapters it should be the first Hardware ID
                 * which usually is the longest one and unique enough */
                RtlStringCbCopyW(pdispdev->DeviceID, sizeof(pdispdev->DeviceID), pHardwareId);

                if (pustrDevice)
                {
                    /* For monitors it should be the first Hardware ID,
                     * which we already have obtained above,
                     * concatenated with the unique driver registry key */

                    RtlStringCbCatW(pdispdev->DeviceID, sizeof(pdispdev->DeviceID), L"\\");

                    dwLength = wcslen(pdispdev->DeviceID) + 1;
                    Status = IoGetDeviceProperty(pdo,
                                                 DevicePropertyDriverKeyName,
                                                 (ARRAYSIZE(pdispdev->DeviceID) - dwLength) * sizeof(WCHAR),
                                                 pdispdev->DeviceID + dwLength - 1,
                                                 &dwLength);
                }

                TRACE("Hardware ID: %ls\n", pdispdev->DeviceID);
            }

            ExFreePoolWithTag(pHardwareId, USERTAG_DISPLAYINFO);
        }
        else
        {
            ERR("IoGetDeviceProperty() failed with status 0x%08lx\n", Status);
        }
    }

    return STATUS_SUCCESS;
}

//NTSTATUS
BOOL
NTAPI
NtUserEnumDisplayDevices(
    PUNICODE_STRING pustrDevice,
    DWORD iDevNum,
    PDISPLAY_DEVICEW pDisplayDevice,
    DWORD dwFlags)
{
    UNICODE_STRING ustrDevice;
    WCHAR awcDevice[CCHDEVICENAME];
    DISPLAY_DEVICEW dispdev;
    NTSTATUS Status;

    TRACE("Enter NtUserEnumDisplayDevices(%wZ, %lu)\n",
          pustrDevice, iDevNum);

    dispdev.cb = sizeof(dispdev);

    if (pustrDevice)
    {
        /* Initialize destination string */
        RtlInitEmptyUnicodeString(&ustrDevice, awcDevice, sizeof(awcDevice));

        _SEH2_TRY
        {
            /* Probe the UNICODE_STRING and the buffer */
            ProbeForRead(pustrDevice, sizeof(UNICODE_STRING), 1);
            ProbeForRead(pustrDevice->Buffer, pustrDevice->Length, 1);

            /* Copy the string */
            RtlCopyUnicodeString(&ustrDevice, pustrDevice);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
//            _SEH2_YIELD(return _SEH2_GetExceptionCode());
            _SEH2_YIELD(return NT_SUCCESS(_SEH2_GetExceptionCode()));
        }
        _SEH2_END

        if (ustrDevice.Length > 0)
            pustrDevice = &ustrDevice;
        else
            pustrDevice = NULL;
   }

    /* Acquire global USER lock */
    UserEnterShared();

    /* Call the internal function */
    Status = UserEnumDisplayDevices(pustrDevice, iDevNum, &dispdev, dwFlags);

    /* Release lock */
    UserLeave();

    /* On success copy data to caller */
    if (NT_SUCCESS(Status))
    {
        /* Enter SEH */
        _SEH2_TRY
        {
            /* First probe the cb field */
            ProbeForWrite(&pDisplayDevice->cb, sizeof(DWORD), 1);

            /* Check the buffer size */
            if (pDisplayDevice->cb)
            {
                /* Probe the output buffer */
                pDisplayDevice->cb = min(pDisplayDevice->cb, sizeof(dispdev));
                ProbeForWrite(pDisplayDevice, pDisplayDevice->cb, 1);

                /* Copy as much as the given buffer allows */
                RtlCopyMemory(pDisplayDevice, &dispdev, pDisplayDevice->cb);
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END
    }

    TRACE("Leave NtUserEnumDisplayDevices, Status = 0x%lx\n", Status);
    /* Return the result */
//    return Status;
    return NT_SUCCESS(Status); // FIXME
}

NTSTATUS
NTAPI
UserEnumCurrentDisplaySettings(
    PUNICODE_STRING pustrDevice,
    PDEVMODEW *ppdm)
{
    PPDEVOBJ ppdev;

    /* Get the PDEV for the device */
    ppdev = EngpGetPDEV(pustrDevice);
    if (!ppdev)
    {
        /*
         * A display device that is off the desktop runs no mode, and is reported with an empty
         * one rather than a failure, as Wine does: wined3d gives up restoring the desktop mode
         * after a game as soon as the current mode of any listed device cannot be read, and the
         * VGA device of the board's iGPU is always listed.
         */
        if ((pustrDevice != NULL) && (gdmDetached.dmSize != 0) &&
            (EngpFindGraphicsDevice(pustrDevice, 0) != NULL))
        {
            *ppdm = &gdmDetached;
            return STATUS_SUCCESS;
        }

        /* No device found */
        ERR("No PDEV found!\n");
        return STATUS_INVALID_PARAMETER_1;
    }

    *ppdm = ppdev->pdmwDev;
    PDEVOBJ_vRelease(ppdev);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
UserEnumDisplaySettings(
   PUNICODE_STRING pustrDevice,
   DWORD iModeNum,
   LPDEVMODEW *ppdm,
   DWORD dwFlags)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PDEVMODEENTRY pdmentry;
    ULONG i, iFoundMode;
    PPDEVOBJ ppdev;

    TRACE("Enter UserEnumDisplaySettings('%wZ', %lu)\n",
          pustrDevice, iModeNum);

    /* Ask GDI for the GRAPHICS_DEVICE */
    pGraphicsDevice = EngpFindGraphicsDevice(pustrDevice, 0);
    ppdev = EngpGetPDEV(pustrDevice);

    if (!pGraphicsDevice || !ppdev)
    {
        /* No device found */
        ERR("No device found!\n");
        return STATUS_INVALID_PARAMETER_1;
    }

    /* let's politely ask the driver for an updated mode list,
       just in case there's something new in there (vbox) */

    PDEVOBJ_vRefreshModeList(ppdev);
    PDEVOBJ_vRelease(ppdev);

    iFoundMode = 0;
    for (i = 0; i < pGraphicsDevice->cDevModes; i++)
    {
        pdmentry = &pGraphicsDevice->pDevModeList[i];

        /* FIXME: Consider EDS_RAWMODE */
#if 0
        if ((!(dwFlags & EDS_RAWMODE) && (pdmentry->dwFlags & 1)) ||!
            (dwFlags & EDS_RAWMODE))
#endif
        {
            /* Is this the one we want? */
            if (iFoundMode == iModeNum)
            {
                *ppdm = pdmentry->pdm;
                return STATUS_SUCCESS;
            }

            /* Increment number of found modes */
            iFoundMode++;
        }
    }

    /* Nothing was found */
    return STATUS_INVALID_PARAMETER_2;
}

NTSTATUS
NTAPI
UserOpenDisplaySettingsKey(
    OUT PHKEY phkey,
    IN PUNICODE_STRING pustrDevice,
    IN BOOL bGlobal)
{
    HKEY hkey;
    DISPLAY_DEVICEW dispdev;
    NTSTATUS Status;

    /* Get device info */
    Status = UserEnumDisplayDevices(pustrDevice, 0, &dispdev, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    if (bGlobal)
    {
        // FIXME: Need to fix the registry key somehow
    }

    /* Open the registry key */
    Status = RegOpenKey(dispdev.DeviceKey, &hkey);
    if (!NT_SUCCESS(Status))
        return Status;

    *phkey = hkey;

    return Status;
}

NTSTATUS
NTAPI
UserEnumRegistryDisplaySettings(
    IN PUNICODE_STRING pustrDevice,
    OUT LPDEVMODEW pdm)
{
    HKEY hkey;
    NTSTATUS Status;
    PGRAPHICS_DEVICE pGraphicsDevice;

    /*
     * A WDDM source keeps its settings in dxgkrnl's display database, not in DefaultSettings
     * values, which its device key never has. Reading those gave an empty mode, so a runtime
     * comparing it against the current one (wined3d, after a game) saw nothing to restore.
     */
    pGraphicsDevice = EngpFindGraphicsDevice(pustrDevice, 0);
    if ((pGraphicsDevice != NULL) && (pGraphicsDevice->DxgAdapter != NULL) &&
        EngpGetSavedDisplayMode(pGraphicsDevice, pdm))
    {
        return STATUS_SUCCESS;
    }

    Status = UserOpenDisplaySettingsKey(&hkey, pustrDevice, 0);
    if(NT_SUCCESS(Status))
    {
        RegReadDisplaySettings(hkey, pdm);
        ZwClose(hkey);
        return STATUS_SUCCESS;
    }
    return Status;
}

NTSTATUS
APIENTRY
NtUserEnumDisplaySettings(
    IN PUNICODE_STRING pustrDevice,
    IN DWORD iModeNum,
    OUT LPDEVMODEW lpDevMode,
    IN DWORD dwFlags)
{
    UNICODE_STRING ustrDeviceUser;
    UNICODE_STRING ustrDevice;
    WCHAR awcDevice[CCHDEVICENAME];
    NTSTATUS Status;
    ULONG cbSize, cbExtra;
    DEVMODEW dmReg, *pdm;

    TRACE("Enter NtUserEnumDisplaySettings(%wZ, %lu, %p, 0x%lx)\n",
          pustrDevice, iModeNum, lpDevMode, dwFlags);

    _SEH2_TRY
    {
        ProbeForRead(lpDevMode, sizeof(DEVMODEW), sizeof(UCHAR));

        cbSize = lpDevMode->dmSize;
        cbExtra = lpDevMode->dmDriverExtra;

        ProbeForWrite(lpDevMode, cbSize + cbExtra, sizeof(UCHAR));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (cbSize != sizeof(DEVMODEW))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (pustrDevice)
    {
        /* Initialize destination string */
        RtlInitEmptyUnicodeString(&ustrDevice, awcDevice, sizeof(awcDevice));

        _SEH2_TRY
        {
            /* Probe the UNICODE_STRING and the buffer */
            ustrDeviceUser = ProbeForReadUnicodeString(pustrDevice);

            if (!ustrDeviceUser.Length || !ustrDeviceUser.Buffer)
                ExRaiseStatus(STATUS_NO_MEMORY);

            ProbeForRead(ustrDeviceUser.Buffer,
                         ustrDeviceUser.Length,
                         sizeof(UCHAR));

            /* Copy the string */
            RtlCopyUnicodeString(&ustrDevice, &ustrDeviceUser);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER_1);
        }
        _SEH2_END;

        pustrDevice = &ustrDevice;
    }

    /* Acquire global USER lock */
    UserEnterShared();

    if (iModeNum == ENUM_REGISTRY_SETTINGS)
    {
        /* Get the registry settings */
        Status = UserEnumRegistryDisplaySettings(pustrDevice, &dmReg);
        pdm = &dmReg;
        pdm->dmSize = sizeof(DEVMODEW);
    }
    else if (iModeNum == ENUM_CURRENT_SETTINGS)
    {
        /* Get the current settings */
        Status = UserEnumCurrentDisplaySettings(pustrDevice, &pdm);
    }
    else
    {
        /* Get specified settings */
        Status = UserEnumDisplaySettings(pustrDevice, iModeNum, &pdm, dwFlags);
    }

    /* Release lock */
    UserLeave();

    /* Did we succeed? */
    if (NT_SUCCESS(Status))
    {
        /* Copy some information back */
        _SEH2_TRY
        {
            /* Output what we got */
            RtlCopyMemory(lpDevMode, pdm, min(cbSize, pdm->dmSize));

            /* Output private/extra driver data */
            if (cbExtra > 0 && pdm->dmDriverExtra > 0)
            {
                RtlCopyMemory((PCHAR)lpDevMode + cbSize,
                              (PCHAR)pdm + pdm->dmSize,
                              min(cbExtra, pdm->dmDriverExtra));
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    return Status;
}

VOID
UserUpdateFullscreen(
    DWORD flags)
{
    if (flags & CDS_FULLSCREEN)
        gpFullscreen = gptiCurrent->ppi;
    else
        gpFullscreen = NULL;
}

/* Brings the metrics and SERVERINFO in line with the primary display's current mode */
static
VOID
UserpUpdateDisplayMetrics(
    _In_ PPDEVOBJ ppdev,
    _In_ DWORD flags)
{
    TEXTMETRICW tmw;

    UserUpdateFullscreen(flags);

    /* Update the system metrics */
    InitMetrics();

    /* Set new size of the monitor */
    UserUpdateMonitorSize((HDEV)ppdev);

    /* Update the SERVERINFO */
    gpsi->dmLogPixels = ppdev->gdiinfo.ulLogPixelsY;
    gpsi->Planes      = ppdev->gdiinfo.cPlanes;
    gpsi->BitsPixel   = ppdev->gdiinfo.cBitsPixel;
    gpsi->BitCount    = gpsi->Planes * gpsi->BitsPixel;
    gpsi->aiSysMet[SM_CXSCREEN] = ppdev->gdiinfo.ulHorzRes;
    gpsi->aiSysMet[SM_CYSCREEN] = ppdev->gdiinfo.ulVertRes;
    if (ppdev->gdiinfo.flRaster & RC_PALETTE)
    {
        gpsi->PUSIFlags |= PUSIF_PALETTEDISPLAY;
    }
    else
    {
        gpsi->PUSIFlags &= ~PUSIF_PALETTEDISPLAY;
    }
    // Font is realized and this dc was previously set to internal DC_ATTR.
    gpsi->cxSysFontChar = IntGetCharDimensions(hSystemBM, &tmw, (DWORD*)&gpsi->cySysFontChar);
    gpsi->tmSysFont     = tmw;
}

/* Tells every window the display changed and redraws the desktop */
static
VOID
UserpBroadcastDisplayChange(
    _In_ PPDEVOBJ ppdev,
    _In_ DWORD flags,
    _In_ WORD OrigBC)
{
    ULONG_PTR ulResult;

    /* Remove all cursor clipping */
    UserClipCursor(NULL);

    //pdesk = IntGetActiveDesktop();
    //IntHideDesktop(pdesk);

    /* Send WM_DISPLAYCHANGE to all toplevel windows */
    co_IntSendMessageTimeout( HWND_BROADCAST,
                              WM_DISPLAYCHANGE,
                              gpsi->BitCount,
                              MAKELONG(gpsi->aiSysMet[SM_CXSCREEN], gpsi->aiSysMet[SM_CYSCREEN]),
                              SMTO_NORMAL,
                              100,
                              &ulResult );

    ERR("BitCount New %d Orig %d ChkNew %d\n",gpsi->BitCount,OrigBC,ppdev->gdiinfo.cBitsPixel);

    /* Not full screen and different bit count, send messages */
    if (!(flags & CDS_FULLSCREEN) &&
        gpsi->BitCount != OrigBC)
    {
        ERR("Detect settings changed.\n");
        UserSendNotifyMessage(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0);
        UserSendNotifyMessage(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0);
    }

    //co_IntShowDesktop(pdesk, ppdev->gdiinfo.ulHorzRes, ppdev->gdiinfo.ulVertRes);

    UserRedrawDesktop();
}

static
LONG
UserpChangeDisplaySettingsWddm(
    _In_ PPDEVOBJ ppdev,
    _In_opt_ PDEVMODEW pdm,
    _In_ DWORD flags);

LONG
APIENTRY
UserChangeDisplaySettings(
   PUNICODE_STRING pustrDevice,
   LPDEVMODEW pdm,
   DWORD flags,
   LPVOID lParam)
{
    DEVMODEW dm;
    LONG lResult = DISP_CHANGE_SUCCESSFUL;
    HKEY hkey;
    NTSTATUS Status;
    PPDEVOBJ ppdev;
    WORD OrigBC;
    //PDESKTOP pdesk;
    PDEVMODEW newDevMode = NULL;

    if ((pdm != NULL) && (pdm->dmSize < FIELD_OFFSET(DEVMODEW, dmFields)))
        return DISP_CHANGE_BADMODE; /* This is what WinXP SP3 returns */

    /*
     * A WDDM source changes mode through the display configuration, never by swapping the CDD's
     * PDEV alone: that leaves dxgkrnl presenting the old source mode out of a shadow of the new
     * size.
     */
    ppdev = EngpGetPDEV(pustrDevice);
    if ((ppdev != NULL) &&
        (ppdev->pGraphicsDevice != NULL) &&
        (ppdev->pGraphicsDevice->DxgAdapter != NULL) &&
        !gbBaseVideo)
    {
        lResult = UserpChangeDisplaySettingsWddm(ppdev, pdm, flags);
        PDEVOBJ_vRelease(ppdev);
        return lResult;
    }
    if (ppdev != NULL)
        PDEVOBJ_vRelease(ppdev);

    /* If no DEVMODE is given, use registry settings */
    if (!pdm)
    {
        /* Get the registry settings */
        Status = UserEnumRegistryDisplaySettings(pustrDevice, &dm);
        if (!NT_SUCCESS(Status))
        {
            ERR("Could not load registry settings\n");
            return DISP_CHANGE_BADPARAM;
        }
    }
    else if (pdm->dmSize < FIELD_OFFSET(DEVMODEW, dmFields))
    {
        return DISP_CHANGE_BADMODE; /* This is what WinXP SP3 returns */
    }
    else
    {
        dm = *pdm;
    }

    /* Save original bit count */
    OrigBC = gpsi->BitCount;

    /* Check params */
    if ((dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT))
    {
        ERR("Devmode doesn't specify the resolution.\n");
        return DISP_CHANGE_BADMODE;
    }

    /* Get the PDEV */
    ppdev = EngpGetPDEV(pustrDevice);
    if (!ppdev)
    {
        ERR("Failed to get PDEV\n");
        return DISP_CHANGE_BADPARAM;
    }

    /* Fixup values */
    if (dm.dmBitsPerPel == 0 || !(dm.dmFields & DM_BITSPERPEL))
    {
        dm.dmBitsPerPel = ppdev->pdmwDev->dmBitsPerPel;
        dm.dmFields |= DM_BITSPERPEL;
    }

    if ((dm.dmFields & DM_DISPLAYFREQUENCY) && (dm.dmDisplayFrequency == 0))
        dm.dmDisplayFrequency = ppdev->pdmwDev->dmDisplayFrequency;

    /* Look for the requested DEVMODE; the saved settings may settle for the closest listed mode */
    if (!LDEVOBJ_bProbeAndCaptureDevmode(ppdev->pGraphicsDevice, &dm, &newDevMode, pdm == NULL))
    {
        ERR("Could not find a matching DEVMODE\n");
        lResult = DISP_CHANGE_BADMODE;
        goto leave;
    }
    else if (flags & CDS_TEST)
    {
        /* It's possible, go ahead! */
        lResult = DISP_CHANGE_SUCCESSFUL;
        goto leave;
    }

    /* Shall we update the registry? */
    if (flags & CDS_UPDATEREGISTRY)
    {
        /* Open the local or global settings key */
        Status = UserOpenDisplaySettingsKey(&hkey, pustrDevice, flags & CDS_GLOBAL);
        if (NT_SUCCESS(Status))
        {
            /* Store the settings */
            RegWriteDisplaySettings(hkey, newDevMode);

            /* Close the registry key */
            ZwClose(hkey);
        }
        else
        {
            ERR("Could not open registry key\n");
            lResult = DISP_CHANGE_NOTUPDATED;
        }
    }

    /* Check if DEVMODE matches the current mode */
    if (newDevMode->dmSize == ppdev->pdmwDev->dmSize &&
        RtlCompareMemory(newDevMode, ppdev->pdmwDev, newDevMode->dmSize) == newDevMode->dmSize &&
        !(flags & CDS_RESET))
    {
        ERR("DEVMODE matches, nothing to do\n");
        goto leave;
    }

    /* Shall we apply the settings? */
    if (!(flags & CDS_NORESET))
    {
        ULONG_PTR ulResult;
        PVOID pvOldCursor;

        /* Remove mouse pointer */
        pvOldCursor = UserSetCursor(NULL, TRUE);

        /* Do the mode switch */
        ulResult = PDEVOBJ_bSwitchMode(ppdev, newDevMode);

        /* Restore mouse pointer, no hooks called */
        pvOldCursor = UserSetCursor(pvOldCursor, TRUE);
        ASSERT(pvOldCursor == NULL);

        /* Check for success or failure */
        if (!ulResult)
        {
            /* Setting mode failed */
            ERR("Failed to set mode\n");

            /* Set the correct return value */
            if ((flags & CDS_UPDATEREGISTRY) && (lResult != DISP_CHANGE_NOTUPDATED))
                lResult = DISP_CHANGE_RESTART;
            else
                lResult = DISP_CHANGE_FAILED;
        }
        else
        {
            /* Setting mode succeeded */
            lResult = DISP_CHANGE_SUCCESSFUL;
            ExFreePoolWithTag(ppdev->pdmwDev, GDITAG_DEVMODE);
            ppdev->pdmwDev = newDevMode;

            UserpUpdateDisplayMetrics(ppdev, flags);
        }

        /*
         * Refresh the display on success and even on failure,
         * since the display may have been messed up.
         */
        UserpBroadcastDisplayChange(ppdev, flags, OrigBC);
    }

leave:
    if (newDevMode && newDevMode != ppdev->pdmwDev)
        ExFreePoolWithTag(newDevMode, GDITAG_DEVMODE);

    /* Release the PDEV */
    PDEVOBJ_vRelease(ppdev);

    return lResult;
}

VOID
UserDisplayNotifyShutdown(
    PPROCESSINFO ppiCurrent)
{
    if (ppiCurrent == gpFullscreen)
    {
        UserChangeDisplaySettings(NULL, NULL, 0, NULL);
        if (gpFullscreen)
            ERR("Failed to restore display mode!\n");
    }
}

LONG
APIENTRY
NtUserChangeDisplaySettings(
    PUNICODE_STRING pustrDevice,
    LPDEVMODEW lpDevMode,
    DWORD dwflags,
    LPVOID lParam)
{
    static UNICODE_STRING ustrPrimary = RTL_CONSTANT_STRING(L"(primary)");
    WCHAR awcDevice[CCHDEVICENAME];
    UNICODE_STRING ustrDevice;
    DEVMODEW dmLocal;
    LONG lRet;

    /* Check arguments */
    if ((dwflags != CDS_VIDEOPARAMETERS) && (lParam != NULL))
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return DISP_CHANGE_BADPARAM;
    }

    /* Check flags */
    if ((dwflags & (CDS_GLOBAL|CDS_NORESET)) && !(dwflags & CDS_UPDATEREGISTRY))
    {
        return DISP_CHANGE_BADFLAGS;
    }

    if ((dwflags & CDS_RESET) && (dwflags & CDS_NORESET))
    {
        return DISP_CHANGE_BADFLAGS;
    }

    /* Copy the device name */
    if (pustrDevice)
    {
        /* Initialize destination string */
        RtlInitEmptyUnicodeString(&ustrDevice, awcDevice, sizeof(awcDevice));

        _SEH2_TRY
        {
            /* Probe the UNICODE_STRING and the buffer */
            ProbeForRead(pustrDevice, sizeof(UNICODE_STRING), 1);
            ProbeForRead(pustrDevice->Buffer, pustrDevice->Length, 1);

            /* Copy the string */
            RtlCopyUnicodeString(&ustrDevice, pustrDevice);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Set and return error */
            SetLastNtError(_SEH2_GetExceptionCode());
            _SEH2_YIELD(return DISP_CHANGE_BADPARAM);
        }
        _SEH2_END

        pustrDevice = &ustrDevice;
   }

    /* Copy devmode */
    if (lpDevMode)
    {
        _SEH2_TRY
        {
            /* Probe the size field of the structure */
            ProbeForRead(lpDevMode, sizeof(dmLocal.dmSize), 1);

            /* Calculate usable size */
            dmLocal.dmSize = min(sizeof(dmLocal), lpDevMode->dmSize);

            /* Probe and copy the full DEVMODE */
            ProbeForRead(lpDevMode, dmLocal.dmSize, 1);
            RtlCopyMemory(&dmLocal, lpDevMode, dmLocal.dmSize);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Set and return error */
            SetLastNtError(_SEH2_GetExceptionCode());
            _SEH2_YIELD(return DISP_CHANGE_BADPARAM);
        }
        _SEH2_END

        /* Check for extra parameters */
        if (dmLocal.dmDriverExtra > 0)
        {
            /* FIXME: TODO */
            ERR("lpDevMode->dmDriverExtra is IGNORED!\n");
            dmLocal.dmDriverExtra = 0;
        }

        /* Use the local structure */
        lpDevMode = &dmLocal;
    }

    // FIXME: Copy videoparameters

    /* One line per call, so a mode change that never returns shows who asked for what */
    if (lpDevMode)
    {
        DPRINT1("win32k: %s asks '%wZ' for %lux%lux%lu %lu Hz (fields 0x%lx), flags 0x%lx\n",
                PsGetProcessImageFileName(PsGetCurrentProcess()), pustrDevice ? pustrDevice : &ustrPrimary,
                lpDevMode->dmPelsWidth, lpDevMode->dmPelsHeight, lpDevMode->dmBitsPerPel,
                lpDevMode->dmDisplayFrequency, lpDevMode->dmFields, dwflags);
    }
    else
    {
        DPRINT1("win32k: %s asks '%wZ' for its saved mode, flags 0x%lx\n",
                PsGetProcessImageFileName(PsGetCurrentProcess()), pustrDevice ? pustrDevice : &ustrPrimary,
                dwflags);
    }

    /* Acquire global USER lock */
    UserEnterExclusive();

    /* Call internal function */
    lRet = UserChangeDisplaySettings(pustrDevice, lpDevMode, dwflags, NULL);

    /* Release lock */
    UserLeave();

    DPRINT1("win32k: ChangeDisplaySettings of %s returns %ld\n",
            PsGetProcessImageFileName(PsGetCurrentProcess()), lRet);
    return lRet;
}

/* Display callouts from watchdog ********************************************/

/* A callout handed to a CSRSS system thread, and how the caller learns it ran */
typedef struct _USER_VIDEO_CALLOUT
{
    PVIDEO_WIN32K_CALLBACKS_PARAMS Params;
    KEVENT Done;
} USER_VIDEO_CALLOUT, *PUSER_VIDEO_CALLOUT;

NTSTATUS NTAPI DlSetDisplayConfig(_In_ PMDEVOBJ pmdevOld, _Out_ PMDEVOBJ *ppmdevNew);

LONG NTAPI
DlChangeDisplaySettings(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_opt_ PDEVMODEW pdmRequest,
    _In_opt_ const DEVMODEW *pdmCurrent,
    _In_ BOOLEAN bTryClosest,
    _In_ BOOLEAN bSetMode,
    _In_ BOOLEAN bUpdateRegistry,
    _In_ BOOLEAN bFullScreen,
    _In_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew,
    _Out_ PBOOLEAN pbApplied);

_Requires_exclusive_lock_held_(UserLock)
static
NTSTATUS
UserpSetDisplayConfig(VOID);

VOID
UserSignalVideoPortCalloutReady(VOID)
{
    KeSetEvent(&gVideoPortCalloutReady, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * Holds a callout that rebuilds the desktop until there is an input desktop to rebuild.
 */
_Requires_exclusive_lock_held_(UserLock)
static
VOID
UserpWaitForVideoPortCalloutReady(
    _In_ VIDEO_WIN32K_CALLBACKS_PARAMS_TYPE CalloutType)
{
    if ((CalloutType < VideoPnpNotifyCallout) || (CalloutType > VideoDxgkFindAdapterTdrCallout))
        return;

    if (gpdeskInputDesktop != NULL)
        return;

    UserLeave();
    KeWaitForSingleObject(&gVideoPortCalloutReady, WrUserRequest, KernelMode, FALSE, NULL);
    UserEnterExclusive();
}

/**
 * @brief
 * Moves the desktop onto a new MDEV. The live PDEV stays the one every DC holds, it takes over
 * the new display's driver instance.
 *
 * @return
 * STATUS_NOT_SUPPORTED when either side spans more than one display.
 */
_Requires_exclusive_lock_held_(UserLock)
static
NTSTATUS
UserpInstallMdev(
    _In_ PMDEVOBJ pmdevOld,
    _In_ PMDEVOBJ pmdevNew)
{
    PPDEVOBJ ppdevLive = pmdevOld->ppdevGlobal;
    PPDEVOBJ ppdevNext;

    if ((pmdevOld->cDev != 1) || (pmdevNew->cDev != 1))
    {
        ERR("Moving the desktop from %lu to %lu displays is not supported\n",
            pmdevOld->cDev, pmdevNew->cDev);
        MDEVOBJ_vDestroy(pmdevNew);
        MDEVOBJ_vEnable(pmdevOld);
        return STATUS_NOT_SUPPORTED;
    }

    ppdevNext = pmdevNew->dev[0].ppdev;
    if (ppdevNext == ppdevLive)
    {
        /* The same display kept its mode, so the old desktop simply comes back */
        MDEVOBJ_vDestroy(pmdevNew);
        MDEVOBJ_vEnable(pmdevOld);
        return STATUS_SUCCESS;
    }

    TRACE("Desktop moves from %S to %S\n",
          ppdevLive->pGraphicsDevice->szNtDeviceName,
          ppdevNext->pGraphicsDevice->szNtDeviceName);

    PDEVOBJ_vSwitchGraphicsDevice(ppdevLive, ppdevNext);

    /* ppdevNext now holds the driver instance the desktop left behind */
    MDEVOBJ_vDestroy(pmdevNew);
    if (ppdevNext->cPdevRefs == 1)
        PDEVOBJ_vRelease(ppdevNext);

    PDEVOBJ_vGetDeviceCaps(ppdevLive, &GdiHandleTable->DevCaps);
    return STATUS_SUCCESS;
}

/* user32 asks for the closest listed mode with this ChangeDisplaySettings flag */
#define USERP_CDS_TRY_CLOSEST   0x00000080

/**
 * @brief
 * Does the source already run the mode asked for? Only the fields the caller set count, the way
 * wined3d compares modes. A mode change to the running mode would still take the display down and
 * up again, and full screen games and runtimes often ask for the mode the desktop already has.
 */
static
BOOLEAN
UserpIsCurrentMode(
    _In_ const DEVMODEW *pdmCurrent,
    _In_ const DEVMODEW *pdm)
{
    if ((pdm->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT))
        return FALSE;

    if ((pdm->dmPelsWidth != pdmCurrent->dmPelsWidth) || (pdm->dmPelsHeight != pdmCurrent->dmPelsHeight))
        return FALSE;

    if ((pdm->dmFields & DM_BITSPERPEL) && (pdm->dmBitsPerPel != 0) &&
        (pdm->dmBitsPerPel != pdmCurrent->dmBitsPerPel))
    {
        return FALSE;
    }

    if ((pdm->dmFields & DM_DISPLAYFREQUENCY) && (pdm->dmDisplayFrequency > 1) &&
        (pdm->dmDisplayFrequency != pdmCurrent->dmDisplayFrequency))
    {
        return FALSE;
    }

    if ((pdm->dmFields & DM_DISPLAYORIENTATION) &&
        (pdm->dmDisplayOrientation != ((pdmCurrent->dmFields & DM_DISPLAYORIENTATION) ?
                                       pdmCurrent->dmDisplayOrientation : DMDO_DEFAULT)))
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Changes the mode of a WDDM source, the way Windows does for a legacy ChangeDisplaySettings
 * caller: dxgkrnl puts the mode into the display configuration and the desktop is rebuilt from it.
 *
 * @param[in] ppdev
 * The PDEV of the source.
 *
 * @param[in] pdm
 * The mode asked for, NULL to go back to the saved configuration.
 *
 * @param[in] flags
 * The CDS_* flags.
 *
 * @return
 * A DISP_CHANGE_* value.
 */
_Requires_exclusive_lock_held_(UserLock)
static
LONG
UserpChangeDisplaySettingsWddm(
    _In_ PPDEVOBJ ppdev,
    _In_opt_ PDEVMODEW pdm,
    _In_ DWORD flags)
{
    PMDEVOBJ pmdevNew = NULL;
    PVOID pvOldCursor = NULL;
    DEVMODEW dmSaved;
    BOOLEAN bSetMode;
    BOOLEAN bApplied = FALSE;
    WORD OrigBC;
    LONG lResult;
    NTSTATUS Status;

    /* CDS_TEST only checks the mode, CDS_NORESET stores it for a later change */
    bSetMode = !(flags & (CDS_TEST | CDS_NORESET));
    OrigBC = gpsi->BitCount;

    /*
     * Asking for the mode the source already runs changes nothing, nor does going back to the
     * saved mode while it runs, so dxgkrnl is not even asked: going through would at least take
     * the CDD's display down and up again while the desktop is rebuilt. Saving still goes through.
     */
    if (bSetMode && !(flags & (CDS_UPDATEREGISTRY | CDS_RESET)) && (ppdev->pdmwDev != NULL))
    {
        if (pdm == NULL)
        {
            if (!gbCdsJournalPending &&
                EngpGetSavedDisplayMode(ppdev->pGraphicsDevice, &dmSaved) &&
                UserpIsCurrentMode(ppdev->pdmwDev, &dmSaved))
            {
                DPRINT1("win32k: '%S' already runs its saved mode\n", ppdev->pGraphicsDevice->szWinDeviceName);
                UserUpdateFullscreen(flags);
                return DISP_CHANGE_SUCCESSFUL;
            }
        }
        else if (UserpIsCurrentMode(ppdev->pdmwDev, pdm))
        {
            DPRINT1("win32k: '%S' already runs %lux%lu\n", ppdev->pGraphicsDevice->szWinDeviceName,
                    pdm->dmPelsWidth, pdm->dmPelsHeight);
            return DISP_CHANGE_SUCCESSFUL;
        }
    }

    if (bSetMode)
        pvOldCursor = UserSetCursor(NULL, TRUE);

    lResult = DlChangeDisplaySettings(ppdev->pGraphicsDevice,
                                      pdm,
                                      ppdev->pdmwDev,
                                      (flags & USERP_CDS_TRY_CLOSEST) != 0,
                                      bSetMode,
                                      (flags & CDS_UPDATEREGISTRY) != 0,
                                      (flags & CDS_FULLSCREEN) != 0,
                                      gpmdev,
                                      &pmdevNew,
                                      &bApplied);
    if (!bSetMode)
    {
        /* DlChangeDisplaySettings only journaled it */
        if ((lResult == DISP_CHANGE_SUCCESSFUL) && (pdm != NULL) && (flags & CDS_UPDATEREGISTRY))
            gbCdsJournalPending = TRUE;
        return lResult;
    }

    if ((lResult == DISP_CHANGE_SUCCESSFUL) && (pmdevNew != NULL))
    {
        if (!NT_SUCCESS(UserpInstallMdev(gpmdev, pmdevNew)))
            lResult = DISP_CHANGE_FAILED;
    }

    pvOldCursor = UserSetCursor(pvOldCursor, TRUE);
    ASSERT(pvOldCursor == NULL);

    if (lResult == DISP_CHANGE_SUCCESSFUL)
    {
        UserpUpdateDisplayMetrics(gpmdev->ppdevGlobal, flags);

        /* The saved configuration went in with the journal, and GDI keeps its modes now */
        if ((pdm == NULL) || (flags & CDS_UPDATEREGISTRY))
            gbCdsJournalPending = FALSE;
    }
    else
    {
        DPRINT1("win32k: WDDM mode change failed %ld%s\n", lResult,
                bApplied ? ", the display runs a mode the desktop does not, going back to the saved configuration" : "");

        /*
         * dxgkrnl programmed the new mode but the desktop could not follow, so the monitor shows
         * a source nothing draws to. Put the saved configuration back and build the desktop for
         * it, as an adapter arrival does; it was running a moment ago.
         */
        if (bApplied)
        {
            Status = UserpSetDisplayConfig();
            DPRINT1("win32k: back to the saved display configuration, 0x%lx\n", Status);
            return lResult;
        }
    }

    /* Refresh even on failure, the display may have been left in a bad state */
    UserpBroadcastDisplayChange(gpmdev->ppdevGlobal, flags, OrigBC);
    return lResult;
}

/**
 * @brief
 * Applies the display configuration again and moves the desktop onto what it turned on.
 */
_Requires_exclusive_lock_held_(UserLock)
static
NTSTATUS
UserpSetDisplayConfig(VOID)
{
    PMDEVOBJ pmdevNew;
    PVOID pvOldCursor;
    WORD OrigBC;
    NTSTATUS Status;

    /* InitVideo builds the first desktop, so there is one by the time a callout gets here */
    if (gpmdev == NULL)
        return UserpCreateDesktopMdev();

    OrigBC = gpsi->BitCount;
    pvOldCursor = UserSetCursor(NULL, TRUE);

    Status = DlSetDisplayConfig(gpmdev, &pmdevNew);
    if (NT_SUCCESS(Status))
        Status = UserpInstallMdev(gpmdev, pmdevNew);

    pvOldCursor = UserSetCursor(pvOldCursor, TRUE);
    ASSERT(pvOldCursor == NULL);

    if (NT_SUCCESS(Status))
        UserpUpdateDisplayMetrics(gpmdev->ppdevGlobal, 0);
    else
        ERR("Applying the display configuration failed 0x%lx\n", Status);

    UserpBroadcastDisplayChange(gpmdev->ppdevGlobal, 0, OrigBC);
    return Status;
}

/**
 * @brief
 * An adapter came or went. The device list picks up what dxgkrnl started, and the desktop is
 * rebuilt from the display configuration.
 */
_Requires_exclusive_lock_held_(UserLock)
static
NTSTATUS
UserpVideoPnpNotify(
    _In_ PVIDEO_WIN32K_CALLBACKS_PARAMS Params)
{
    NTSTATUS Status;

    if (gpdeskInputDesktop == NULL)
        return STATUS_UNSUCCESSFUL;

    if (Params->Param)
    {
        TRACE("Display adapter arrived\n");
        EngpUpdateGraphicsDeviceList();
        return UserpSetDisplayConfig();
    }

    TRACE("Display adapter %p left, surprise removal %u\n", Params->PhysDisp, Params->SurpriseRemoval);

    EngpMarkGraphicsDevicesRemoved(Params->PhysDisp);

    Status = UserpSetDisplayConfig();
    if (NT_SUCCESS(Status))
        EngpCleanupGraphicsDevices(Params->PhysDisp);

    if (Params->LockUserSession)
        UserPostMessage(hwndSAS, WM_LOGONNOTIFY, LN_LOCK_WORKSTATION, 0);

    return Status;
}

/**
 * @brief
 * Runs one display callout on a CSRSS thread, inside the user lock.
 *
 * @param[in] Param
 * The USER_VIDEO_CALLOUT the caller of VideoPortCallout waits on.
 */
VOID
UserVideoPortCalloutThread(
    _In_ PVOID Param)
{
    PUSER_VIDEO_CALLOUT pRequest = Param;
    PVIDEO_WIN32K_CALLBACKS_PARAMS Params = pRequest->Params;
    PSMGR_GDI_CALLOUT_PARAM pWrapped;
    PTHREADINFO pti;

    /* watchdog pointed Param at its own record for the call, the caller's value is inside */
    pWrapped = (PSMGR_GDI_CALLOUT_PARAM)Params->Param;
    Params->Param = pWrapped->Param;

    pti = PsGetCurrentThreadWin32Thread();
    if (pti != NULL)
    {
        pti->TIF_flags |= TIF_SYSTEMTHREAD;
        pti->pClientInfo->dwTIFlags = pti->TIF_flags;
    }

    UserEnterExclusive();

    UserpWaitForVideoPortCalloutReady(Params->CalloutType);

    switch (Params->CalloutType)
    {
        case VideoFindAdapterCallout:
        case VideoDxgkFindAdapterTdrCallout:
        {
            if (gpmdev == NULL)
            {
                Params->Status = STATUS_SUCCESS;
                break;
            }

            if (Params->Param)
            {
                /* The adapter is back, re-enable the display and redraw everything */
                MDEVOBJ_vEnable(gpmdev);
                UserRefreshDisplay(gpmdev->ppdevGlobal);
            }
            else
            {
                MDEVOBJ_bDisable(gpmdev);
            }

            Params->Status = STATUS_SUCCESS;
            break;
        }

        case VideoPnpNotifyCallout:
            Params->Status = UserpVideoPnpNotify(Params);
            break;

        case VideoDxgkDisplaySwitchCallout:
        {
            if (Params->Param == 0)
            {
                Params->Status = UserpSetDisplayConfig();
            }
            else
            {
                /* The paths and modes come from SetDisplayConfig, which win32k does not have */
                ERR("Display switch with a supplied configuration is not supported\n");
                Params->Status = STATUS_NOT_SUPPORTED;
            }
            break;
        }

        case VideoDxgkMonitorEventCallout:
            ERR("Monitor event callouts are not supported\n");
            Params->Status = STATUS_NOT_SUPPORTED;
            break;

        case VideoDxgkHardwareProtectionTeardown:
            /* Only a compositor has protected content to drop */
            break;

        default:
            ERR("Unknown display callout 0x%x\n", Params->CalloutType);
            Params->Status = STATUS_UNSUCCESSFUL;
            break;
    }

    UserLeave();

    KeSetEvent(&pRequest->Done, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * The GDI callout watchdog delivers adapter arrival, removal, display switch and TDR through.
 * The work runs on a CSRSS system thread inside the user lock, this waits for it.
 *
 * @param[in,out] Params
 * A VIDEO_WIN32K_CALLBACKS_PARAMS. Status receives the result.
 */
VOID
NTAPI
VideoPortCallout(
    _In_ PVOID Params)
{
    PVIDEO_WIN32K_CALLBACKS_PARAMS CallbackParams = Params;
    USER_VIDEO_CALLOUT Request;
    NTSTATUS Status;

    /* Before InitVideo there is no display to act on, and the user lock may be held by the
     * CSRSS thread whose session open made dxgkrnl start the adapter */
    if (!gbVideoInitialized)
    {
        CallbackParams->Status = STATUS_UNSUCCESSFUL;
        return;
    }

    /*
     * dxgkrnl raises some callouts synchronously on the thread that is inside it. If that thread
     * holds the user lock (a ChangeDisplaySettings caller is in dxgkrnl's apply with it), the
     * CSRSS thread below could never take the lock and both would wait for good, with the display
     * in the middle of a mode change. Turn the callout away instead; dxgkrnl logs and goes on.
     */
    if (UserIsEntered())
    {
        DPRINT1("win32k: display callout %d from a thread inside the user lock turned away\n",
                CallbackParams->CalloutType);
        CallbackParams->Status = STATUS_DEVICE_BUSY;
        return;
    }

    Request.Params = CallbackParams;
    KeInitializeEvent(&Request.Done, SynchronizationEvent, FALSE);

    UserEnterExclusive();
    Status = UserQueueSystemThread(ST_VIDEO_CALLOUT, &Request);
    UserLeave();

    if (!NT_SUCCESS(Status))
    {
        CallbackParams->Status = Status;
        return;
    }

    KeWaitForSingleObject(&Request.Done, WrUserRequest, KernelMode, FALSE, NULL);
}

/* DisplayConfigGetDeviceInfo ************************************************/

/*
 * The info types, packet layouts and packet sizes below are those of Windows 11 build 26100.
 *
 * win32k builds for Windows Server 2003, where wingdi.h has none of the display configuration
 * types, so the packets are spelled out here.
 */
typedef struct _DISPLAYCONFIG_PACKET_HEADER
{
    LONG Type;
    ULONG Size;
    LUID AdapterLuid;
    ULONG Id;
} DISPLAYCONFIG_PACKET_HEADER, *PDISPLAYCONFIG_PACKET_HEADER;

C_ASSERT(sizeof(DISPLAYCONFIG_PACKET_HEADER) == 0x14);

/* DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME */
#define DISPLAYCONFIG_GET_SOURCE_NAME       1

typedef struct _DISPLAYCONFIG_SOURCE_NAME_PACKET
{
    DISPLAYCONFIG_PACKET_HEADER Header;
    WCHAR GdiDeviceName[CCHDEVICENAME];
} DISPLAYCONFIG_SOURCE_NAME_PACKET, *PDISPLAYCONFIG_SOURCE_NAME_PACKET;

C_ASSERT(sizeof(DISPLAYCONFIG_SOURCE_NAME_PACKET) == 0x54);

/* DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE, its targetMode is 8-byte aligned at 0x20 */
#define DISPLAYCONFIG_GET_TARGET_PREFERRED_MODE 3
#define TARGET_PREFERRED_MODE_PADDING       0x1c

/* DISPLAYCONFIG_DEVICE_INFO_GET_SUPPORT_VIRTUAL_RESOLUTION */
#define DISPLAYCONFIG_GET_SUPPORT_VIRTUAL_RESOLUTION 7

/*
 * The undocumented info type the Direct3D 9 runtime learns its adapters from, d3d9.dll calls the
 * packet _DISPLAYCONFIG_GET_DISPLAY_INFO. dxgkrnl's DxgkGetAdapterDeviceDesc fills in the adapters,
 * the mode of the source and the flags from bit 5 up, win32k where the source sits on the desktop,
 * its GDI name and the flags below. On Windows, types -30, -21 and -11 return the same packet;
 * ReactOS only knows -2.
 */
#define DISPLAYCONFIG_GET_DISPLAY_INFO      (-2)

#define DISPLAY_INFO_REMOTE                 0x00000001  /* off the desktop of a remote session, never in ReactOS */
#define DISPLAY_INFO_AT_ORIGIN              0x00000002  /* the source starts at the desktop origin */
#define DISPLAY_INFO_ON_DESKTOP             0x00000004  /* an enabled display PDEV shows the source */
#define DISPLAY_INFO_WIN32K_FLAGS           0x00000007

/* The mode win32k reports for a source that is off the desktop */
#define DISPLAY_INFO_FORMAT_NONGDI          5           /* DISPLAYCONFIG_PIXELFORMAT_NONGDI */
#define DISPLAY_INFO_ROTATION_IDENTITY      1           /* D3DKMDT_VPPR_IDENTITY */

typedef struct _DISPLAYCONFIG_DISPLAY_INFO_PACKET
{
    DISPLAYCONFIG_PACKET_HEADER Header;     /* 0x000 */
    UCHAR AdapterInfo[0x658];               /* 0x014 two adapter descriptions: PCI IDs, description,
                                               device path, driver model. d3d9 takes its adapter
                                               identifier from the second, at 0x340 */
    RECTL rcDesktop;                        /* 0x66c */
    WCHAR GdiDeviceName[128];               /* 0x67c */
    ULONG TargetId;                         /* 0x77c the most important target of the source */
    ULONG DisplayId;                        /* 0x780 */
    ULONG Width;                            /* 0x784 the mode of the source */
    ULONG Height;                           /* 0x788 */
    ULONG PixelFormat;                      /* 0x78c DISPLAYCONFIG_PIXELFORMAT */
    ULONG RefreshNumerator;                 /* 0x790 */
    ULONG RefreshDenominator;               /* 0x794 */
    ULONG Rotation;                         /* 0x798 D3DKMDT_VIDPN_PRESENT_PATH_ROTATION */
    ULONG Reserved79C;                      /* 0x79c from the mode as well */
    RECTL rcSource;                         /* 0x7a0 d3d9 reads it, turned with Rotation */
    UCHAR ColorInfo[0x3c];                  /* 0x7b0 wire format, color space, colorimetry */
    ULONG TargetUsage;                      /* 0x7ec nonzero when dxgkrnl set the target aside */
    UCHAR Reserved7F0[0xc];                 /* 0x7f0 */
    LUID PairedAdapterLuid;                 /* 0x7fc render adapter of an indirect display */
    ULONG Flags;                            /* 0x804 */
} DISPLAYCONFIG_DISPLAY_INFO_PACKET, *PDISPLAYCONFIG_DISPLAY_INFO_PACKET;

C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, rcDesktop) == 0x66c);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, GdiDeviceName) == 0x67c);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, TargetId) == 0x77c);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, Width) == 0x784);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, rcSource) == 0x7a0);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, TargetUsage) == 0x7ec);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, PairedAdapterLuid) == 0x7fc);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_DISPLAY_INFO_PACKET, Flags) == 0x804);
C_ASSERT(sizeof(DISPLAYCONFIG_DISPLAY_INFO_PACKET) == 0x808);

/* The info types dxgkrnl answers itself. A packet must have the exact size of its type */
static const struct
{
    LONG Type;
    ULONG Size;
} gDxgkDisplayConfigTypes[] =
{
    { -37, 0x2c }, { -36, 0x18 }, { -34, 0x308 }, { -33, 0x178 }, { -29, 0x18 }, { -26, 0x18 },
    { -24, 0x20 }, { -22, 0x1c }, { -14, 0x28 }, { -13, 0x24 }, { -12, 0x18 }, { -10, 0x1c },
    { -5, 0x1c },
    { 2, 0x1a4 },   /* GET_TARGET_NAME */
    { 3, 0x50 },    /* GET_TARGET_PREFERRED_MODE */
    { 4, 0x114 },   /* GET_ADAPTER_NAME */
    { 6, 0x18 },    /* GET_TARGET_BASE_TYPE */
    { 7, 0x18 },    /* GET_SUPPORT_VIRTUAL_RESOLUTION */
    { 9, 0x20 },    /* GET_ADVANCED_COLOR_INFO */
    { 11, 0x18 },   /* GET_SDR_WHITE_LEVEL */
    { 12, 0x18 },   /* GET_MONITOR_SPECIALIZATION */
    { 15, 0x24 },   /* GET_ADVANCED_COLOR_INFO_2 */
    { 100, 0x18 },
};

NTSTATUS NTAPI DlConvertDisplayConfigAdapterAndId(_Inout_ PLUID pAdapterLuid, _Inout_ PULONG pId);
NTSTATUS NTAPI DlDisplayConfigGetDeviceInfo(_Inout_ PVOID Packet);
NTSTATUS NTAPI DlGetAdapterDeviceDesc(_In_ LUID AdapterLuid, _In_ ULONG VidPnSourceId, _Inout_ PVOID Packet);

/**
 * @brief Fills win32k's part of a display info packet: the GDI name of the source, where it sits
 *        on the desktop and the flags that say so. dxgkrnl filled in its part already.
 *
 * @return STATUS_NOT_FOUND when GDI has no device for the source.
 */
_Requires_lock_held_(UserLock)
static
NTSTATUS
UserGetDisplayInfo(
    _Inout_ PDISPLAYCONFIG_DISPLAY_INFO_PACKET pInfo)
{
    PGRAPHICS_DEVICE pGraphicsDevice;
    PPDEVOBJ ppdev = NULL;
    SIZEL sizl;
    ULONG i;

    pGraphicsDevice = EngpFindGraphicsDeviceBySource(&pInfo->Header.AdapterLuid,
                                                     pInfo->Header.Id,
                                                     pInfo->GdiDeviceName,
                                                     sizeof(pInfo->GdiDeviceName));
    if (pGraphicsDevice == NULL)
        return STATUS_NOT_FOUND;

    /* The source is on the desktop when an enabled display PDEV of the desktop shows it */
    for (i = 0; (gpmdev != NULL) && (i < gpmdev->cDev); i++)
    {
        if ((gpmdev->dev[i].ppdev->pGraphicsDevice == pGraphicsDevice) &&
            ((gpmdev->dev[i].ppdev->flFlags & (PDEV_DISPLAY | PDEV_DISABLED)) == PDEV_DISPLAY))
        {
            ppdev = gpmdev->dev[i].ppdev;
            break;
        }
    }

    pInfo->Flags &= ~DISPLAY_INFO_WIN32K_FLAGS;

    if (ppdev != NULL)
    {
        PDEVOBJ_sizl(ppdev, &sizl);
        pInfo->rcDesktop.left = ppdev->ptlOrigion.x;
        pInfo->rcDesktop.top = ppdev->ptlOrigion.y;
        pInfo->rcDesktop.right = ppdev->ptlOrigion.x + sizl.cx;
        pInfo->rcDesktop.bottom = ppdev->ptlOrigion.y + sizl.cy;

        pInfo->Flags |= DISPLAY_INFO_ON_DESKTOP;
        if ((ppdev->ptlOrigion.x == 0) && (ppdev->ptlOrigion.y == 0))
            pInfo->Flags |= DISPLAY_INFO_AT_ORIGIN;

        return STATUS_SUCCESS;
    }

    /* Off the desktop it has no mode either, unless dxgkrnl set its target aside for another use */
    if (pInfo->TargetUsage == 0)
    {
        pInfo->Width = 0;
        pInfo->Height = 0;
        pInfo->PixelFormat = DISPLAY_INFO_FORMAT_NONGDI;
        pInfo->RefreshNumerator = 0;
        pInfo->RefreshDenominator = 1;
        pInfo->Rotation = DISPLAY_INFO_ROTATION_IDENTITY;
        pInfo->Reserved79C = 0;
        RtlZeroMemory(&pInfo->rcDesktop, sizeof(pInfo->rcDesktop));
        RtlZeroMemory(&pInfo->rcSource, sizeof(pInfo->rcSource));
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Answers one DisplayConfigGetDeviceInfo packet, already copied into the kernel and checked
 *        to have the size of its type.
 */
static
NTSTATUS
UserDisplayConfigGetDeviceInfo(
    _Inout_ PDISPLAYCONFIG_PACKET_HEADER pPacket)
{
    PDISPLAYCONFIG_SOURCE_NAME_PACKET pSourceName;
    PDISPLAYCONFIG_DISPLAY_INFO_PACKET pInfo;
    PGRAPHICS_DEVICE pGraphicsDevice;
    NTSTATUS Status, StatusUser;
    ULONG i;

    /* The id of a QueryDisplayConfig path can carry dxgkrnl's virtual topology hash */
    if (pPacket->Id != (ULONG)-1)
    {
        Status = DlConvertDisplayConfigAdapterAndId(&pPacket->AdapterLuid, &pPacket->Id);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    switch (pPacket->Type)
    {
        case DISPLAYCONFIG_GET_SOURCE_NAME:
            pSourceName = (PDISPLAYCONFIG_SOURCE_NAME_PACKET)pPacket;

            UserEnterShared();
            pGraphicsDevice = EngpFindGraphicsDeviceBySource(&pPacket->AdapterLuid,
                                                             pPacket->Id,
                                                             pSourceName->GdiDeviceName,
                                                             sizeof(pSourceName->GdiDeviceName));
            UserLeave();

            return (pGraphicsDevice != NULL) ? STATUS_SUCCESS : STATUS_NOT_FOUND;

        case DISPLAYCONFIG_GET_DISPLAY_INFO:
            pInfo = (PDISPLAYCONFIG_DISPLAY_INFO_PACKET)pPacket;

            /* The flags are cleared before dxgkrnl sets its own, win32k's go in afterwards */
            pInfo->Flags = 0;

            Status = DlGetAdapterDeviceDesc(pPacket->AdapterLuid, pPacket->Id, pPacket);
            if (Status == STATUS_NOT_SUPPORTED)
                return Status;

            if (!NT_SUCCESS(Status))
            {
                DPRINT1("win32k: DxgkGetAdapterDeviceDesc(%08lx:%08lx, %ld) failed 0x%lx\n",
                        pPacket->AdapterLuid.HighPart, pPacket->AdapterLuid.LowPart,
                        (LONG)pPacket->Id, Status);
            }

            /*
             * win32k fills in its part even when dxgkrnl failed. A source GDI does not know fails
             * the call, otherwise dxgkrnl's status stands.
             */
            if (pPacket->Id != (ULONG)-1)
            {
                UserEnterShared();
                StatusUser = UserGetDisplayInfo(pInfo);
                UserLeave();

                if (!NT_SUCCESS(StatusUser))
                    return StatusUser;
            }

            return NT_SUCCESS(Status) ? STATUS_SUCCESS : Status;

        default:
            for (i = 0; i < RTL_NUMBER_OF(gDxgkDisplayConfigTypes); i++)
            {
                if (gDxgkDisplayConfigTypes[i].Type == pPacket->Type)
                    return DlDisplayConfigGetDeviceInfo(pPacket);
            }
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief The packet size an info type takes, 0 for a type win32k does not answer.
 */
static
ULONG
UserDisplayConfigPacketSize(
    _In_ LONG Type)
{
    ULONG i;

    if (Type == DISPLAYCONFIG_GET_SOURCE_NAME)
        return sizeof(DISPLAYCONFIG_SOURCE_NAME_PACKET);

    if (Type == DISPLAYCONFIG_GET_DISPLAY_INFO)
        return sizeof(DISPLAYCONFIG_DISPLAY_INFO_PACKET);

    for (i = 0; i < RTL_NUMBER_OF(gDxgkDisplayConfigTypes); i++)
    {
        if (gDxgkDisplayConfigTypes[i].Type == Type)
            return gDxgkDisplayConfigTypes[i].Size;
    }

    return 0;
}

/**
 * @brief Narrows a failure down to the codes that may reach a DisplayConfigGetDeviceInfo caller,
 *        every other one becomes STATUS_UNSUCCESSFUL (ERROR_GEN_FAILURE once user32 has it).
 */
static
NTSTATUS
UserDisplayConfigMapStatus(
    _In_ LONG Type,
    _In_ NTSTATUS Status)
{
    if (NT_SUCCESS(Status) || (Status == STATUS_BUFFER_TOO_SMALL))
        return Status;

    if (Status == STATUS_BUFFER_OVERFLOW)
        return STATUS_BUFFER_TOO_SMALL;

    if ((Type == DISPLAYCONFIG_GET_SUPPORT_VIRTUAL_RESOLUTION) &&
        ((Status == STATUS_ILLEGAL_FUNCTION) || (Status == STATUS_DEVICE_DOES_NOT_EXIST)))
    {
        return Status;
    }

    switch (Status)
    {
        case STATUS_ACCESS_DENIED:
        case STATUS_INVALID_PARAMETER:
        case STATUS_NO_MEMORY:
        case STATUS_INVALID_PARAMETER_MIX:
        case STATUS_NOT_SUPPORTED:
            return Status;
    }

    if ((Status >= STATUS_INVALID_PARAMETER_1) && (Status <= STATUS_INVALID_PARAMETER_12))
        return Status;

    return STATUS_UNSUCCESSFUL;
}

/**
 * @brief The kernel side of DisplayConfigGetDeviceInfo.
 *
 * @param pPacket The caller's packet, a DISPLAYCONFIG_DEVICE_INFO_HEADER followed by what its
 *        type returns, 4-byte aligned. Its size has to be exactly the one of the type.
 *
 * @return An NTSTATUS, which user32 turns into a Win32 error. A packet that cannot be read or
 *         written back, is smaller than its header or cannot be captured for lack of memory fails
 *         with STATUS_UNSUCCESSFUL and leaves the reason as the thread's last error.
 */
NTSTATUS
NTAPI
NtUserDisplayConfigGetDeviceInfo(
    _Inout_ struct DISPLAYCONFIG_DEVICE_INFO_HEADER *pPacket)
{
    PDISPLAYCONFIG_PACKET_HEADER pUserPacket = (PDISPLAYCONFIG_PACKET_HEADER)pPacket;
    PDISPLAYCONFIG_PACKET_HEADER pCapture;
    DISPLAYCONFIG_PACKET_HEADER Header;
    NTSTATUS Status;
    ULONG Size;

    _SEH2_TRY
    {
        ProbeForRead(pUserPacket, sizeof(Header), sizeof(ULONG));
        Header = *pUserPacket;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
        _SEH2_YIELD(return STATUS_UNSUCCESSFUL);
    }
    _SEH2_END;

    if (Header.Size < sizeof(Header))
    {
        SetLastNtError(STATUS_INVALID_PARAMETER);
        return STATUS_UNSUCCESSFUL;
    }

    Size = UserDisplayConfigPacketSize(Header.Type);
    if ((Size == 0) || (Header.Size != Size))
        return STATUS_INVALID_PARAMETER;

    pCapture = ExAllocatePoolZero(PagedPool, Size, USERTAG_DISPLAYCONFIG);
    if (pCapture == NULL)
    {
        SetLastNtError(STATUS_NO_MEMORY);
        return STATUS_UNSUCCESSFUL;
    }

    _SEH2_TRY
    {
        ProbeForRead(pUserPacket, Size, sizeof(ULONG));
        RtlCopyMemory(pCapture, pUserPacket, Size);
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
        Status = STATUS_UNSUCCESSFUL;
    }
    _SEH2_END;

    if (NT_SUCCESS(Status))
    {
        /* Decide on what was checked, not on what the caller may since have changed */
        *pCapture = Header;

        Status = UserDisplayConfigGetDeviceInfo(pCapture);
        Status = UserDisplayConfigMapStatus(Header.Type, Status);

        /*
         * The padding in front of targetMode is not dxgkrnl's to fill: clear it, no kernel data
         * goes out.
         */
        if ((Header.Type == DISPLAYCONFIG_GET_TARGET_PREFERRED_MODE) && NT_SUCCESS(Status))
            *(PULONG)((PUCHAR)pCapture + TARGET_PREFERRED_MODE_PADDING) = 0;

        /* The packet goes back whatever the outcome */
        _SEH2_TRY
        {
            ProbeForWrite(pUserPacket, Size, sizeof(ULONG));
            RtlCopyMemory(pUserPacket, pCapture, Size);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastNtError(_SEH2_GetExceptionCode());
            Status = STATUS_UNSUCCESSFUL;
        }
        _SEH2_END;
    }

    ExFreePoolWithTag(pCapture, USERTAG_DISPLAYCONFIG);

    TRACE("NtUserDisplayConfigGetDeviceInfo(%ld) -> 0x%lx\n", Header.Type, Status);
    return Status;
}
