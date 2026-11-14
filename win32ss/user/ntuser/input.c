/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Win32k subsystem
 * PURPOSE:          General input functions
 * FILE:             win32ss/user/ntuser/input.c
 * PROGRAMERS:       Casper S. Hornstrup (chorns@users.sourceforge.net)
 *                   Rafal Harabien (rafalh@reactos.org)
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserInput);

/* LastRITEventTickCount update cycle has been adjusted from 60s to 1s since NT6 */
#if (NTDDI_VERSION < NTDDI_VISTA)
#define LAST_RIT_EVENT_UPDATE_INTERVAL 60000UL
#else
#define LAST_RIT_EVENT_UPDATE_INTERVAL 1000UL
#endif

static
VOID NTAPI
RITReadApc(
    _In_ PVOID ApcContext,
    _In_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ ULONG Reserved);

/* GLOBALS *******************************************************************/

PTHREADINFO ptiRawInput;
PKTIMER MasterTimer = NULL;
PATTACHINFO gpai = NULL;
INT paiCount = 0;
HANDLE ghKeyboardDevice = NULL;
PINPUT_DEVICE_INFO gpInputDeviceInfo = NULL;
PERESOURCE gpDeviceInfoListMutex = NULL;

static DWORD LastInputTick = 0;
static PKEVENT gDeviceListChangedEvent;

/* HID collections are rescanned on this period, win32k cannot take PnP callbacks in session space */
#define HID_SCAN_PERIOD_MS 2000
static PKTIMER gHidScanTimer;

static const GUID HidInterfaceGuid =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

/* HID collections already looked at that are not touch screens */
typedef struct _HID_SKIPPED_DEVICE
{
    struct _HID_SKIPPED_DEVICE *Next;
    UNICODE_STRING Name;
} HID_SKIPPED_DEVICE, *PHID_SKIPPED_DEVICE;

static PHID_SKIPPED_DEVICE gHidSkippedDevices;

/* FUNCTIONS *****************************************************************/

/*
 * IntLastInputTick
 *
 * Updates or gets last input tick count
 */
static DWORD FASTCALL
IntLastInputTick(BOOL bUpdate)
{
    if (bUpdate)
    {
        LastInputTick = EngGetTickCount32();
        if (gpsi)
        {
            gpsi->dwLastRITEventTickCount = LastInputTick;
            if (gpsi->dwLastRITEventTickCount - gpsi->dwLastSystemRITEventTickCountUpdate >
                LAST_RIT_EVENT_UPDATE_INTERVAL)
            {
                SharedUserData->LastSystemRITEventTickCount = LastInputTick;
                gpsi->dwLastSystemRITEventTickCountUpdate = LastInputTick;
            }
        }
    }
    return LastInputTick;
}

/*
 * DoTheScreenSaver
 *
 * Check if screensaver should be started and sends message to SAS window
 */
VOID FASTCALL
DoTheScreenSaver(VOID)
{
    DWORD Test, TO;

    if (gspv.iScrSaverTimeout > 0) // Zero means Off.
    {
        Test = EngGetTickCount32();
        Test = Test - LastInputTick;
        TO = 1000 * gspv.iScrSaverTimeout;
        if (Test > TO)
        {
            TRACE("Screensaver Message Start! Tick %lu Timeout %d \n", Test, gspv.iScrSaverTimeout);

            if (ppiScrnSaver) // We are or we are not the screensaver, prevent reentry...
            {
                if (!(ppiScrnSaver->W32PF_flags & W32PF_IDLESCREENSAVER))
                {
                    ppiScrnSaver->W32PF_flags |= W32PF_IDLESCREENSAVER;
                    ERR("Screensaver is Idle\n");
                }
            }
            else
            {
                PUSER_MESSAGE_QUEUE ForegroundQueue = IntGetFocusMessageQueue();
                if (ForegroundQueue && ForegroundQueue->spwndActive)
                    UserPostMessage(hwndSAS, WM_LOGONNOTIFY, LN_START_SCREENSAVE, 1); // lParam 1 == Secure
                else
                    UserPostMessage(hwndSAS, WM_LOGONNOTIFY, LN_START_SCREENSAVE, 0);
            }
        }
    }
}

/*
 * CreateInputDevice
 *
 * Create input device structure
 *
 */
static
PINPUT_DEVICE_INFO
CreateInputDevice(
    _In_ UCHAR DeviceType,
    _In_ PUNICODE_STRING DeviceName)
{
    PINPUT_DEVICE_INFO DeviceInfo;

    DeviceInfo = ExAllocatePoolWithTag(PagedPool, sizeof(*DeviceInfo) + DeviceName->Length + sizeof(UNICODE_NULL), USERTAG_PNP);
    if (!DeviceInfo)
    {
        ERR("Failed to allocate memory for INPUT_DEVICE_INFO\n");
        return NULL;
    }

    RtlZeroMemory(DeviceInfo, sizeof(*DeviceInfo));
    DeviceInfo->DeviceType = DeviceType;
    DeviceInfo->DeviceName.Buffer = (PWCH)(DeviceInfo + 1);
    DeviceInfo->DeviceName.MaximumLength = DeviceName->Length + sizeof(UNICODE_NULL);
    RtlCopyUnicodeString(&DeviceInfo->DeviceName, DeviceName);

    AcquireDeviceInfoListMutex();
    DeviceInfo->pNextDeviceInfo = gpInputDeviceInfo;
    gpInputDeviceInfo = DeviceInfo;
    ReleaseDeviceInfoListMutex();

    KeSetEvent(gDeviceListChangedEvent, EVENT_INCREMENT, FALSE);
    return DeviceInfo;
}

/*
 * OpenInputDevice
 *
 * Opens input device for asynchronous access
 */
static
NTSTATUS NTAPI
OpenInputDevice(
    _In_ PINPUT_DEVICE_INFO DeviceInfo)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    IO_STATUS_BLOCK Iosb;

    InitializeObjectAttributes(&ObjectAttributes,
                               &DeviceInfo->DeviceName,
                               OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&DeviceInfo->Handle,
                        FILE_ALL_ACCESS,
                        &ObjectAttributes,
                        &Iosb,
                        0,
                        0);
    if (!NT_SUCCESS(Status))
    {
        ERR("ZwOpenFile('%wZ') failed with status 0x%08x\n", &DeviceInfo->DeviceName, Status);
        return Status;
    }

    if (DeviceInfo->DeviceType == RIM_TYPEKEYBOARD)
    {
        Status = ZwDeviceIoControlFile(DeviceInfo->Handle,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &DeviceInfo->Iosb,
                                       IOCTL_KEYBOARD_QUERY_ATTRIBUTES,
                                       NULL, 0,
                                       &DeviceInfo->Keyboard.Attributes,
                                       sizeof(DeviceInfo->Keyboard.Attributes));
        if (!NT_SUCCESS(Status))
        {
            ERR("ZwDeviceIoControlFile('%wZ', IOCTL_KEYBOARD_QUERY_ATTRIBUTES) failed with status 0x%08x\n", &DeviceInfo->DeviceName, Status);
        }
    }
    else if (DeviceInfo->DeviceType == RIM_TYPEMOUSE)
    {
        Status = ZwDeviceIoControlFile(DeviceInfo->Handle,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &DeviceInfo->Iosb,
                                       IOCTL_MOUSE_QUERY_ATTRIBUTES,
                                       NULL, 0,
                                       &DeviceInfo->Mouse.Attributes,
                                       sizeof(DeviceInfo->Mouse.Attributes));
        if (!NT_SUCCESS(Status))
        {
            ERR("ZwDeviceIoControlFile('%wZ', IOCTL_MOUSE_QUERY_ATTRIBUTES) failed with status 0x%08x\n", &DeviceInfo->DeviceName, Status);
        }
    }

    return STATUS_SUCCESS;
}

static
VOID
CloseInputDevice(
    _In_ PINPUT_DEVICE_INFO DeviceInfo)
{
    ASSERT(DeviceInfo->Handle);
    (void)ZwCancelIoFile(DeviceInfo->Handle, &DeviceInfo->Iosb);
    ObCloseHandle(DeviceInfo->Handle, KernelMode);

    if (DeviceInfo->Handle == ghKeyboardDevice)
        ghKeyboardDevice = NULL;
}

static
VOID
StartReadInputDevice(
    _In_ PINPUT_DEVICE_INFO DeviceInfo)
{
    PVOID Buffer;
    SIZE_T BufferSize;
    LARGE_INTEGER ByteOffset;

    ByteOffset.QuadPart = (LONGLONG)0;

    if (DeviceInfo->DeviceType == RIM_TYPEKEYBOARD)
    {
        Buffer = &DeviceInfo->Keyboard.Data,
        BufferSize = sizeof(DeviceInfo->Keyboard.Data);
    }
    else if (DeviceInfo->DeviceType == RIM_TYPEMOUSE)
    {
        Buffer = &DeviceInfo->Mouse.Data,
        BufferSize = sizeof(DeviceInfo->Mouse.Data);
    }
    else if (DeviceInfo->DeviceType == RIM_TYPEHID && DeviceInfo->Hid.ReportBuffer)
    {
        Buffer = DeviceInfo->Hid.ReportBuffer;
        BufferSize = DeviceInfo->Hid.ReportLength;
    }
    else
        return;

    DeviceInfo->Status = ZwReadFile(
        DeviceInfo->Handle,
        NULL,
        RITReadApc,
        DeviceInfo,
        &DeviceInfo->Iosb,
        Buffer,
        BufferSize,
        &ByteOffset,
        NULL);
}

static
VOID NTAPI
RITReadApc(
    _In_ PVOID ApcContext,
    _In_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ ULONG Reserved)
{
    PINPUT_DEVICE_INFO DeviceInfo = ApcContext;

    if (DeviceInfo->Handle && NT_SUCCESS(IoStatusBlock->Status))
    {
        if (DeviceInfo->DeviceType == RIM_TYPEKEYBOARD)
        {
            TRACE("KeyboardEvent: %s %04x\n",
                  (DeviceInfo->Keyboard.Data.Flags & KEY_BREAK) ? "up" : "down",
                  DeviceInfo->Keyboard.Data.MakeCode);

            /* Set LastInputTick */
            IntLastInputTick(TRUE);

            /* Process data */
            UserEnterExclusive();
            UserProcessKeyboardInput(DeviceInfo, &DeviceInfo->Keyboard.Data);
            UserLeave();
        }
        else if (DeviceInfo->DeviceType == RIM_TYPEMOUSE)
        {
            TRACE("MouseEvent\n");

            /* Set LastInputTick */
            IntLastInputTick(TRUE);

            /* Process data */
            UserEnterExclusive();
            UserProcessMouseInput(DeviceInfo, &DeviceInfo->Mouse.Data);
            UserLeave();
        }
        else if (DeviceInfo->DeviceType == RIM_TYPEHID)
        {
            IntLastInputTick(TRUE);

            UserEnterExclusive();
            UserProcessTouchReport(DeviceInfo, DeviceInfo->Hid.ReportBuffer, (ULONG)IoStatusBlock->Information);
            UserLeave();
        }
    }

    /* A failed HID read means the collection went away, the next scan drops it */
    if (DeviceInfo->DeviceType == RIM_TYPEHID && !NT_SUCCESS(IoStatusBlock->Status))
    {
        DeviceInfo->Status = IoStatusBlock->Status;
        return;
    }

    StartReadInputDevice(DeviceInfo);
}

static
BOOLEAN
HidIsKnownDevice(
    _In_ PUNICODE_STRING Name)
{
    PINPUT_DEVICE_INFO DeviceInfo;
    PHID_SKIPPED_DEVICE Skipped;

    for (DeviceInfo = gpInputDeviceInfo; DeviceInfo; DeviceInfo = DeviceInfo->pNextDeviceInfo)
    {
        if (RtlEqualUnicodeString(&DeviceInfo->DeviceName, Name, TRUE))
            return TRUE;
    }

    for (Skipped = gHidSkippedDevices; Skipped; Skipped = Skipped->Next)
    {
        if (RtlEqualUnicodeString(&Skipped->Name, Name, TRUE))
            return TRUE;
    }

    return FALSE;
}

static
VOID
HidSkipDevice(
    _In_ PUNICODE_STRING Name)
{
    PHID_SKIPPED_DEVICE Skipped;

    Skipped = ExAllocatePoolWithTag(PagedPool, sizeof(*Skipped) + Name->Length, USERTAG_PNP);
    if (!Skipped)
        return;

    Skipped->Name.Buffer = (PWCH)(Skipped + 1);
    Skipped->Name.Length = 0;
    Skipped->Name.MaximumLength = Name->Length;
    RtlCopyUnicodeString(&Skipped->Name, Name);
    Skipped->Next = gHidSkippedDevices;
    gHidSkippedDevices = Skipped;
}

/**
 * @brief Reads the collection information and parsed descriptor of one HID collection.
 */
static
NTSTATUS
HidQueryCollection(
    _In_ PUNICODE_STRING Name,
    _Out_ PHID_DEVICE_INFO Hid)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Handle;
    NTSTATUS Status;

    RtlZeroMemory(Hid, sizeof(*Hid));

    InitializeObjectAttributes(&ObjectAttributes, Name, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&Handle,
                        GENERIC_READ | SYNCHRONIZE,
                        &ObjectAttributes,
                        &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_HID_GET_COLLECTION_INFORMATION,
                                   NULL, 0,
                                   &Hid->CollectionInformation, sizeof(Hid->CollectionInformation));
    if (NT_SUCCESS(Status) && Hid->CollectionInformation.DescriptorSize == 0)
        Status = STATUS_DEVICE_DATA_ERROR;
    if (!NT_SUCCESS(Status))
        goto Exit;

    Hid->PreparsedData = ExAllocatePoolWithTag(PagedPool, Hid->CollectionInformation.DescriptorSize, USERTAG_PNP);
    if (!Hid->PreparsedData)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                   NULL, 0,
                                   Hid->PreparsedData, Hid->CollectionInformation.DescriptorSize);
    if (NT_SUCCESS(Status) && HidP_GetCaps(Hid->PreparsedData, &Hid->Caps) != HIDP_STATUS_SUCCESS)
        Status = STATUS_DEVICE_DATA_ERROR;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Hid->PreparsedData, USERTAG_PNP);
        Hid->PreparsedData = NULL;
    }

Exit:
    ObCloseHandle(Handle, KernelMode);
    return Status;
}

static
VOID
HidFreeDevice(
    _In_ PINPUT_DEVICE_INFO DeviceInfo)
{
    if (DeviceInfo->Handle)
        ObCloseHandle(DeviceInfo->Handle, KernelMode);

    UserEnterExclusive();
    UserCleanupTouchDevice(DeviceInfo);
    UserLeave();

    if (DeviceInfo->Hid.PreparsedData)
        ExFreePoolWithTag(DeviceInfo->Hid.PreparsedData, USERTAG_PNP);
    ExFreePoolWithTag(DeviceInfo, USERTAG_PNP);
}

/**
 * @brief Opens a touch screen collection and starts reading it.
 */
static
VOID
HidAddTouchDevice(
    _In_ PUNICODE_STRING Name)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    PINPUT_DEVICE_INFO DeviceInfo;
    PINPUT_DEVICE_INFO *Link;
    IO_STATUS_BLOCK Iosb;
    HID_DEVICE_INFO Hid;
    BOOLEAN IsTouch;
    NTSTATUS Status;

    /* Collections kbdhid or mouhid hold exclusively fail here and are skipped */
    Status = HidQueryCollection(Name, &Hid);
    if (!NT_SUCCESS(Status))
    {
        HidSkipDevice(Name);
        return;
    }

    DeviceInfo = CreateInputDevice(RIM_TYPEHID, Name);
    if (!DeviceInfo)
    {
        ExFreePoolWithTag(Hid.PreparsedData, USERTAG_PNP);
        return;
    }

    DeviceInfo->Hid = Hid;

    UserEnterExclusive();
    IsTouch = UserInitTouchDevice(DeviceInfo);
    UserLeave();

    if (IsTouch)
    {
        InitializeObjectAttributes(&ObjectAttributes, Name, OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ZwOpenFile(&DeviceInfo->Handle,
                            GENERIC_READ,
                            &ObjectAttributes,
                            &Iosb,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            0);
        if (NT_SUCCESS(Status))
        {
            StartReadInputDevice(DeviceInfo);
            return;
        }

        DeviceInfo->Handle = NULL;
    }

    /* Not a usable touch screen: drop the entry, remember the name */
    AcquireDeviceInfoListMutex();
    for (Link = &gpInputDeviceInfo; *Link; Link = &(*Link)->pNextDeviceInfo)
    {
        if (*Link == DeviceInfo)
        {
            *Link = DeviceInfo->pNextDeviceInfo;
            break;
        }
    }
    ReleaseDeviceInfoListMutex();

    HidFreeDevice(DeviceInfo);
    HidSkipDevice(Name);
}

/**
 * @brief Drops HID devices whose reads failed, then picks up new collections.
 */
static
VOID
ScanHidDevices(
    VOID)
{
    PINPUT_DEVICE_INFO *Link;
    PINPUT_DEVICE_INFO DeviceInfo;
    PINPUT_DEVICE_INFO Dead = NULL;
    UNICODE_STRING Name;
    PWSTR List;
    PWSTR Entry;
    BOOLEAN Known;
    NTSTATUS Status;

    AcquireDeviceInfoListMutex();
    Link = &gpInputDeviceInfo;
    while ((DeviceInfo = *Link) != NULL)
    {
        if (DeviceInfo->DeviceType != RIM_TYPEHID || NT_SUCCESS(DeviceInfo->Status))
        {
            Link = &DeviceInfo->pNextDeviceInfo;
            continue;
        }

        *Link = DeviceInfo->pNextDeviceInfo;
        DeviceInfo->pNextDeviceInfo = Dead;
        Dead = DeviceInfo;
    }
    ReleaseDeviceInfoListMutex();

    while (Dead)
    {
        DeviceInfo = Dead;
        Dead = DeviceInfo->pNextDeviceInfo;
        HidFreeDevice(DeviceInfo);
    }

    Status = IoGetDeviceInterfaces(&HidInterfaceGuid, NULL, 0, &List);
    if (!NT_SUCCESS(Status))
        return;

    for (Entry = List; *Entry; Entry += wcslen(Entry) + 1)
    {
        RtlInitUnicodeString(&Name, Entry);

        AcquireDeviceInfoListMutex();
        Known = HidIsKnownDevice(&Name);
        ReleaseDeviceInfoListMutex();

        if (!Known)
            HidAddTouchDevice(&Name);
    }

    ExFreePool(List);
}

static
VOID
ProcessDeviceChanges(
    VOID)
{
    PINPUT_DEVICE_INFO DeviceInfo;
    NTSTATUS Status;
    DWORD NumberOfMice = 0;
    DWORD MaxNumberOfButtons = 0;
    BOOL HasMouseWheel = FALSE;
#if (_WIN32_WINNT >= 0x0600)
    BOOL HasHorizontalMouseWheel = FALSE;
#endif
    DWORD MaxFunctionKeys = 0;

    AcquireDeviceInfoListMutex();
    for (DeviceInfo = gpInputDeviceInfo; DeviceInfo; DeviceInfo = DeviceInfo->pNextDeviceInfo)
    {
        if (!DeviceInfo->Handle)
        {
            Status = OpenInputDevice(DeviceInfo);
            if (NT_SUCCESS(Status))
            {
                if (DeviceInfo->DeviceType == RIM_TYPEKEYBOARD)
                {
                    TRACE("Keyboard connected!\n");
                    if (!ghKeyboardDevice)
                    {
                        // Get and load keyboard attributes.
                        UserInitKeyboard(DeviceInfo->Handle);
                        UserEnterExclusive();
                        // Register the Window hotkey.
                        UserRegisterHotKey(PWND_BOTTOM, IDHK_WINKEY, MOD_WIN, 0);
                        // Register the Window Snap hotkey.
                        UserRegisterHotKey(PWND_BOTTOM, IDHK_SNAP_LEFT, MOD_WIN, VK_LEFT);
                        UserRegisterHotKey(PWND_BOTTOM, IDHK_SNAP_RIGHT, MOD_WIN, VK_RIGHT);
                        UserRegisterHotKey(PWND_BOTTOM, IDHK_SNAP_UP, MOD_WIN, VK_UP);
                        UserRegisterHotKey(PWND_BOTTOM, IDHK_SNAP_DOWN, MOD_WIN, VK_DOWN);
                        // Register the debug hotkeys.
                        SetDebugHotKeys();
                        UserLeave();
                    }
                }
                else if (DeviceInfo->DeviceType == RIM_TYPEMOUSE)
                {
                    TRACE("Mouse connected!\n");
                }
                StartReadInputDevice(DeviceInfo);
            }
        }

        if (DeviceInfo->DeviceType == RIM_TYPEKEYBOARD)
        {
            if (DeviceInfo->Keyboard.Attributes.KeyboardIdentifier.Type == 4 /* Enhanced 101- or 102-key keyboard */ ||
                DeviceInfo->Keyboard.Attributes.KeyboardIdentifier.Type == 7 /* Japanese keyboard */ ||
                DeviceInfo->Keyboard.Attributes.KeyboardIdentifier.Type == 8 /* Korean keyboard */)
            {
                DWORD FunctionKeys = DeviceInfo->Keyboard.Attributes.NumberOfFunctionKeys;
                /* Keep current keyboard description */
                gKeyboardInfo = DeviceInfo->Keyboard.Attributes;
                if (FunctionKeys > MaxFunctionKeys)
                    MaxFunctionKeys = FunctionKeys;
            }
        }
        else if (DeviceInfo->DeviceType == RIM_TYPEMOUSE)
        {
            DWORD MouseIdentifier = DeviceInfo->Mouse.Attributes.MouseIdentifier & ~HORIZONTAL_WHEEL_PRESENT;
            DWORD NumberOfButtons = DeviceInfo->Mouse.Attributes.NumberOfButtons;

            NumberOfMice++;
            if (NumberOfButtons > MaxNumberOfButtons)
                MaxNumberOfButtons = NumberOfButtons;
            HasMouseWheel |= MouseIdentifier == WHEELMOUSE_I8042_HARDWARE
                          || MouseIdentifier == WHEELMOUSE_SERIAL_HARDWARE
                          || MouseIdentifier == WHEELMOUSE_HID_HARDWARE;
#if (_WIN32_WINNT >= 0x0600)
            HasHorizontalMouseWheel |= DeviceInfo->Mouse.Attributes.MouseIdentifier & HORIZONTAL_WHEEL_PRESENT;
#endif
        }
    }
    ReleaseDeviceInfoListMutex();

    /* Update metrics */
    gpsi->aiSysMet[SM_MOUSEPRESENT] = NumberOfMice > 0 ? 1 : 0;
    gpsi->aiSysMet[SM_CMOUSEBUTTONS] = MaxNumberOfButtons;
    gpsi->aiSysMet[SM_MOUSEWHEELPRESENT] = HasMouseWheel ? 1 : 0;
#if (_WIN32_WINNT >= 0x0600)
    gpsi->aiSysMet[SM_MOUSEHORIZONTALWHEELPRESENT] = HasHorizontalMouseWheel ? 1 : 0;
#endif
    /* FIXME: show/hide cursor depending of mouse presence */
    TRACE("Mouse present %u, buttons %u, wheel %u\n",
          gpsi->aiSysMet[SM_MOUSEPRESENT],
          gpsi->aiSysMet[SM_CMOUSEBUTTONS],
          gpsi->aiSysMet[SM_MOUSEWHEELPRESENT]);

    gKeyboardInfo.NumberOfFunctionKeys = MaxFunctionKeys;
    TRACE("Keyboard type %u, subtype %u and number of func keys %u\n",
          gKeyboardInfo.KeyboardIdentifier.Type,
          gKeyboardInfo.KeyboardIdentifier.Subtype,
          gKeyboardInfo.NumberOfFunctionKeys);
}

/*
 * RawInputThreadMain
 *
 * Reads data from input devices and supports win32 timers
 */
VOID NTAPI
RawInputThreadMain(VOID)
{
    PINPUT_DEVICE_INFO DeviceInfo;
    NTSTATUS Status;
    //LARGE_INTEGER WaitTimeout;
    PVOID WaitObjects[4], pSignaledObject = NULL;
    LARGE_INTEGER DueTime;
    KWAIT_BLOCK WaitBlockArray[RTL_NUMBER_OF(WaitObjects)];
    PVOID ShutdownEvent;
    HWINSTA hWinSta;
    PINPUT_DEVICE_INFO Mouse;
    PINPUT_DEVICE_INFO Keyboard;
    UNICODE_STRING LegacyMouseName = RTL_CONSTANT_STRING(L"\\Device\\PointerClass0");
    UNICODE_STRING LegacyKeyboardName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");

    gDeviceListChangedEvent = ExAllocatePoolWithTag(NonPagedPool, sizeof(*gDeviceListChangedEvent), USERTAG_PNP);
    ASSERT(gDeviceListChangedEvent);
    KeInitializeEvent(gDeviceListChangedEvent, SynchronizationEvent, FALSE);

    //WaitTimeout.QuadPart = (LONGLONG)(-10000000);

    ptiRawInput = GetW32ThreadInfo();
    ptiRawInput->TIF_flags |= TIF_SYSTEMTHREAD;
    ptiRawInput->pClientInfo->dwTIFlags = ptiRawInput->TIF_flags;

    TRACE("Raw Input Thread %p\n", ptiRawInput);

    KeSetPriorityThread(&PsGetCurrentThread()->Tcb,
                        LOW_REALTIME_PRIORITY + 3);

    Status = ObOpenObjectByPointer(InputWindowStation,
                                   0,
                                   NULL,
                                   MAXIMUM_ALLOWED,
                                   ExWindowStationObjectType,
                                   UserMode,
                                   (PHANDLE)&hWinSta);
    if (NT_SUCCESS(Status))
    {
        UserSetProcessWindowStation(hWinSta);
    }
    else
    {
        ASSERT(FALSE);
        /* Failed to open the interactive winsta! What now? */
    }

    gpDeviceInfoListMutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(*gpDeviceInfoListMutex), USERTAG_SYSTEM);
    ASSERT(gpDeviceInfoListMutex);
    Status = ExInitializeResourceLite(gpDeviceInfoListMutex);
    ASSERT(NT_SUCCESS(Status));

    Mouse = CreateInputDevice(RIM_TYPEMOUSE, &LegacyMouseName);
    ASSERT(Mouse);
    Keyboard = CreateInputDevice(RIM_TYPEKEYBOARD, &LegacyKeyboardName);
    ASSERT(Keyboard);

    UserEnterExclusive();
    StartTheTimers();
    UserLeave();

    NT_ASSERT(ghKeyboardDevice == NULL);
    ghKeyboardDevice = Keyboard->Handle;

    gHidScanTimer = ExAllocatePoolWithTag(NonPagedPool, sizeof(*gHidScanTimer), USERTAG_PNP);
    ASSERT(gHidScanTimer);
    KeInitializeTimerEx(gHidScanTimer, SynchronizationTimer);
    DueTime.QuadPart = -10000LL * HID_SCAN_PERIOD_MS;
    KeSetTimerEx(gHidScanTimer, DueTime, HID_SCAN_PERIOD_MS, NULL);

    PoRequestShutdownEvent(&ShutdownEvent);
    for (;;)
    {
        /* Reset WaitHandles array */
        WaitObjects[0] = ShutdownEvent;
        WaitObjects[1] = MasterTimer;
        WaitObjects[2] = gDeviceListChangedEvent;
        WaitObjects[3] = gHidScanTimer;

        Status = KeWaitForMultipleObjects(RTL_NUMBER_OF(WaitObjects),
                                          WaitObjects,
                                          WaitAny,
                                          UserRequest,
                                          KernelMode,
                                          TRUE,
                                          NULL,//&WaitTimeout,
                                          WaitBlockArray);

        if ((Status >= STATUS_WAIT_0) &&
            (Status < (STATUS_WAIT_0 + RTL_NUMBER_OF(WaitObjects))))
        {
            /* Some device has finished reading */
            pSignaledObject = WaitObjects[Status - STATUS_WAIT_0];

            if (pSignaledObject == gDeviceListChangedEvent)
            {
                ProcessDeviceChanges();
            }
            else if (pSignaledObject == MasterTimer)
            {
                ProcessTimers();
            }
            else if (pSignaledObject == gHidScanTimer)
            {
                ScanHidDevices();
            }
            else if (pSignaledObject == ShutdownEvent)
            {
                break;
            }
            else ASSERT(FALSE);
        }
    }

    AcquireDeviceInfoListMutex();
    while (gpInputDeviceInfo)
    {
        DeviceInfo = gpInputDeviceInfo;
        gpInputDeviceInfo = DeviceInfo->pNextDeviceInfo;
        CloseInputDevice(DeviceInfo);
        ExFreePoolWithTag(DeviceInfo, USERTAG_PNP);
    }
    gpInputDeviceInfo = NULL;
    ReleaseDeviceInfoListMutex();

    ExDeleteResourceLite(gpDeviceInfoListMutex);
    ExFreePoolWithTag(gpDeviceInfoListMutex, USERTAG_SYSTEM);
    ExFreePoolWithTag(gDeviceListChangedEvent, USERTAG_PNP);

    KeCancelTimer(gHidScanTimer);
    ExFreePoolWithTag(gHidScanTimer, USERTAG_PNP);

    ERR("Raw Input Thread Exit!\n");
}

/*
 * InitInputImpl
 *
 * Inits input implementation
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
InitInputImpl(VOID)
{
    MasterTimer = ExAllocatePoolWithTag(NonPagedPool, sizeof(KTIMER), USERTAG_SYSTEM);
    if (!MasterTimer)
    {
        ERR("Failed to allocate memory\n");
        ASSERT(FALSE);
        return STATUS_UNSUCCESSFUL;
    }
    KeInitializeTimer(MasterTimer);

    return STATUS_SUCCESS;
}

BOOL FASTCALL
IntBlockInput(PTHREADINFO pti, BOOL BlockIt)
{
    PTHREADINFO OldBlock;
    ASSERT(pti);

    if(!pti->rpdesk || ((pti->TIF_flags & TIF_INCLEANUP) && BlockIt))
    {
        /*
         * Fail blocking if exiting the thread
         */

        return FALSE;
    }

    /*
     * FIXME: Check access rights of the window station
     *        e.g. services running in the service window station cannot block input
     */
    if(!ThreadHasInputAccess(pti) ||
       !IntIsActiveDesktop(pti->rpdesk))
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    ASSERT(pti->rpdesk);
    OldBlock = pti->rpdesk->BlockInputThread;
    if(OldBlock)
    {
        if(OldBlock != pti)
        {
            EngSetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        pti->rpdesk->BlockInputThread = (BlockIt ? pti : NULL);
        return OldBlock == NULL;
    }

    pti->rpdesk->BlockInputThread = (BlockIt ? pti : NULL);
    return OldBlock == NULL;
}

BOOL
APIENTRY
NtUserBlockInput(
    BOOL BlockIt)
{
    BOOL ret;

    TRACE("Enter NtUserBlockInput\n");
    UserEnterExclusive();

    ret = IntBlockInput(PsGetCurrentThreadWin32Thread(), BlockIt);

    UserLeave();
    TRACE("Leave NtUserBlockInput, ret=%i\n", ret);

    return ret;
}

BOOL
FASTCALL
IsRemoveAttachThread(PTHREADINFO pti)
{
    NTSTATUS Status;
    PATTACHINFO pai;
    BOOL Ret = TRUE;
    PTHREADINFO ptiFrom = NULL, ptiTo = NULL;

    do
    {
       if (!gpai) return TRUE;

       pai = gpai; // Bottom of the list.

       do
       {
          if (pai->pti2 == pti)
          {
             ptiFrom = pai->pti1;
             ptiTo = pti;
             break;
          }
          if (pai->pti1 == pti)
          {
             ptiFrom = pti;
             ptiTo = pai->pti2;
             break;
          }
          pai = pai->paiNext;

       } while (pai);

       if (!pai && !ptiFrom && !ptiTo) break;

       Status = UserAttachThreadInput(ptiFrom, ptiTo, FALSE);
       if (!NT_SUCCESS(Status)) Ret = FALSE;

    } while (Ret);

    return Ret;
}

NTSTATUS FASTCALL
UserAttachThreadInput(PTHREADINFO ptiFrom, PTHREADINFO ptiTo, BOOL fAttach)
{
    MSG msg;
    PATTACHINFO pai;
    PCURICON_OBJECT CurIcon;

    /* Can not be the same thread. */
    if (ptiFrom == ptiTo) return STATUS_INVALID_PARAMETER;

    /* Do not attach to system threads or between different desktops. */
    if (ptiFrom->TIF_flags & TIF_DONTATTACHQUEUE ||
        ptiTo->TIF_flags & TIF_DONTATTACHQUEUE ||
        ptiFrom->rpdesk != ptiTo->rpdesk)
        return STATUS_ACCESS_DENIED;

    /* MSDN Note:
       Keyboard and mouse events received by both threads are processed by the thread specified by the idAttachTo.
     */

    /* If Attach set, allocate and link. */
    if (fAttach)
    {
        pai = ExAllocatePoolWithTag(PagedPool, sizeof(ATTACHINFO), USERTAG_ATTACHINFO);
        if (!pai) return STATUS_NO_MEMORY;

        pai->paiNext = gpai;
        pai->pti1 = ptiFrom;
        pai->pti2 = ptiTo;
        gpai = pai;
        paiCount++;
        ERR("Attach Allocated! ptiFrom 0x%p  ptiTo 0x%p paiCount %d\n",ptiFrom,ptiTo,paiCount);

        if (ptiTo->MessageQueue != ptiFrom->MessageQueue)
        {

           ptiTo->MessageQueue->iCursorLevel -= ptiFrom->iCursorLevel;

           if (ptiFrom->MessageQueue == gpqForeground)
           {
              ERR("ptiFrom is Foreground\n");
              ptiTo->MessageQueue->spwndActive  = ptiFrom->MessageQueue->spwndActive;
              ptiTo->MessageQueue->spwndFocus   = ptiFrom->MessageQueue->spwndFocus;
              ptiTo->MessageQueue->spwndCapture = ptiFrom->MessageQueue->spwndCapture;
              ptiTo->MessageQueue->QF_flags    ^= ((ptiTo->MessageQueue->QF_flags ^ ptiFrom->MessageQueue->QF_flags) & QF_CAPTURELOCKED);
              RtlCopyMemory(&ptiTo->MessageQueue->CaretInfo,
                            &ptiFrom->MessageQueue->CaretInfo,
                            sizeof(ptiTo->MessageQueue->CaretInfo));
              IntSetFocusMessageQueue(NULL);
              IntSetFocusMessageQueue(ptiTo->MessageQueue);
              gptiForeground = ptiTo;
           }
           else
           {
              ERR("ptiFrom NOT Foreground\n");
              if ( ptiTo->MessageQueue->spwndActive == 0 )
                  ptiTo->MessageQueue->spwndActive = ptiFrom->MessageQueue->spwndActive;
              if ( ptiTo->MessageQueue->spwndFocus == 0 )
                  ptiTo->MessageQueue->spwndFocus  = ptiFrom->MessageQueue->spwndFocus;
           }

           CurIcon = ptiFrom->MessageQueue->CursorObject;

           MsqDestroyMessageQueue(ptiFrom);

           if (CurIcon)
           {
              // Could be global. Keep it above the water line!
              UserReferenceObject(CurIcon);
           }

           if (CurIcon && UserObjectInDestroy(UserHMGetHandle(CurIcon)))
           {
              UserDereferenceObject(CurIcon);
              CurIcon = NULL;
           }

           ptiFrom->MessageQueue = ptiTo->MessageQueue;

           // Pass cursor From if To is null. Pass test_SetCursor parent_id == current pti ID.
           if (CurIcon && ptiTo->MessageQueue->CursorObject == NULL)
           {
              ERR("ptiTo receiving ptiFrom Cursor\n");
              ptiTo->MessageQueue->CursorObject = CurIcon;
           }

           ptiFrom->MessageQueue->cThreads++;
           ERR("ptiTo S Share count %u\n", ptiFrom->MessageQueue->cThreads);

           IntReferenceMessageQueue(ptiTo->MessageQueue);
        }
        else
        {
           ERR("Attach Threads are already associated!\n");
        }
    }
    else /* If clear, unlink and free it. */
    {
        BOOL Hit = FALSE;
        PATTACHINFO *ppai;

        if (!gpai) return STATUS_INVALID_PARAMETER;

        /* Search list and free if found or return false. */
        ppai = &gpai;
        while (*ppai != NULL)
        {
           if ( (*ppai)->pti2 == ptiTo && (*ppai)->pti1 == ptiFrom )
           {
              pai = *ppai;
              /* Remove it from the list */
              *ppai = (*ppai)->paiNext;
              ExFreePoolWithTag(pai, USERTAG_ATTACHINFO);
              paiCount--;
              Hit = TRUE;
              break;
           }
           ppai = &((*ppai)->paiNext);
        }

        if (!Hit) return STATUS_INVALID_PARAMETER;

        ERR("Attach Free! ptiFrom 0x%p  ptiTo 0x%p paiCount %d\n",ptiFrom,ptiTo,paiCount);

        if (ptiTo->MessageQueue == ptiFrom->MessageQueue)
        {
           PWND spwndActive = ptiTo->MessageQueue->spwndActive;
           PWND spwndFocus  = ptiTo->MessageQueue->spwndFocus;

           if (gptiForeground == ptiFrom)
           {
              ERR("ptiTo is now pti FG.\n");
              // MessageQueue foreground is set so switch threads.
              gptiForeground = ptiTo;
           }
           ptiTo->MessageQueue->cThreads--;
           ERR("ptiTo E Share count %u\n", ptiTo->MessageQueue->cThreads);
           ASSERT(ptiTo->MessageQueue->cThreads >= 1);

           IntDereferenceMessageQueue(ptiTo->MessageQueue);

           ptiFrom->MessageQueue = MsqCreateMessageQueue(ptiFrom);

           if (spwndActive)
           {
              if (spwndActive->head.pti == ptiFrom)
              {
                 ptiFrom->MessageQueue->spwndActive = spwndActive;
                 ptiTo->MessageQueue->spwndActive = 0;
              }
           }
           if (spwndFocus)
           {
              if (spwndFocus->head.pti == ptiFrom)
              {
                 ptiFrom->MessageQueue->spwndFocus = spwndFocus;
                 ptiTo->MessageQueue->spwndFocus = 0;
              }
           }
           ptiTo->MessageQueue->iCursorLevel -= ptiFrom->iCursorLevel;
        }
        else
        {
           ERR("Detaching Threads are not associated!\n");
        }
    }
    /* Note that key state, which can be ascertained by calls to the GetKeyState
       or GetKeyboardState function, is reset after a call to AttachThreadInput.
       ATM which one?
     */
    RtlCopyMemory(ptiTo->MessageQueue->afKeyState, gafAsyncKeyState, sizeof(gafAsyncKeyState));

    ptiTo->MessageQueue->msgDblClk.message = 0;

    /* Generate mouse move message */
    msg.message = WM_MOUSEMOVE;
    msg.wParam = UserGetMouseButtonsState();
    msg.lParam = MAKELPARAM(gpsi->ptCursor.x, gpsi->ptCursor.y);
    msg.pt = gpsi->ptCursor;
    co_MsqInsertMouseMessage(&msg, 0, 0, TRUE);

    return STATUS_SUCCESS;
}

BOOL
APIENTRY
NtUserAttachThreadInput(
    IN DWORD idAttach,
    IN DWORD idAttachTo,
    IN BOOL fAttach)
{
  NTSTATUS Status;
  PTHREADINFO pti, ptiTo;
  BOOL Ret = FALSE;

  UserEnterExclusive();
  TRACE("Enter NtUserAttachThreadInput %s\n",(fAttach ? "TRUE" : "FALSE" ));

  pti = IntTID2PTI(UlongToHandle(idAttach));
  ptiTo = IntTID2PTI(UlongToHandle(idAttachTo));

  if ( !pti || !ptiTo )
  {
     TRACE("AttachThreadInput pti or ptiTo NULL.\n");
     EngSetLastError(ERROR_INVALID_PARAMETER);
     goto Exit;
  }

  Status = UserAttachThreadInput( pti, ptiTo, fAttach);
  if (!NT_SUCCESS(Status))
  {
     TRACE("AttachThreadInput Error Status 0x%x. \n",Status);
     EngSetLastError(RtlNtStatusToDosError(Status));
  }
  else Ret = TRUE;

Exit:
  TRACE("Leave NtUserAttachThreadInput, ret=%d\n",Ret);
  UserLeave();
  return Ret;
}

/*
 * NtUserSendInput
 *
 * Generates input events from software
 */
UINT
APIENTRY
NtUserSendInput(
    UINT nInputs,
    LPINPUT pInput,
    INT cbSize)
{
    PTHREADINFO pti;
    UINT uRet = 0;

    TRACE("Enter NtUserSendInput\n");
    UserEnterExclusive();

    pti = PsGetCurrentThreadWin32Thread();
    ASSERT(pti);

    if (!pti->rpdesk)
    {
        goto cleanup;
    }

    if (!nInputs || !pInput || cbSize != sizeof(INPUT))
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        goto cleanup;
    }

    /*
     * FIXME: Check access rights of the window station
     *        e.g. services running in the service window station cannot block input
     */
    if (!ThreadHasInputAccess(pti) ||
        !IntIsActiveDesktop(pti->rpdesk))
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        goto cleanup;
    }

    while (nInputs--)
    {
        INPUT SafeInput;
        NTSTATUS Status;

        Status = MmCopyFromCaller(&SafeInput, pInput++, sizeof(INPUT));
        if (!NT_SUCCESS(Status))
        {
            SetLastNtError(Status);
            goto cleanup;
        }

        switch (SafeInput.type)
        {
            case INPUT_MOUSE:
                if (UserSendMouseInput(&SafeInput.mi, TRUE))
                    uRet++;
                break;
            case INPUT_KEYBOARD:
                if (UserSendKeyboardInput(&SafeInput.ki, TRUE))
                    uRet++;
                break;
            case INPUT_HARDWARE:
                FIXME("INPUT_HARDWARE not supported!\n");
                break;
            default:
                ERR("SendInput(): Invalid input type: 0x%x\n", SafeInput.type);
                break;
        }
    }

cleanup:
    TRACE("Leave NtUserSendInput, ret=%u\n", uRet);
    UserLeave();
    return uRet;
}

/* EOF */
