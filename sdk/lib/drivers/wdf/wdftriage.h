/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Missing headers (wdftriage.h)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Field offsets the framework publishes so a debugger can walk its objects in
 * a crash dump. Layouts follow the published UMDF 2.33 header, which the
 * kernel framework shares.
 */

#ifndef _WDFTRIAGE_H_
#define _WDFTRIAGE_H_

#define WDF_01_TRIAGE_INFO_MAJOR_VERSION    1
#define WDF_01_TRIAGE_INFO_MINOR_VERSION    0

typedef struct _WDFOBJECT_TRIAGE_INFO {
    ULONG RawObjectSize;
    ULONG ObjectType;
    ULONG TotalObjectSize;
    ULONG ChildListHead;
    ULONG ChildEntry;
    ULONG Globals;
    ULONG ParentObject;
} WDFOBJECT_TRIAGE_INFO, *PWDFOBJECT_TRIAGE_INFO;

typedef struct _WDFCONTEXT_TRIAGE_INFO {
    ULONG HeaderSize;
    ULONG NextHeader;
    ULONG Object;
    ULONG TypeInfoPtr;
    ULONG Context;
} WDFCONTEXT_TRIAGE_INFO, *PWDFCONTEXT_TRIAGE_INFO;

typedef struct _WDFCONTEXTTYPE_TRIAGE_INFO {
    ULONG TypeInfoSize;
    ULONG ContextSize;
    ULONG ContextName;
} WDFCONTEXTTYPE_TRIAGE_INFO, *PWDFCONTEXTTYPE_TRIAGE_INFO;

typedef struct _WDFQUEUE_TRIAGE_INFO {
    ULONG QueueSize;
    ULONG IrpQueue1;
    ULONG IrpQueue2;
    ULONG RequestList1;
    ULONG RequestList2;
    ULONG FwdProgressContext;
    ULONG PkgIo;
} WDFQUEUE_TRIAGE_INFO, *PWDFQUEUE_TRIAGE_INFO;

typedef struct _WDFFWDPROGRESS_TRIAGE_INFO {
    ULONG ReservedRequestList;
    ULONG ReservedRequestInUseList;
    ULONG PendedIrpList;
} WDFFWDPROGRESS_TRIAGE_INFO, *PWDFFWDPROGRESS_TRIAGE_INFO;

typedef struct _WDFIRPQUEUE_TRIAGE_INFO {
    ULONG IrpQueueSize;
    ULONG IrpListHeader;
    ULONG IrpListEntry;
    ULONG IrpContext;
} WDFIRPQUEUE_TRIAGE_INFO, *PWDFIRPQUEUE_TRIAGE_INFO;

typedef struct _WDFREQUEST_TRIAGE_INFO {
    ULONG RequestSize;
    ULONG CsqContext;
    ULONG FxIrp;
    ULONG ListEntryQueueOwned;
    ULONG ListEntryQueueOwned2;
    ULONG RequestListEntry;
    ULONG FwdProgressList;
} WDFREQUEST_TRIAGE_INFO, *PWDFREQUEST_TRIAGE_INFO;

typedef struct _WDFDEVICE_TRIAGE_INFO {
    ULONG DeviceInitSize;
    ULONG DeviceDriver;
} WDFDEVICE_TRIAGE_INFO, *PWDFDEVICE_TRIAGE_INFO;

typedef struct _WDFIRP_TRIAGE_INFO {
    ULONG FxIrpSize;
    ULONG IrpPtr;
} WDFIRP_TRIAGE_INFO, *PWDFIRP_TRIAGE_INFO;

typedef struct _WDF_TRIAGE_INFO {
    ULONG WdfMajorVersion;
    ULONG WdfMinorVersion;
    ULONG TriageInfoMajorVersion;
    ULONG TriageInfoMinorVersion;
    PVOID Reserved;
    PWDFOBJECT_TRIAGE_INFO WdfObjectTriageInfo;
    PWDFCONTEXT_TRIAGE_INFO WdfContextTriageInfo;
    PWDFCONTEXTTYPE_TRIAGE_INFO WdfContextTypeTriageInfo;
    PWDFQUEUE_TRIAGE_INFO WdfQueueTriageInfo;
    PWDFFWDPROGRESS_TRIAGE_INFO WdfFwdProgressTriageInfo;
    PWDFIRPQUEUE_TRIAGE_INFO WdfIrpQueueTriageInfo;
    PWDFREQUEST_TRIAGE_INFO WdfRequestTriageInfo;
    PWDFDEVICE_TRIAGE_INFO WdfDeviceTriageInfo;
    PWDFIRP_TRIAGE_INFO WdfIrpTriageInfo;
} WDF_TRIAGE_INFO, *PWDF_TRIAGE_INFO;

#endif /* _WDFTRIAGE_H_ */
