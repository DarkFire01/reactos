/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI thermal-zone class device interface, thermal IOCTLs and WMI provider
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

// The kernel thermal manager drives a zone through the thermal IOCTLs; the
// WMI block is the MSAcpi_ThermalZoneTemperature view of the same zone.
//
// Per-release contract (NTDDI gated):
//   Vista/Win7  THERMAL_INFORMATION with the _PSL processor mask; IOCTLs 0x20-0x22.
//   Win8/8.1    THERMAL_INFORMATION_EX (76 bytes, _HOT); SET_PASSIVE_LIMIT; _NTT/_DTI;
//               every trip/cooling change re-reads the temperature.
//   Win10+      THERMAL_INFORMATION_EX (88 bytes, _CR3/_MTL/_DSM); _STR description
//               IOCTL; the sample period is reported in ms.

#include "acpipriv.h"
#include <wmilib.h>
#include <wmistr.h>

extern const GUID GUID_DEVICE_THERMAL_ZONE;   // guid.c

// WMI class {A1BC18C0-A7C8-11d1-BF3C-00A0C9062910}; storage in guid.c.
extern const GUID MSAcpi_ThermalZoneTemperature_GUID;

// MSAcpi_ThermalZoneTemperature MOF layout (all temperatures in deci-Kelvin).
#include <pshpack1.h>
typedef struct _ACPI_THERMAL_WMI
{
    ULONG ThermalStamp;
    ULONG ThermalConstant1;      // _TC1
    ULONG ThermalConstant2;      // _TC2
    ULONG Reserved;
    ULONG SamplingPeriod;        // _TSP
    ULONG CurrentTemperature;    // _TMP
    ULONG PassiveTripPoint;      // _PSV
    ULONG CriticalTripPoint;     // _CRT
    ULONG ActiveTripPointCount;  // number of _ACx present
    ULONG ActiveTripPoint[10];   // _AC0.._AC9
} UACPI_THERMAL_WMI;
#include <poppack.h>

static WMIGUIDREGINFO UacpiThermalWmiGuidList[] = {
    { &MSAcpi_ThermalZoneTemperature_GUID, 1, 0 },
};

static ULONG UacpiThermalStamp;

// poclass.h: FILE_DEVICE_BATTERY, METHOD_BUFFERED.
#define IOCTL_THERMAL_QUERY_INFORMATION   0x00294080   // fn 0x20
#define IOCTL_THERMAL_SET_COOLING_POLICY  0x00298084   // fn 0x21
#define IOCTL_RUN_ACTIVE_COOLING_METHOD   0x00298088   // fn 0x22
#define IOCTL_THERMAL_SET_PASSIVE_LIMIT   0x0029808C   // fn 0x23, Win8+
#define UACPI_IOCTL_THERMAL_DESCRIPTION   0x00294098   // fn 0x26, Win10+ (_STR)

#define UACPI_COOLING_PASSIVE   1       // _SCP argument the zone starts with
#define UACPI_THERMAL_DESC_TAG  'TpcA'  // 'AcpT': the kernel frees the description

// QUERY_INFORMATION output, one layout per release.
#if (NTDDI_VERSION >= NTDDI_WIN10)
typedef struct _UACPI_TZ_QUERY_INFO
{
    ULONG ThermalStamp;
    ULONG ThermalConstant1;
    ULONG ThermalConstant2;
    ULONG SamplingPeriod;              // ms
    ULONG CurrentTemperature;
    ULONG PassiveTripPoint;
    ULONG ThermalStandbyTripPoint;
    ULONG CriticalTripPoint;
    UCHAR ActiveTripPointCount;
    ULONG ActiveTripPoint[10];
    ULONG S4TransitionTripPoint;
    ULONG MinimumThrottle;
    ULONG OverThrottleThreshold;
} UACPI_TZ_QUERY_INFO;
C_ASSERT(sizeof(UACPI_TZ_QUERY_INFO) == 88);
#elif (NTDDI_VERSION >= NTDDI_WIN8)
typedef struct _UACPI_TZ_QUERY_INFO
{
    ULONG ThermalStamp;
    ULONG ThermalConstant1;
    ULONG ThermalConstant2;
    ULONG SamplingPeriod;
    ULONG CurrentTemperature;
    ULONG PassiveTripPoint;
    ULONG CriticalTripPoint;
    UCHAR ActiveTripPointCount;
    ULONG ActiveTripPoint[10];
    ULONG S4TransitionTripPoint;
} UACPI_TZ_QUERY_INFO;
C_ASSERT(sizeof(UACPI_TZ_QUERY_INFO) == 76);
#else
typedef struct _UACPI_TZ_QUERY_INFO
{
    ULONG     ThermalStamp;
    ULONG     ThermalConstant1;
    ULONG     ThermalConstant2;
    KAFFINITY Processors;
    ULONG     SamplingPeriod;
    ULONG     CurrentTemperature;
    ULONG     PassiveTripPoint;
    ULONG     CriticalTripPoint;
    UCHAR     ActiveTripPointCount;
    ULONG     ActiveTripPoint[10];
} UACPI_TZ_QUERY_INFO;
C_ASSERT(sizeof(UACPI_TZ_QUERY_INFO) == (sizeof(KAFFINITY) == 8 ? 88 : 76));
#endif

// Steps the zone worker owes, run in this order.
#define UACPI_TZ_INIT          0x01   // _DSM mask, _STR (Win10); once per start
#define UACPI_TZ_POLICY        0x02   // _SCP(CoolingPolicy)
#define UACPI_TZ_TRIPS         0x04   // constants, trip points, _PSL mask
#define UACPI_TZ_ACTIVE        0x08   // _ALx devices for CoolingLevel
#define UACPI_TZ_TEMPERATURE   0x10   // _TMP; bumps ThermalStamp
#define UACPI_TZ_COMPLETE      0x20   // complete the queued thermal IRPs

static LONG UacpiThermalQuerySizeNoted;

// Evaluate a 4-char thermal method to an integer; FALSE when absent or failing.
static BOOLEAN
UacpipThermalEvalInt(uacpi_namespace_node *node, const char *name, PULONG out)
{
    uacpi_u64 v = 0;

    if (node == NULL ||
        uacpi_unlikely_error(uacpi_eval_simple_integer(node, name, &v)))
    {
        return FALSE;
    }
    *out = (ULONG)v;
    return TRUE;
}

// Evaluate a 4-char thermal method to an integer; returns 'dflt' if absent.
static ULONG
UacpiThermalEvalOr(uacpi_namespace_node *node, const char *name, ULONG dflt)
{
    ULONG v;
    return UacpipThermalEvalInt(node, name, &v) ? v : dflt;
}

// Run a method that takes one Integer and whose result is unused (_SCP, _DTI).
static VOID
UacpipThermalEvalArg(uacpi_namespace_node *node, const char *name, ULONG arg)
{
    uacpi_object *obj;
    uacpi_object_array args;

    if (node == NULL || !UacpiNodeHasChild(node, name))
    {
        return;
    }
    obj = uacpi_object_create_integer(arg);
    if (obj == NULL)
    {
        return;
    }
    args.objects = &obj;
    args.count   = 1;
    (void)uacpi_eval(node, name, &args, NULL);
    uacpi_object_unref(obj);
}

// _ALx / _PSL elements name devices; resolve them relative to the zone.
static uacpi_namespace_node *
UacpipThermalResolve(uacpi_object *elem, uacpi_namespace_node *scope)
{
    uacpi_namespace_node *node = NULL;

    if (uacpi_unlikely_error(uacpi_object_resolve_as_aml_namepath(elem, scope, &node)))
    {
        return NULL;
    }
    return node;
}

#if (NTDDI_VERSION >= NTDDI_WIN10)
// Thermal extensions _DSM {14D399CD-7A27-4B18-8FB4-7CB7B9F4E500}, revision 0.
static const GUID UacpiThermalDsmUuid =
    { 0x14D399CD, 0x7A27, 0x4B18, { 0x8F, 0xB4, 0x7C, 0xB7, 0xB9, 0xF4, 0xE5, 0x00 } };

static BOOLEAN
UacpipThermalDsm(PUACPI_PDO Pdo, ULONG Function, uacpi_object **Ret)
{
    uacpi_object *argv[4] = { 0 };
    uacpi_object_array args, empty = { 0 };
    uacpi_data_view view;
    BOOLEAN ok = FALSE;
    ULONG i;

    *Ret = NULL;
    if (!UacpiNodeHasChild(Pdo->Node, "_DSM"))
    {
        return FALSE;
    }

    view.bytes  = (uacpi_u8 *)&UacpiThermalDsmUuid;
    view.length = sizeof(GUID);
    argv[0] = uacpi_object_create_buffer(view);
    argv[1] = uacpi_object_create_integer(0);          // revision
    argv[2] = uacpi_object_create_integer(Function);
    argv[3] = uacpi_object_create_package(empty);
    for (i = 0; i < 4; i++)
    {
        if (argv[i] == NULL)
        {
            goto out;
        }
    }

    args.objects = argv;
    args.count   = 4;
    ok = uacpi_likely_success(uacpi_eval(Pdo->Node, "_DSM", &args, Ret)) && *Ret != NULL;
out:
    for (i = 0; i < 4; i++)
    {
        if (argv[i] != NULL)
        {
            uacpi_object_unref(argv[i]);
        }
    }
    return ok;
}

// A supported _DSM function returning an Integer, clamped to a percentage.
static BOOLEAN
UacpipThermalDsmPercent(PUACPI_PDO Pdo, ULONG Function, PULONG Out)
{
    uacpi_object *ret;
    uacpi_u64 v = 0;
    BOOLEAN ok;

    if ((Pdo->Thermal.DsmSupport & (1ul << Function)) == 0 ||
        !UacpipThermalDsm(Pdo, Function, &ret))
    {
        return FALSE;
    }
    ok = uacpi_likely_success(uacpi_object_get_integer(ret, &v));
    uacpi_object_unref(ret);
    if (ok)
    {
        *Out = (v > 100) ? 100 : (ULONG)v;
    }
    return ok;
}

// _STR: a NUL-terminated UTF-16 buffer naming the zone.
static VOID
UacpipThermalReadDescription(PUACPI_PDO Pdo)
{
    uacpi_object *obj = NULL;
    uacpi_data_view view;
    PWCHAR copy;

    if (uacpi_unlikely_error(uacpi_eval_simple_buffer(Pdo->Node, "_STR", &obj)) ||
        obj == NULL)
    {
        return;
    }
    if (uacpi_likely_success(uacpi_object_get_buffer(obj, &view)) &&
        view.length > sizeof(WCHAR) && (view.length % sizeof(WCHAR)) == 0 &&
        view.length <= MAXUSHORT &&
        ((const WCHAR *)view.const_bytes)[view.length / sizeof(WCHAR) - 1] == L'\0')
    {
        copy = (PWCHAR)ExAllocatePoolWithTag(NonPagedPool, view.length, UACPI_POOL_TAG);
        if (copy != NULL)
        {
            RtlCopyMemory(copy, view.const_bytes, view.length);
            Pdo->Thermal.Description.Buffer        = copy;
            Pdo->Thermal.Description.Length        = (USHORT)(wcslen(copy) * sizeof(WCHAR));
            Pdo->Thermal.Description.MaximumLength =
                (USHORT)(Pdo->Thermal.Description.Length + sizeof(WCHAR));
        }
    }
    uacpi_object_unref(obj);
}
#endif

// Worker steps (PASSIVE_LEVEL)

static VOID
UacpipThermalInit(PUACPI_PDO Pdo)
{
#if (NTDDI_VERSION >= NTDDI_WIN10)
    uacpi_object *ret;
    uacpi_data_view view;

    // _DSM function 0: byte 0 of the result is the supported-function mask.
    Pdo->Thermal.DsmSupport = 0;
    if (UacpipThermalDsm(Pdo, 0, &ret))
    {
        if (uacpi_likely_success(uacpi_object_get_buffer(ret, &view)) && view.length >= 1)
        {
            Pdo->Thermal.DsmSupport = view.const_bytes[0];
        }
        uacpi_object_unref(ret);
    }
    if (Pdo->Thermal.Description.Buffer == NULL)
    {
        UacpipThermalReadDescription(Pdo);
    }
#endif
    Pdo->Thermal.DtiArmed = FALSE;
}

// _SCP(policy): the platform reorders its trip points for active or passive.
static VOID
UacpipThermalSetPolicy(PUACPI_PDO Pdo, UCHAR Policy)
{
    UacpipThermalEvalArg(Pdo->Node, "_SCP", Policy);
}

#if (NTDDI_VERSION < NTDDI_WIN8)
// Processors _PSL names for passive cooling. With no ACPI-to-NT processor map,
// any listed processor stands for the whole active set.
static KAFFINITY
UacpipThermalPslMask(PUACPI_PDO Pdo)
{
    uacpi_object *pkg = NULL;
    uacpi_object_array arr;
    KAFFINITY mask = 0;
    uacpi_size i;

    if (uacpi_unlikely_error(uacpi_eval_simple_package(Pdo->Node, "_PSL", &pkg)) ||
        pkg == NULL)
    {
        return 0;
    }
    if (uacpi_likely_success(uacpi_object_get_package(pkg, &arr)))
    {
        for (i = 0; i < arr.count; i++)
        {
            if (UacpipThermalResolve(arr.objects[i], Pdo->Node) != NULL)
            {
                mask = KeQueryActiveProcessors();
                break;
            }
        }
    }
    uacpi_object_unref(pkg);
    return mask;
}
#endif

// Constants and trip points; a missing method reads as 0.
static VOID
UacpipThermalReadTrips(PUACPI_PDO Pdo)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    UACPI_THERMAL_INFO *info = &tz->Info;
    uacpi_namespace_node *node = Pdo->Node;
    ULONG i, v;

    info->ThermalConstant1  = UacpiThermalEvalOr(node, "_TC1", 0);
    info->ThermalConstant2  = UacpiThermalEvalOr(node, "_TC2", 0);
    info->PassiveTripPoint  = UacpiThermalEvalOr(node, "_PSV", 0);
    info->CriticalTripPoint = UacpiThermalEvalOr(node, "_CRT", 0);
#if (NTDDI_VERSION >= NTDDI_WIN10)
    // Win10 reports the period in ms: _TFP, else _TSP (tenths of a second).
    if (UacpipThermalEvalInt(node, "_TFP", &v))
    {
        info->SamplingPeriod = v;
    }
    else
    {
        info->SamplingPeriod = UacpiThermalEvalOr(node, "_TSP", 0) * 100;
    }
    info->StandbyTripPoint = UacpiThermalEvalOr(node, "_CR3", 0);
#else
    info->SamplingPeriod = UacpiThermalEvalOr(node, "_TSP", 0);
#endif
#if (NTDDI_VERSION >= NTDDI_WIN8)
    info->S4TripPoint = UacpiThermalEvalOr(node, "_HOT", 0);
    tz->NotifyDelta   = UacpiThermalEvalOr(node, "_NTT", 0);
#endif

    RtlZeroMemory(info->ActiveTripPoint, sizeof(info->ActiveTripPoint));
    for (i = 0; i < RTL_NUMBER_OF(info->ActiveTripPoint); i++)
    {
        char name[5] = { '_', 'A', 'C', (char)('0' + i), 0 };
        if (!UacpipThermalEvalInt(node, name, &info->ActiveTripPoint[i]))
        {
            break;   // _ACx are contiguous from _AC0
        }
    }
    info->ActiveTripPointCount = i;

#if (NTDDI_VERSION >= NTDDI_WIN10)
    if (UacpipThermalEvalInt(node, "_MTL", &v))
    {
        info->MinimumThrottle = (v > 100) ? 100 : v;
    }
    else if (!UacpipThermalDsmPercent(Pdo, 1, &info->MinimumThrottle))
    {
        info->MinimumThrottle = 0;
    }
    if (!UacpipThermalDsmPercent(Pdo, 3, &info->OverThrottleThreshold))
    {
        info->OverThrottleThreshold = 0;
    }
#elif (NTDDI_VERSION < NTDDI_WIN8)
    info->Processors = UacpipThermalPslMask(Pdo);
    UNREFERENCED_PARAMETER(v);
#else
    UNREFERENCED_PARAMETER(v);
#endif

    UacpiTrace("[acpi] thermal: %s trips PSV %u CRT %u AC count %u\n", Pdo->Name,
               info->PassiveTripPoint, info->CriticalTripPoint,
               info->ActiveTripPointCount);
}

// _ALx lists the devices that run at active level x and above; the rest go
// to D3. Level 0 runs every list.
static VOID
UacpipThermalRunActive(PUACPI_PDO Pdo, UCHAR Level)
{
    ULONG i;

    for (i = 0; i < 10; i++)
    {
        char name[5] = { '_', 'A', 'L', (char)('0' + i), 0 };
        DEVICE_POWER_STATE state = (i >= Level) ? PowerDeviceD0 : PowerDeviceD3;
        uacpi_object *pkg = NULL;
        uacpi_object_array arr;
        uacpi_size j;

        if (uacpi_unlikely_error(uacpi_eval_simple_package(Pdo->Node, name, &pkg)) ||
            pkg == NULL)
        {
            break;   // _ALx are contiguous from _AL0
        }
        if (uacpi_likely_success(uacpi_object_get_package(pkg, &arr)))
        {
            for (j = 0; j < arr.count; j++)
            {
                uacpi_namespace_node *dev = UacpipThermalResolve(arr.objects[j], Pdo->Node);
                PUACPI_PDO fan = (dev != NULL) ? UacpiFindPdoByNode(g_AcpiFdo, dev) : NULL;

                if (fan == NULL)
                {
                    UacpiTrace("[acpi] thermal: %s %s[%u] names no device PDO\n",
                               Pdo->Name, name, (ULONG)j);
                    continue;
                }
                if (!fan->DStateKnown || fan->CurrentDState != state)
                {
                    (void)UacpiPowerSetDeviceState(fan, state);
                }
            }
        }
        uacpi_object_unref(pkg);
    }
}

#if (NTDDI_VERSION >= NTDDI_WIN8)
// _DTI(NotifyTemp) re-arms the platform's trip notification once the
// temperature moved by _NTT or crossed a trip point since the last call.
static VOID
UacpipThermalCheckDti(PUACPI_PDO Pdo)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    UACPI_THERMAL_INFO *info = &tz->Info;
    ULONG cur = info->CurrentTemperature;
    ULONG nt = tz->NotifyTemp;
    ULONG delta = tz->NotifyDelta;
    ULONG trips[14];
    ULONG count = 0, i;
    BOOLEAN rearm = !tz->DtiArmed;

    if (!UacpiNodeHasChild(Pdo->Node, "_DTI"))
    {
        return;
    }

    if (!rearm && delta != 0)
    {
        rearm = (nt > delta && cur <= nt - delta) ||
                (nt <= MAXULONG - delta && cur >= nt + delta);
    }

    trips[count++] = info->PassiveTripPoint;
    trips[count++] = info->CriticalTripPoint;
    trips[count++] = info->S4TripPoint;
#if (NTDDI_VERSION >= NTDDI_WIN10)
    trips[count++] = info->StandbyTripPoint;
#endif
    for (i = 0; i < info->ActiveTripPointCount; i++)
    {
        trips[count++] = info->ActiveTripPoint[i];
    }
    for (i = 0; i < count && !rearm; i++)
    {
        ULONG t = trips[i];
        rearm = (t != 0) && ((nt < t && t <= cur) || (cur <= t && t < nt));
    }
    if (!rearm)
    {
        return;
    }

    tz->NotifyTemp = cur;
    tz->DtiArmed   = TRUE;
    UacpipThermalEvalArg(Pdo->Node, "_DTI", cur);
}
#endif

// Every reading gets a new stamp; a query with an old stamp is waiting on it.
static VOID
UacpipThermalReadTemperature(PUACPI_PDO Pdo)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    ULONG temp;
    KIRQL irql;

    KeAcquireSpinLock(&tz->Lock, &irql);
    tz->Info.ThermalStamp++;
    KeReleaseSpinLock(&tz->Lock, irql);

    if (UacpipThermalEvalInt(Pdo->Node, "_TMP", &temp))
    {
        tz->Info.CurrentTemperature = temp;
    }
#if (NTDDI_VERSION >= NTDDI_WIN8)
    UacpipThermalCheckDti(Pdo);
#endif
}

static VOID
UacpipThermalFillQuery(const UACPI_THERMAL_INFO *Info, UACPI_TZ_QUERY_INFO *Out)
{
    ULONG i;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->ThermalStamp         = Info->ThermalStamp;
    Out->ThermalConstant1     = Info->ThermalConstant1;
    Out->ThermalConstant2     = Info->ThermalConstant2;
    Out->SamplingPeriod       = Info->SamplingPeriod;
    Out->CurrentTemperature   = Info->CurrentTemperature;
    Out->PassiveTripPoint     = Info->PassiveTripPoint;
    Out->CriticalTripPoint    = Info->CriticalTripPoint;
    Out->ActiveTripPointCount = (UCHAR)Info->ActiveTripPointCount;
    for (i = 0; i < RTL_NUMBER_OF(Out->ActiveTripPoint); i++)
    {
        Out->ActiveTripPoint[i] = Info->ActiveTripPoint[i];
    }
#if (NTDDI_VERSION >= NTDDI_WIN8)
    Out->S4TransitionTripPoint = Info->S4TripPoint;
#else
    Out->Processors = Info->Processors;
#endif
#if (NTDDI_VERSION >= NTDDI_WIN10)
    Out->ThermalStandbyTripPoint = Info->StandbyTripPoint;
    Out->MinimumThrottle         = Info->MinimumThrottle;
    Out->OverThrottleThreshold   = Info->OverThrottleThreshold;
#endif
}

#if (NTDDI_VERSION >= NTDDI_WIN10)
// The description goes out in a pool buffer the kernel frees.
static VOID
UacpipThermalFillDescription(PUACPI_THERMAL_ZONE Tz, PUNICODE_STRING Out)
{
    RtlZeroMemory(Out, sizeof(*Out));
    if (Tz->Description.Buffer == NULL)
    {
        return;
    }
    Out->Buffer = (PWCH)ExAllocatePoolWithTag(NonPagedPoolNx, Tz->Description.MaximumLength,
                                              UACPI_THERMAL_DESC_TAG);
    if (Out->Buffer == NULL)
    {
        return;
    }
    RtlCopyMemory(Out->Buffer, Tz->Description.Buffer, Tz->Description.MaximumLength);
    Out->Length        = Tz->Description.Length;
    Out->MaximumLength = Tz->Description.MaximumLength;
}
#endif

static ULONG
UacpipThermalIrpCode(PIRP Irp)
{
    return IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode;
}

// Complete every queued thermal IRP with success; queries get the zone data.
static VOID
UacpipThermalCompleteIrps(PUACPI_PDO Pdo)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    UACPI_THERMAL_INFO snapshot;
    LIST_ENTRY done;
    PLIST_ENTRY e, next;
    BOOLEAN query = FALSE;
    KIRQL irql;

    InitializeListHead(&done);

    KeAcquireSpinLock(&tz->Lock, &irql);
    for (e = tz->IrpQueue.Flink; e != &tz->IrpQueue; e = next)
    {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);

        next = e->Flink;
        if (UacpipThermalIrpCode(irp) == IOCTL_THERMAL_QUERY_INFORMATION)
        {
            // A query whose cancel routine already ran belongs to it.
            if (IoSetCancelRoutine(irp, NULL) == NULL)
            {
                continue;
            }
            query = TRUE;
        }
        RemoveEntryList(e);
        InsertTailList(&done, e);
    }
    if (query)
    {
        tz->Delivered = TRUE;
    }
    snapshot = tz->Info;
    KeReleaseSpinLock(&tz->Lock, irql);

    while (!IsListEmpty(&done))
    {
        PIRP irp = CONTAINING_RECORD(RemoveHeadList(&done), IRP, Tail.Overlay.ListEntry);
        ULONG_PTR info = 0;

        switch (UacpipThermalIrpCode(irp))
        {
        case IOCTL_THERMAL_QUERY_INFORMATION:
            UacpipThermalFillQuery(&snapshot,
                                   (UACPI_TZ_QUERY_INFO *)irp->AssociatedIrp.SystemBuffer);
            info = sizeof(UACPI_TZ_QUERY_INFO);
            break;
#if (NTDDI_VERSION >= NTDDI_WIN10)
        case UACPI_IOCTL_THERMAL_DESCRIPTION:
            UacpipThermalFillDescription(tz, (PUNICODE_STRING)irp->AssociatedIrp.SystemBuffer);
            info = sizeof(UNICODE_STRING);
            break;
#endif
        default:
            break;
        }
        irp->IoStatus.Status      = STATUS_SUCCESS;
        irp->IoStatus.Information = info;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

_Function_class_(IO_WORKITEM_ROUTINE)
static VOID
NTAPI
UacpipThermalWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    PUACPI_PDO          pdo  = (PUACPI_PDO)DeviceObject->DeviceExtension;
    PUACPI_THERMAL_ZONE tz   = &pdo->Thermal;
    PIO_WORKITEM        item = (PIO_WORKITEM)Context;
    ULONG work;
    UCHAR policy, level;
    KIRQL irql;

    for (;;)
    {
        KeAcquireSpinLock(&tz->Lock, &irql);
        work     = tz->Work;
        tz->Work = 0;
        if (work == 0)
        {
            tz->Running = FALSE;
            KeReleaseSpinLock(&tz->Lock, irql);
            break;
        }
        policy = tz->CoolingPolicy;
        level  = tz->CoolingLevel;
        KeReleaseSpinLock(&tz->Lock, irql);

#if (NTDDI_VERSION >= NTDDI_WIN8)
        // Win8+: any step besides the reading itself starts a fresh reading.
        if (work & (UACPI_TZ_INIT | UACPI_TZ_TRIPS | UACPI_TZ_ACTIVE))
        {
            work |= UACPI_TZ_TEMPERATURE;
        }
#endif
        if (work & UACPI_TZ_INIT)
        {
            UacpipThermalInit(pdo);
        }
        if (work & UACPI_TZ_POLICY)
        {
            UacpipThermalSetPolicy(pdo, policy);
        }
        if (work & UACPI_TZ_TRIPS)
        {
            UacpipThermalReadTrips(pdo);
        }
        if (work & UACPI_TZ_ACTIVE)
        {
            UacpipThermalRunActive(pdo, level);
        }
        if (work & UACPI_TZ_TEMPERATURE)
        {
            UacpipThermalReadTemperature(pdo);
        }
        if (work & UACPI_TZ_COMPLETE)
        {
            UacpipThermalCompleteIrps(pdo);
        }
    }
    IoFreeWorkItem(item);
}

// Record owed work; TRUE when the caller must queue the worker. Lock held.
static BOOLEAN
UacpipThermalAddWorkLocked(PUACPI_THERMAL_ZONE Tz, ULONG Work)
{
    if (Work & UACPI_TZ_COMPLETE)
    {
        Tz->Delivered = FALSE;   // the next completion pass carries news
    }
    Tz->Work |= Work;
    if (Tz->Work == 0 || Tz->Running)
    {
        return FALSE;
    }
    Tz->Running = TRUE;
    return TRUE;
}

static VOID
UacpipThermalQueueWorker(PUACPI_PDO Pdo)
{
    PIO_WORKITEM item = IoAllocateWorkItem(Pdo->Common.Self);
    KIRQL irql;

    if (item != NULL)
    {
        IoQueueWorkItem(item, UacpipThermalWorker, DelayedWorkQueue, item);
        return;
    }
    // The work stays owed; the next request queues the worker again.
    KeAcquireSpinLock(&Pdo->Thermal.Lock, &irql);
    Pdo->Thermal.Running = FALSE;
    KeReleaseSpinLock(&Pdo->Thermal.Lock, irql);
    UacpiTrace("[acpi] thermal: %s worker allocation failed\n", Pdo->Name);
}

static VOID
UacpipThermalKick(PUACPI_PDO Pdo, ULONG Work)
{
    BOOLEAN queue = FALSE;
    KIRQL irql;

    KeAcquireSpinLock(&Pdo->Thermal.Lock, &irql);
    if (Pdo->Thermal.Active)
    {
        queue = UacpipThermalAddWorkLocked(&Pdo->Thermal, Work);
    }
    KeReleaseSpinLock(&Pdo->Thermal.Lock, irql);

    if (queue)
    {
        UacpipThermalQueueWorker(Pdo);
    }
}

// Cancel routine for a pended IOCTL_THERMAL_QUERY_INFORMATION.
static VOID
NTAPI
UacpipThermalCancelIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PUACPI_PDO Pdo = (PUACPI_PDO)DeviceObject->DeviceExtension;
    KIRQL irql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Pdo->Thermal.Lock, &irql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Pdo->Thermal.Lock, irql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

NTSTATUS
UacpiThermalDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code   = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf    = Irp->AssociatedIrp.SystemBuffer;
    ULONG needIn = 0, needOut = 0, work = 0;
    BOOLEAN queue;
    KIRQL irql;

    *Handled = FALSE;
    switch (code)
    {
    case IOCTL_THERMAL_QUERY_INFORMATION:
        needIn  = sizeof(ULONG);
        needOut = sizeof(UACPI_TZ_QUERY_INFO);
        break;
    case IOCTL_THERMAL_SET_COOLING_POLICY:
    case IOCTL_RUN_ACTIVE_COOLING_METHOD:
#if (NTDDI_VERSION >= NTDDI_WIN8)
    case IOCTL_THERMAL_SET_PASSIVE_LIMIT:
#endif
        needIn = sizeof(UCHAR);
        break;
#if (NTDDI_VERSION >= NTDDI_WIN10)
    case UACPI_IOCTL_THERMAL_DESCRIPTION:
        needOut = sizeof(UNICODE_STRING);
        break;
#endif
    default:
        return STATUS_NOT_SUPPORTED;   // not a thermal IOCTL for this release
    }
    *Handled = TRUE;

    // The kernel thermal manager is the only client.
    if (Irp->RequestorMode != KernelMode)
    {
        return UacpiCompleteIrp(Irp, STATUS_NOT_IMPLEMENTED, 0);
    }
    if (code == IOCTL_THERMAL_QUERY_INFORMATION && outLen != needOut &&
        InterlockedExchange(&UacpiThermalQuerySizeNoted, 1) == 0)
    {
        UacpiTrace("[acpi] thermal: QUERY_INFORMATION output %u bytes, this build returns %u\n",
                   outLen, needOut);
    }
    if ((buf == NULL && (needIn | needOut) != 0) || inLen < needIn || outLen < needOut)
    {
        return UacpiCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    KeAcquireSpinLock(&tz->Lock, &irql);
    if (!tz->Active)
    {
        KeReleaseSpinLock(&tz->Lock, irql);
        return UacpiCompleteIrp(Irp, STATUS_NO_SUCH_DEVICE, 0);
    }

    switch (code)
    {
    case IOCTL_THERMAL_QUERY_INFORMATION:
        // An old stamp wants a fresh reading; a current one waits for news.
        if (*(PULONG)buf != tz->Info.ThermalStamp)
        {
            work = UACPI_TZ_TEMPERATURE | UACPI_TZ_COMPLETE;
        }
        else if (!tz->Delivered)
        {
            work = UACPI_TZ_COMPLETE;
        }
        IoSetCancelRoutine(Irp, UacpipThermalCancelIrp);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
        {
            KeReleaseSpinLock(&tz->Lock, irql);
            return UacpiCompleteIrp(Irp, STATUS_CANCELLED, 0);
        }
        break;

    case IOCTL_THERMAL_SET_COOLING_POLICY:
        tz->CoolingPolicy = *(PUCHAR)buf;
        work = UACPI_TZ_POLICY | UACPI_TZ_TRIPS | UACPI_TZ_COMPLETE;
        UacpiTrace("[acpi] thermal: %s cooling policy %u\n", Pdo->Name, tz->CoolingPolicy);
        break;

    case IOCTL_RUN_ACTIVE_COOLING_METHOD:
        tz->CoolingLevel = *(PUCHAR)buf;
        work = UACPI_TZ_ACTIVE | UACPI_TZ_COMPLETE;
        UacpiTrace("[acpi] thermal: %s active cooling level %u\n", Pdo->Name,
                   tz->CoolingLevel);
        break;

#if (NTDDI_VERSION >= NTDDI_WIN8)
    case IOCTL_THERMAL_SET_PASSIVE_LIMIT:
        // Recorded only: no GUID_THERMAL_COOLING_INTERFACE consumer is driven yet.
        tz->ThrottleLimit = *(PUCHAR)buf;
        work = UACPI_TZ_TEMPERATURE | UACPI_TZ_COMPLETE;
        UacpiTrace("[acpi] thermal: %s passive limit %u%%\n", Pdo->Name, tz->ThrottleLimit);
        break;
#endif

    default:   // Win10 description
        work = UACPI_TZ_COMPLETE;
        break;
    }

    IoMarkIrpPending(Irp);
    InsertTailList(&tz->IrpQueue, &Irp->Tail.Overlay.ListEntry);
    queue = UacpipThermalAddWorkLocked(tz, work);
    KeReleaseSpinLock(&tz->Lock, irql);

    if (queue)
    {
        UacpipThermalQueueWorker(Pdo);
    }
    return STATUS_PENDING;
}

VOID
UacpiThermalNotify(PUACPI_PDO Pdo, ULONG NotifyValue)
{
    if (NotifyValue == 0x80)
    {
        UacpipThermalKick(Pdo, UACPI_TZ_TEMPERATURE | UACPI_TZ_COMPLETE);
    }
    else if (NotifyValue == 0x81)
    {
        UacpipThermalKick(Pdo, UACPI_TZ_TRIPS | UACPI_TZ_TEMPERATURE | UACPI_TZ_COMPLETE);
    }
}

// Every _ALx device starts on (level 0) until the kernel picks a level.
static VOID
UacpipThermalZoneStart(PUACPI_PDO Pdo)
{
    PUACPI_THERMAL_ZONE tz = &Pdo->Thermal;
    BOOLEAN queue = FALSE;
    KIRQL irql;

    KeAcquireSpinLock(&tz->Lock, &irql);
    if (!tz->Active)
    {
        tz->Active        = TRUE;
        tz->CoolingPolicy = UACPI_COOLING_PASSIVE;
        tz->CoolingLevel  = 0;
        tz->ThrottleLimit = 100;
        queue = UacpipThermalAddWorkLocked(tz, UACPI_TZ_INIT | UACPI_TZ_POLICY |
                                               UACPI_TZ_TRIPS | UACPI_TZ_ACTIVE |
                                               UACPI_TZ_TEMPERATURE | UACPI_TZ_COMPLETE);
    }
    KeReleaseSpinLock(&tz->Lock, irql);

    if (queue)
    {
        UacpipThermalQueueWorker(Pdo);
    }
}

// Stop taking IOCTLs and drain the queue with success, as a stopped zone does.
static VOID
UacpipThermalZoneStop(PUACPI_PDO Pdo)
{
    KIRQL irql;

    KeAcquireSpinLock(&Pdo->Thermal.Lock, &irql);
    Pdo->Thermal.Active = FALSE;
    Pdo->Thermal.Work   = 0;
    KeReleaseSpinLock(&Pdo->Thermal.Lock, irql);

    UacpipThermalCompleteIrps(Pdo);
}

// WmiLib callbacks
static NTSTATUS
NTAPI
UacpiThermalQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags,
                           PUNICODE_STRING InstanceName, PUNICODE_STRING *RegistryPath,
                           PUNICODE_STRING MofResourceName, PDEVICE_OBJECT *Pdo)
{
    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);
    // WmiLib does not pre-init RegistryPath; set a persistent string or NULL.
    *RegistryPath = (UacpiDriverRegistryPath.Buffer != NULL)
                    ? &UacpiDriverRegistryPath : NULL;
    // Derive the instance name from the PDO's device instance path.
    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *Pdo      = DeviceObject;
    return STATUS_SUCCESS;
}

static NTSTATUS
NTAPI
UacpiThermalQueryWmiDataBlock(PDEVICE_OBJECT DeviceObject, PIRP Irp, ULONG GuidIndex,
                             ULONG InstanceIndex, ULONG InstanceCount,
                             PULONG InstanceLengthArray, ULONG BufferAvail,
                             PUCHAR Buffer)
{
    PUACPI_PDO Pdo = (PUACPI_PDO)DeviceObject->DeviceExtension;
    UACPI_THERMAL_WMI *tz;
    uacpi_namespace_node *node = Pdo->Node;
    ULONG i, active = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(InstanceCount);

    if (GuidIndex != 0)
    {
        return WmiCompleteRequest(DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0,
                                  IO_NO_INCREMENT);
    }
    if (BufferAvail < sizeof(UACPI_THERMAL_WMI) || InstanceLengthArray == NULL)
    {
        return WmiCompleteRequest(DeviceObject, Irp, STATUS_BUFFER_TOO_SMALL, 0,
                                  IO_NO_INCREMENT);
    }

    tz = (UACPI_THERMAL_WMI *)Buffer;
    RtlZeroMemory(tz, sizeof(*tz));
    tz->ThermalStamp       = InterlockedIncrement((PLONG)&UacpiThermalStamp);
    tz->ThermalConstant1   = UacpiThermalEvalOr(node, "_TC1", 0);
    tz->ThermalConstant2   = UacpiThermalEvalOr(node, "_TC2", 0);
    tz->SamplingPeriod     = UacpiThermalEvalOr(node, "_TSP", 0);
    tz->CurrentTemperature = UacpiThermalEvalOr(node, "_TMP", 2732);   // ~0C fallback
    tz->PassiveTripPoint   = UacpiThermalEvalOr(node, "_PSV", 0);
    tz->CriticalTripPoint  = UacpiThermalEvalOr(node, "_CRT", 0);
    for (i = 0; i < 10; i++)
    {
        char name[5] = { '_', 'A', 'C', (char)('0' + i), 0 };
        if (!UacpipThermalEvalInt(node, name, &tz->ActiveTripPoint[i]))
        {
            break;
        }
        active = i + 1;   // _ACx are contiguous from _AC0
    }
    tz->ActiveTripPointCount = active;

    InstanceLengthArray[0] = sizeof(UACPI_THERMAL_WMI);
    status = STATUS_SUCCESS;
    return WmiCompleteRequest(DeviceObject, Irp, status, sizeof(UACPI_THERMAL_WMI),
                              IO_NO_INCREMENT);
}

// IRP_MJ_SYSTEM_CONTROL (WMI) dispatch for a thermal-zone PDO.
NTSTATUS
UacpiThermalSystemControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled)
{
    SYSCTL_IRP_DISPOSITION disposition;
    NTSTATUS status;

    *Handled = FALSE;
    if (Pdo->ThermalWmi == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }
    *Handled = TRUE;
    status = WmiSystemControl((PWMILIB_CONTEXT)Pdo->ThermalWmi, Pdo->Common.Self,
                              Irp, &disposition);
    switch (disposition)
    {
    case IrpProcessed:
        break;                      // WmiSystemControl completed the IRP
    case IrpNotCompleted:
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    default:                        // IrpForward / IrpNotWmi; a PDO is the bottom
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }
    return status;
}

static VOID
UacpipThermalWmiRegister(PUACPI_PDO Pdo)
{
    PWMILIB_CONTEXT wmi;

    if (Pdo->ThermalWmi != NULL)
    {
        return;   // already registered
    }
    wmi = (PWMILIB_CONTEXT)ExAllocatePoolWithTag(NonPagedPool, sizeof(*wmi),
                                                 UACPI_POOL_TAG);
    if (wmi == NULL)
    {
        return;
    }
    RtlZeroMemory(wmi, sizeof(*wmi));
    wmi->GuidCount          = RTL_NUMBER_OF(UacpiThermalWmiGuidList);
    wmi->GuidList           = UacpiThermalWmiGuidList;
    wmi->QueryWmiRegInfo    = UacpiThermalQueryWmiRegInfo;
    wmi->QueryWmiDataBlock  = UacpiThermalQueryWmiDataBlock;
    Pdo->ThermalWmi = wmi;

    (void)IoWMIRegistrationControl(Pdo->Common.Self, WMIREG_ACTION_REGISTER);
    UacpiTrace("[acpi] thermal: %s WMI temperature provider up\n", Pdo->Name);
}

VOID
UacpiThermalStart(PUACPI_PDO Pdo)
{
    if (!Pdo->IsThermalZone)
    {
        return;   // not a thermal zone
    }
    UacpiDevIfRegister(Pdo, &GUID_DEVICE_THERMAL_ZONE, "thermal");
    UacpipThermalWmiRegister(Pdo);
    UacpipThermalZoneStart(Pdo);
}

VOID
UacpiThermalRemove(PUACPI_PDO Pdo)
{
    if (!Pdo->IsThermalZone)
    {
        return;
    }
    UacpipThermalZoneStop(Pdo);

    if (Pdo->ThermalWmi != NULL)
    {
        (void)IoWMIRegistrationControl(Pdo->Common.Self, WMIREG_ACTION_DEREGISTER);
        ExFreePool(Pdo->ThermalWmi);
        Pdo->ThermalWmi = NULL;
    }
}
