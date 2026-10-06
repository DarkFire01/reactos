/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Virtual power coordination PDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

#define USB4HR_POWER_PDO_ID             L"USB4\\VIRTUAL_POWER_PDO"
#define USB4HR_POWER_PDO_ID_CHARS       96
#define USB4HR_DEVICE_TEXT_LCID         1033

static const UNICODE_STRING Usb4HrPowerPdoId = RTL_CONSTANT_STRING(USB4HR_POWER_PDO_ID);
static const UNICODE_STRING Usb4HrPowerPdoInstanceId = RTL_CONSTANT_STRING(L"0");
static const UNICODE_STRING Usb4HrPowerPdoDescription = RTL_CONSTANT_STRING(L"USB4 Virtual power coordination device");
static const UNICODE_STRING Usb4HrPowerPdoLocation = RTL_CONSTANT_STRING(L"USB4 Domain");

/* Characters of the type tag that starts the suffix of an open name */
#define USB4HR_OPEN_TAG_CHARS           3

/** Per handle context: the PDO the handle was opened on. */
struct Usb4HrPowerFile
{
    Usb4HrPowerPdo* PowerPdo;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrPowerFile, Usb4HrGetPowerFile);

/** Case sensitive match of a three letter type tag at the start of Suffix. */
static
BOOLEAN
NTAPI
Usb4HrOpenTagIs(
    _In_reads_(SuffixChars) PCWCH Suffix,
    _In_ SIZE_T SuffixChars,
    _In_reads_(USB4HR_OPEN_TAG_CHARS) PCWSTR Tag)
{
    SIZE_T Bytes = USB4HR_OPEN_TAG_CHARS * sizeof(WCHAR);

    if (SuffixChars < USB4HR_OPEN_TAG_CHARS)
        return FALSE;

    return RtlCompareMemory(Suffix, Tag, Bytes) == Bytes;
}

NTSTATUS
Usb4HrPowerPdo::AssignIds(
    _In_ Usb4HrHostRouter* HostRouter,
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    const Usb4HrIdentity* Identity = HostRouter->Hardware()->Identity();
    WCHAR FullBuffer[USB4HR_POWER_PDO_ID_CHARS];
    WCHAR ShortBuffer[USB4HR_POWER_PDO_ID_CHARS];
    UNICODE_STRING FullId;
    UNICODE_STRING ShortId;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&FullId, FullBuffer, sizeof(FullBuffer));
    RtlInitEmptyUnicodeString(&ShortId, ShortBuffer, sizeof(ShortBuffer));

    switch (Identity->Bus)
    {
        case Usb4HrParentBus::Pci:
            Status = RtlUnicodeStringPrintf(&FullId,
                                            USB4HR_POWER_PDO_ID L"&VID_%04X&PID_%04X&REV_%04X",
                                            Identity->VendorId,
                                            Identity->DeviceId,
                                            Identity->RevisionId);
            if (NT_SUCCESS(Status))
            {
                Status = RtlUnicodeStringPrintf(&ShortId,
                                                USB4HR_POWER_PDO_ID L"&VID_%04X&PID_%04X",
                                                Identity->VendorId,
                                                Identity->DeviceId);
            }
            break;

        case Usb4HrParentBus::Acpi:
            Status = RtlUnicodeStringPrintf(&FullId,
                                            USB4HR_POWER_PDO_ID L"&VID_%hs&PID_%hs&REV_%04X",
                                            Identity->AcpiVendor,
                                            Identity->AcpiDevice,
                                            Identity->AcpiRevision);
            if (NT_SUCCESS(Status))
            {
                Status = RtlUnicodeStringPrintf(&ShortId,
                                                USB4HR_POWER_PDO_ID L"&VID_%hs&PID_%hs",
                                                Identity->AcpiVendor,
                                                Identity->AcpiDevice);
            }
            break;

        default:
            DPRINT1("Power PDO: host router on an unknown bus %lu\n", (ULONG)Identity->Bus);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: hardware IDs not formatted 0x%lx\n", Status);
        return Status;
    }

    Status = WdfPdoInitAddHardwareID(DeviceInit, &FullId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(DeviceInit, &ShortId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(DeviceInit, &Usb4HrPowerPdoId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAssignDeviceID(DeviceInit, &Usb4HrPowerPdoId);

    if (!NT_SUCCESS(Status))
        DPRINT1("Power PDO: IDs not assigned 0x%lx\n", Status);

    return Status;
}

NTSTATUS
Usb4HrPowerPdo::Create(
    _In_ Usb4HrHostRouter* HostRouter,
    _Out_ Usb4HrPowerPdo** PowerPdo)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_OBJECT_ATTRIBUTES FileAttributes;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_DEVICE_PNP_CAPABILITIES PnpCaps;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;
    PWDFDEVICE_INIT DeviceInit;
    Usb4HrPowerPdo* Pdo;
    WDFDEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    *PowerPdo = NULL;

    DeviceInit = WdfPdoInitAllocate(HostRouter->Device());
    if (DeviceInit == NULL)
    {
        DPRINT1("Power PDO: device init not allocated\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_USB4);

    Status = WdfPdoInitAssignInstanceID(DeviceInit, &Usb4HrPowerPdoInstanceId);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: instance ID not assigned 0x%lx\n", Status);
        goto Failed;
    }

    /* The text only names the device; a failure is not fatal */
    Status = WdfPdoInitAddDeviceText(DeviceInit,
                                     &Usb4HrPowerPdoDescription,
                                     &Usb4HrPowerPdoLocation,
                                     USB4HR_DEVICE_TEXT_LCID);
    if (!NT_SUCCESS(Status))
        DPRINT1("Power PDO: device text not added 0x%lx\n", Status);

    Status = AssignIds(HostRouter, DeviceInit);
    if (!NT_SUCCESS(Status))
        goto Failed;

    Status = WdfPdoInitAssignRawDevice(DeviceInit, &GUID_USB4HR_POWER_PDO_CLASS);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: raw device class not assigned 0x%lx\n", Status);
        goto Failed;
    }

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, EvtFileCreate, EvtFileClose, WDF_NO_EVENT_CALLBACK);
    FileConfig.AutoForwardCleanupClose = WdfFalse;
    FileConfig.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&FileAttributes, Usb4HrPowerFile);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, &FileAttributes);

    /* Prepare, release and D0 exit only feed SleepStudy in Windows */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDeviceD0Entry = EvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4HrPowerPdo);
    Attributes.EvtCleanupCallback = EvtCleanup;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: device not created 0x%lx\n", Status);
        goto Failed;
    }

    Pdo = FromDevice(Device);
    Pdo->m_Device = Device;
    Pdo->m_HostRouter = HostRouter;
    InitializeListHead(&Pdo->m_OpenNames);
    KeInitializeSpinLock(&Pdo->m_DpLock);

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCaps);
    PnpCaps.Removable = WdfFalse;
    PnpCaps.UniqueID = WdfFalse;
    PnpCaps.SilentInstall = WdfTrue;
    PnpCaps.SurpriseRemovalOK = WdfFalse;
    PnpCaps.NoDisplayInUI = WdfTrue;
    PnpCaps.Address = 0xFFFFFFFF;
    PnpCaps.UINumber = 0xFFFFFFFF;
    WdfDeviceSetPnpCapabilities(Device, &PnpCaps);

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCanWakeFromS0);
    IdleSettings.DxState = PowerDeviceMaximum;
    IdleSettings.IdleTimeout = 1;
    IdleSettings.UserControlOfIdleSettings = IdleAllowUserControl;
    IdleSettings.Enabled = WdfUseDefault;
    IdleSettings.PowerUpIdleDeviceOnSystemWake = WdfUseDefault;
    IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
    IdleSettings.ExcludeD3Cold = WdfUseDefault;

    /* Windows only traces this one; the PDO works without idle support */
    Status = WdfDeviceAssignS0IdleSettings(Device, &IdleSettings);
    if (!NT_SUCCESS(Status))
        DPRINT1("Power PDO: S0 idle settings not assigned 0x%lx\n", Status);

    Status = Pdo->Initialize(HostRouter, Device);
    if (NT_SUCCESS(Status))
    {
        Status = WdfFdoAddStaticChild(HostRouter->Device(), Device);
        if (!NT_SUCCESS(Status))
            DPRINT1("Power PDO: static child not added 0x%lx\n", Status);
    }

    if (!NT_SUCCESS(Status))
    {
        /* Windows leaves the unreported PDO to the FDO's removal */
        WdfObjectDelete(Device);
        return Status;
    }

    DPRINT("Power PDO %p created, reference string %wZ\n", Pdo, &Pdo->m_ReferenceString);
    *PowerPdo = Pdo;
    return STATUS_SUCCESS;

Failed:
    if (DeviceInit != NULL)
        WdfDeviceInitFree(DeviceInit);

    return Status;
}

Usb4HrPowerPdo*
Usb4HrPowerPdo::FromDevice(
    _In_ WDFDEVICE Device)
{
    return Usb4HrGetPowerPdo(Device);
}

WDFDEVICE Usb4HrPowerPdo::Device() const
{
    return m_Device;
}

PCUNICODE_STRING Usb4HrPowerPdo::ReferenceString() const
{
    return &m_ReferenceString;
}

NTSTATUS
Usb4HrPowerPdo::Initialize(
    _In_ Usb4HrHostRouter* HostRouter,
    _In_ WDFDEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_DEVICE_STATE State;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(HostRouter);

    WDF_DEVICE_STATE_INIT(&State);
    State.NotDisableable = Usb4HrDriver.Policy.DriverDisableable ? WdfFalse : WdfTrue;
    WdfDeviceSetDeviceState(Device, &State);

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWaitLockCreate(&Attributes, &m_OpenLock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: open list lock not created 0x%lx\n", Status);
        return Status;
    }

    /* The interface itself is created by the work item the first D0 entry queues */
    return BuildReferenceString();
}

/**
 * "Usb4-Host-Interface-^<acpi>^<dvsec>^CMID<n>"; either name may be empty,
 * not both. USBHUB3 matches the fields against the names of its own ports.
 */
NTSTATUS Usb4HrPowerPdo::BuildReferenceString()
{
    Usb4HrHardware* Hardware = m_HostRouter->Hardware();
    PCWSTR AcpiName = Hardware->AcpiName();
    PCWSTR DvsecName = Hardware->DvsecName();
    WCHAR Acpi[USB4HR_NAME_CHARS];
    ULONG Index;
    NTSTATUS Status;

    if (AcpiName == NULL)
        AcpiName = L"";
    if (DvsecName == NULL)
        DvsecName = L"";

    if (AcpiName[0] == UNICODE_NULL && DvsecName[0] == UNICODE_NULL)
    {
        DPRINT1("Power PDO: host router has neither an ACPI nor a DVSEC name\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = RtlStringCchCopyW(Acpi, RTL_NUMBER_OF(Acpi), AcpiName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO: ACPI name too long 0x%lx\n", Status);
        return Status;
    }

    /* The reference string must not carry path or field separators */
    for (Index = 0; Acpi[Index] != UNICODE_NULL; Index++)
    {
        if (Acpi[Index] == L'/' || Acpi[Index] == L'\\' || Acpi[Index] == L'^')
            Acpi[Index] = L'#';
    }

    RtlInitEmptyUnicodeString(&m_ReferenceString, m_ReferenceBuffer, sizeof(m_ReferenceBuffer));
    Status = RtlUnicodeStringPrintf(&m_ReferenceString,
                                    L"%ws^%ws^%ws^CMID%u",
                                    USB4HR_REFERENCE_PREFIX,
                                    Acpi,
                                    DvsecName,
                                    (ULONG)Hardware->Cmid());
    if (!NT_SUCCESS(Status))
        DPRINT1("Power PDO: reference string not formatted 0x%lx\n", Status);

    return Status;
}

NTSTATUS Usb4HrPowerPdo::WaitForDomain()
{
    Usb4HrTunnelManager* Tunnels = m_HostRouter->Tunnels();

    if (Tunnels->HasPoweredDownTunnels())
        return Tunnels->WaitForPowerUp(USB4HR_DOMAIN_POWER_WAIT_MS);

    return m_HostRouter->Topology()->WaitForRootRouterPoweredOn(USB4HR_ROOT_POWER_WAIT_MS);
}

NTSTATUS
Usb4HrPowerPdo::D0Entry(
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    WDF_WORKITEM_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFWORKITEM WorkItem;
    NTSTATUS Status;

    m_PowerAction = WdfDeviceGetSystemPowerAction(m_Device);

    if (PreviousState == WdfPowerDeviceD3Final)
    {
        WDF_WORKITEM_CONFIG_INIT(&Config, EvtInterfaceEnable);
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = m_Device;

        Status = WdfWorkItemCreate(&Config, &Attributes, &WorkItem);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Power PDO %p: interface work item not created 0x%lx\n", this, Status);
            return Status;
        }

        WdfWorkItemEnqueue(WorkItem);
        return STATUS_SUCCESS;
    }

    /* QUIRK: D0 entry succeeds even when the domain did not come up in time */
    Status = WaitForDomain();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO %p: domain not powered up from D%lu, action %lu, 0x%lx\n",
                this, (ULONG)PreviousState - WdfPowerDeviceD0, (ULONG)m_PowerAction, Status);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrPowerPdo::ParseOpenName(
    _In_ PCUNICODE_STRING Name,
    _Out_ Usb4HrPowerOpenType* Type,
    _Out_ PUCHAR Adapter) const
{
    SIZE_T NameChars = Name->Length / sizeof(WCHAR);
    SIZE_T SuffixStart = m_ReferenceString.Length / sizeof(WCHAR) + 1;
    SIZE_T SuffixChars;
    PCWCH Suffix;
    UNICODE_STRING Digits;
    ULONG Value;
    NTSTATUS Status;

    *Type = Usb4HrPowerOpenType::None;
    *Adapter = 0;

    /* The name is "\<reference string><suffix>" and the suffix may not be empty */
    if (NameChars <= SuffixStart)
    {
        DPRINT1("Power PDO %p: open name %wZ has no suffix\n", this, Name);
        return STATUS_INVALID_PARAMETER;
    }

    /* QUIRK: only the length of the reference string part counts, not its text */
    Suffix = &Name->Buffer[SuffixStart];
    SuffixChars = NameChars - SuffixStart;

    if (Usb4HrOpenTagIs(Suffix, SuffixChars, L"USB"))
    {
        *Type = Usb4HrPowerOpenType::Usb;
        return STATUS_SUCCESS;
    }

    if (Usb4HrOpenTagIs(Suffix, SuffixChars, L"PCI"))
    {
        *Type = Usb4HrPowerOpenType::Pci;
        return STATUS_SUCCESS;
    }

    if (!Usb4HrOpenTagIs(Suffix, SuffixChars, L"GFX"))
    {
        DPRINT1("Power PDO %p: open name %wZ has an unknown type\n", this, Name);
        return STATUS_INVALID_PARAMETER;
    }

    /* "GFX", "GFX-..." and "GFX" plus a single character are generic graphics opens */
    if (SuffixChars < USB4HR_OPEN_TAG_CHARS + 2 || Suffix[USB4HR_OPEN_TAG_CHARS] == L'-')
    {
        *Type = Usb4HrPowerOpenType::Graphics;
        return STATUS_SUCCESS;
    }

    Digits.Buffer = (PWCH)&Suffix[USB4HR_OPEN_TAG_CHARS];
    Digits.Length = 2 * sizeof(WCHAR);
    Digits.MaximumLength = Digits.Length;

    Status = RtlUnicodeStringToInteger(&Digits, 16, &Value);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO %p: adapter of %wZ not parsed 0x%lx\n", this, Name, Status);
        return Status;
    }

    if (Value >= USB4HR_DP_ADAPTER_SLOTS)
    {
        DPRINT1("Power PDO %p: open names adapter 0x%lx\n", this, Value);
        return STATUS_INVALID_PARAMETER;
    }

    *Type = Usb4HrPowerOpenType::GraphicsAdapter;
    *Adapter = (UCHAR)Value;
    return STATUS_SUCCESS;
}

VOID
Usb4HrPowerPdo::RecordOpen(
    _In_ PCUNICODE_STRING Name)
{
    OpenName* Entry;

    Entry = (OpenName*)ExAllocatePoolWithTag(PagedPool, sizeof(*Entry) + Name->Length, USB4HR_TAG_CHILD);
    if (Entry == NULL)
    {
        DPRINT1("Power PDO %p: open name %wZ not recorded\n", this, Name);
        return;
    }

    Entry->Name.Buffer = (PWCH)(Entry + 1);
    Entry->Name.Length = Name->Length;
    Entry->Name.MaximumLength = Name->Length;
    RtlCopyMemory(Entry->Name.Buffer, Name->Buffer, Name->Length);

    Usb4HrWaitLockGuard Guard(m_OpenLock);
    InsertHeadList(&m_OpenNames, &Entry->Link);
}

VOID
Usb4HrPowerPdo::ForgetOpen(
    _In_ PCUNICODE_STRING Name)
{
    OpenName* Found = NULL;
    PLIST_ENTRY Current;

    {
        Usb4HrWaitLockGuard Guard(m_OpenLock);

        for (Current = m_OpenNames.Flink; Current != &m_OpenNames; Current = Current->Flink)
        {
            OpenName* Entry = CONTAINING_RECORD(Current, OpenName, Link);

            if (RtlEqualUnicodeString(&Entry->Name, Name, FALSE))
            {
                RemoveEntryList(Current);
                Found = Entry;
                break;
            }
        }
    }

    if (Found == NULL)
    {
        DPRINT1("Power PDO %p: closed %wZ that was never recorded\n", this, Name);
        return;
    }

    ExFreePoolWithTag(Found, USB4HR_TAG_CHILD);
}

NTSTATUS
Usb4HrPowerPdo::FileCreate(
    _In_ WDFFILEOBJECT FileObject)
{
    PUNICODE_STRING Name = WdfFileObjectGetFileName(FileObject);
    Usb4HrPowerOpenType Type;
    UCHAR Adapter;
    NTSTATUS Status;

    if (Name == NULL || Name->Length == 0)
    {
        DPRINT1("Power PDO %p: open without a name\n", this);
        return STATUS_INVALID_PARAMETER;
    }

    Status = ParseOpenName(Name, &Type, &Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT("Power PDO %p: opened as %wZ, type %lu\n", this, Name, (ULONG)Type);
    RecordOpen(Name);

    if (Type != Usb4HrPowerOpenType::GraphicsAdapter)
        return STATUS_SUCCESS;

    Status = ClaimGraphics(Adapter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO %p: graphics claim of DP IN %u failed 0x%lx\n", this, Adapter, Status);

        /* Windows keeps the name of the failed open on the list */
        ForgetOpen(Name);
    }

    return Status;
}

VOID
Usb4HrPowerPdo::FileClose(
    _In_ WDFFILEOBJECT FileObject)
{
    PUNICODE_STRING Name = WdfFileObjectGetFileName(FileObject);
    Usb4HrPowerOpenType Type;
    UCHAR Adapter;
    NTSTATUS Status;

    if (Name == NULL || Name->Length == 0)
    {
        DPRINT1("Power PDO %p: close without a name\n", this);
        return;
    }

    Status = ParseOpenName(Name, &Type, &Adapter);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Power PDO %p: close of %wZ with a bad name\n", this, Name);
        return;
    }

    DPRINT("Power PDO %p: closed %wZ\n", this, Name);
    ForgetOpen(Name);

    if (Type == Usb4HrPowerOpenType::GraphicsAdapter)
    {
        Status = RelinquishGraphics(Adapter);
        if (!NT_SUCCESS(Status))
            DPRINT1("Power PDO %p: graphics release of DP IN %u failed 0x%lx\n", this, Adapter, Status);
    }
}

/**
 * The graphics driver takes over the power dependency of a tunneled DP IN
 * adapter: the idle reference the PDO held for it is dropped.
 */
NTSTATUS
Usb4HrPowerPdo::ClaimGraphics(
    _In_ UCHAR Adapter)
{
    Usb4HrSpinLockGuard Guard(&m_DpLock);
    DpInAdapter* State = &m_DpAdapters[Adapter];

    if (!State->Present || !State->TunnelActive)
    {
        DPRINT1("Power PDO %p: graphics claim of DP IN %u without a tunnel\n", this, Adapter);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (State->Claims != 0)
    {
        DPRINT1("Power PDO %p: DP IN %u claimed again, %u claims\n", this, Adapter, State->Claims);

        if (Usb4HrDriver.Policy.BugcheckOnDuplicateGfxRef)
            KeBugCheck(BUGCODE_USB_DRIVER);

        if (State->Claims > USB4HR_MAX_GFX_CLAIMS)
            return STATUS_OBJECT_NAME_COLLISION;
    }

    if (State->ReferenceHeld)
    {
        WdfDeviceResumeIdle(m_Device);
        State->ReferenceHeld = FALSE;
    }
    else if (State->Claims == 0)
    {
        DPRINT("Power PDO %p: DP IN %u claimed with no reference held\n", this, Adapter);
    }

    State->Claims++;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrPowerPdo::RelinquishGraphics(
    _In_ UCHAR Adapter)
{
    Usb4HrSpinLockGuard Guard(&m_DpLock);
    DpInAdapter* State = &m_DpAdapters[Adapter];
    NTSTATUS Status = STATUS_SUCCESS;

    if (!State->Present)
    {
        DPRINT1("Power PDO %p: graphics release of unknown DP IN %u\n", this, Adapter);
        return STATUS_INVALID_PARAMETER;
    }

    if (State->ReferenceHeld)
        DPRINT1("Power PDO %p: DP IN %u released while the PDO holds its reference\n", this, Adapter);

    if (State->Claims != 0)
        State->Claims -= 1;
    else
        DPRINT1("Power PDO %p: DP IN %u released without a claim\n", this, Adapter);

    /* The tunnel still runs: keep the domain up again until the next claim */
    if (State->TunnelActive && State->Claims == 0)
    {
        Status = WdfDeviceStopIdle(m_Device, FALSE);
        if (NT_SUCCESS(Status))
            State->ReferenceHeld = TRUE;
        else
            DPRINT1("Power PDO %p: idle reference for DP IN %u not taken 0x%lx\n", this, Adapter, Status);
    }

    return Status;
}

/**
 * A DP tunnel on a host router DP IN adapter keeps the PDO, and so the host
 * router, in D0 until graphics claims the adapter. No DP tunnels exist yet,
 * so nothing calls this.
 */
NTSTATUS
Usb4HrPowerPdo::DpTunnelConfigured(
    _In_ UCHAR Adapter,
    _In_ BOOLEAN AltModeWorkaround)
{
    Usb4HrSpinLockGuard Guard(&m_DpLock);
    DpInAdapter* State;
    NTSTATUS Status;

    if (Adapter >= USB4HR_DP_ADAPTER_SLOTS)
        return STATUS_INVALID_PARAMETER;

    State = &m_DpAdapters[Adapter];
    State->Present = TRUE;

    if (AltModeWorkaround)
    {
        if (State->TunnelActive || State->InAltMode)
        {
            DPRINT1("Power PDO %p: DP IN %u busy, alt mode workaround refused\n", this, Adapter);
            return STATUS_UNSUCCESSFUL;
        }

        State->InAltMode = TRUE;
        if (State->Claims != 0)
            DPRINT("Power PDO %p: DP IN %u in alt mode while claimed\n", this, Adapter);
    }
    else if (State->InAltMode)
    {
        DPRINT1("Power PDO %p: DP IN %u tunneled during the alt mode workaround\n", this, Adapter);
    }

    State->TunnelActive = TRUE;

    if (!State->ReferenceHeld && State->Claims == 0)
    {
        Status = WdfDeviceStopIdle(m_Device, FALSE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Power PDO %p: idle reference for DP IN %u not taken 0x%lx\n", this, Adapter, Status);
            return Status;
        }

        State->ReferenceHeld = TRUE;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4HrPowerPdo::DpTunnelTornDown(
    _In_ UCHAR Adapter,
    _In_ BOOLEAN AltModeWorkaround)
{
    Usb4HrSpinLockGuard Guard(&m_DpLock);
    DpInAdapter* State;

    /* Windows goes on with a NULL adapter when it is not known */
    if (Adapter >= USB4HR_DP_ADAPTER_SLOTS || !m_DpAdapters[Adapter].Present)
    {
        DPRINT1("Power PDO %p: teardown of unknown DP IN %u\n", this, Adapter);
        return;
    }

    State = &m_DpAdapters[Adapter];

    if (AltModeWorkaround)
    {
        if (!State->InAltMode)
            DPRINT1("Power PDO %p: DP IN %u was not in the alt mode workaround\n", this, Adapter);

        State->InAltMode = FALSE;
    }
    else if (State->InAltMode)
    {
        DPRINT1("Power PDO %p: DP IN %u torn down during the alt mode workaround\n", this, Adapter);
    }

    State->TunnelActive = FALSE;

    if (State->ReferenceHeld)
    {
        WdfDeviceResumeIdle(m_Device);
        State->ReferenceHeld = FALSE;
    }
}

VOID Usb4HrPowerPdo::Cleanup()
{
    while (!IsListEmpty(&m_OpenNames))
    {
        PLIST_ENTRY Current = RemoveHeadList(&m_OpenNames);

        ExFreePoolWithTag(CONTAINING_RECORD(Current, OpenName, Link), USB4HR_TAG_CHILD);
    }
}

/* KMDF callbacks *************************************************************/

VOID
NTAPI
Usb4HrPowerPdo::EvtCleanup(
    _In_ WDFOBJECT Object)
{
    Usb4HrPowerPdo* Pdo = FromDevice((WDFDEVICE)Object);

    /* Creation can fail before the list is set up */
    if (Pdo->m_OpenNames.Flink != NULL)
        Pdo->Cleanup();
}

NTSTATUS
NTAPI
Usb4HrPowerPdo::EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    return FromDevice(Device)->D0Entry(PreviousState);
}

VOID
NTAPI
Usb4HrPowerPdo::EvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject)
{
    Usb4HrPowerPdo* Pdo = FromDevice(Device);

    Usb4HrGetPowerFile(FileObject)->PowerPdo = Pdo;
    WdfRequestComplete(Request, Pdo->FileCreate(FileObject));
}

VOID
NTAPI
Usb4HrPowerPdo::EvtFileClose(
    _In_ WDFFILEOBJECT FileObject)
{
    Usb4HrGetPowerFile(FileObject)->PowerPdo->FileClose(FileObject);
}

/** First start: wait for the domain, then announce the interface. */
VOID
NTAPI
Usb4HrPowerPdo::EvtInterfaceEnable(
    _In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    Usb4HrPowerPdo* Pdo = FromDevice(Device);
    NTSTATUS Status;

    Status = Pdo->WaitForDomain();
    if (!NT_SUCCESS(Status))
        DPRINT1("Power PDO %p: domain not powered up at first start 0x%lx\n", Pdo, Status);

    /*
     * KMDF enables every interface that exists at start, so it is only created
     * now. QUIRK: it comes up even when the domain did not.
     */
    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_USB4HR_VIRTUAL_POWER_INTERFACE, &Pdo->m_ReferenceString);
    if (NT_SUCCESS(Status))
        WdfDeviceSetDeviceInterfaceState(Device, &GUID_USB4HR_VIRTUAL_POWER_INTERFACE, &Pdo->m_ReferenceString, TRUE);
    else
        DPRINT1("Power PDO %p: interface not created 0x%lx\n", Pdo, Status);

    WdfObjectDelete(WorkItem);
}
