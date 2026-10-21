/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Routines the Windows 11 dxgkrnl looks up by name in the win32k image
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>

#define NDEBUG
#include <debug.h>

/*
 * dxgkrnl builds its table of win32k routines from these exports when win32k queries its
 * interface. Any name it does not find falls back to a stub of its own; the ones here are
 * those composition surfaces, flip tokens and the DWM checks cannot do without.
 */

#define TAG_DXGK_IMPORT     'iGxD'

/* Region combine modes, the GDI ones */
#define DL_REGION_AND       1
#define DL_REGION_OR        2
#define DL_REGION_XOR       3
#define DL_REGION_DIFF      4
#define DL_REGION_COPY      5

/* Region kinds GetRegionData reports */
#define DL_REGION_EMPTY     0
#define DL_REGION_FULL      1
#define DL_REGION_RECTS     2

/* Past this many rectangles a region keeps only its bounds; a dirty region may grow, never shrink */
#define DL_REGION_MAX_RECTS 32

typedef struct _DL_REGION DL_REGION, *PDL_REGION;

/* The interface dxgkrnl calls regions through, in its slot order; there is no AddRef */
typedef struct _DL_REGION_VTBL
{
    ULONG (NTAPI *Release)(_In_ PDL_REGION Region);
    BOOLEAN (NTAPI *IsEmpty)(_In_ PDL_REGION Region);
    VOID (NTAPI *SetEmpty)(_In_ PDL_REGION Region);
    BOOLEAN (NTAPI *IsFullRegion)(_In_ PDL_REGION Region);
    VOID (NTAPI *SetFullRegion)(_In_ PDL_REGION Region);
    NTSTATUS (NTAPI *AddRect)(_In_ PDL_REGION Region, _In_ const RECT* Rect);
    NTSTATUS (NTAPI *Combine)(_In_ PDL_REGION Region, _In_ PDL_REGION Source, _In_ ULONG Mode);
    NTSTATUS (NTAPI *GetRegionData)(_In_ PDL_REGION Region, _Inout_ PULONG Count, _Out_writes_opt_(*Count) RECT* Rects, _Out_ PULONG Kind);
    VOID (NTAPI *GetBoundingRect)(_In_ PDL_REGION Region, _Out_ RECT* Bounds);
} DL_REGION_VTBL;

struct _DL_REGION
{
    const DL_REGION_VTBL* Vtbl;
    LONG RefCount;
    ULONG Kind;
    ULONG Count;
    RECT Rects[DL_REGION_MAX_RECTS];
};

static ERESOURCE gDwmStateLock;

/* GDI system memory present tokens would be queued behind this event; none are yet */
static PKEVENT gpSfmTokenEvent = NULL;

/* Set once the DWM runs a token thread, so flip presents have someone to retire them */
static BOOLEAN gbDwmTokenThread = FALSE;

static
BOOLEAN
DlpIsValidRect(
    _In_ const RECT* Rect)
{
    return Rect->left < Rect->right && Rect->top < Rect->bottom;
}

static
BOOLEAN
DlpRectContains(
    _In_ const RECT* Outer,
    _In_ const RECT* Inner)
{
    return Outer->left <= Inner->left && Outer->top <= Inner->top &&
           Outer->right >= Inner->right && Outer->bottom >= Inner->bottom;
}

static
VOID
DlpRegionBounds(
    _In_ PDL_REGION Region,
    _Out_ RECT* Bounds)
{
    ULONG i;

    *Bounds = Region->Rects[0];
    for (i = 1; i < Region->Count; i++)
    {
        Bounds->left = min(Bounds->left, Region->Rects[i].left);
        Bounds->top = min(Bounds->top, Region->Rects[i].top);
        Bounds->right = max(Bounds->right, Region->Rects[i].right);
        Bounds->bottom = max(Bounds->bottom, Region->Rects[i].bottom);
    }
}

/* Adds a valid rectangle to a region that is not full */
static
VOID
DlpRegionUnion(
    _Inout_ PDL_REGION Region,
    _In_ const RECT* Rect)
{
    ULONG i, Kept;

    for (i = 0; i < Region->Count; i++)
    {
        if (DlpRectContains(&Region->Rects[i], Rect))
            return;
    }

    /* Rectangles the new one covers go away */
    for (i = 0, Kept = 0; i < Region->Count; i++)
    {
        if (!DlpRectContains(Rect, &Region->Rects[i]))
            Region->Rects[Kept++] = Region->Rects[i];
    }
    Region->Count = Kept;

    if (Region->Count == DL_REGION_MAX_RECTS)
    {
        DlpRegionBounds(Region, &Region->Rects[0]);
        Region->Count = 1;
        DlpRegionUnion(Region, Rect);
        return;
    }

    Region->Rects[Region->Count++] = *Rect;
    Region->Kind = DL_REGION_RECTS;
}

static
ULONG
NTAPI
DlpRegionRelease(
    _In_ PDL_REGION Region)
{
    LONG Count = InterlockedDecrement(&Region->RefCount);

    if (!Count)
        ExFreePoolWithTag(Region, TAG_DXGK_IMPORT);
    return (ULONG)Count;
}

static
BOOLEAN
NTAPI
DlpRegionIsEmpty(
    _In_ PDL_REGION Region)
{
    return Region->Kind == DL_REGION_EMPTY || (Region->Kind == DL_REGION_RECTS && !Region->Count);
}

static
VOID
NTAPI
DlpRegionSetEmpty(
    _In_ PDL_REGION Region)
{
    Region->Kind = DL_REGION_EMPTY;
    Region->Count = 0;
}

static
BOOLEAN
NTAPI
DlpRegionIsFull(
    _In_ PDL_REGION Region)
{
    return Region->Kind == DL_REGION_FULL;
}

static
VOID
NTAPI
DlpRegionSetFull(
    _In_ PDL_REGION Region)
{
    Region->Kind = DL_REGION_FULL;
    Region->Count = 0;
}

static
NTSTATUS
NTAPI
DlpRegionAddRect(
    _In_ PDL_REGION Region,
    _In_ const RECT* Rect)
{
    if (!Rect || !DlpIsValidRect(Rect))
        return STATUS_INVALID_PARAMETER;

    if (Region->Kind != DL_REGION_FULL)
        DlpRegionUnion(Region, Rect);
    return STATUS_SUCCESS;
}

/* dxgkrnl only ORs regions together; the other modes keep at least the area they should */
static
NTSTATUS
NTAPI
DlpRegionCombine(
    _In_ PDL_REGION Region,
    _In_ PDL_REGION Source,
    _In_ ULONG Mode)
{
    ULONG i;

    if (!Source || Mode < DL_REGION_AND || Mode > DL_REGION_COPY)
        return STATUS_INVALID_PARAMETER;

    switch (Mode)
    {
        case DL_REGION_COPY:
            if (Source != Region)
                RtlCopyMemory(&Region->Kind, &Source->Kind, sizeof(*Region) - FIELD_OFFSET(DL_REGION, Kind));
            break;

        case DL_REGION_OR:
        case DL_REGION_XOR:
            if (Source == Region || Region->Kind == DL_REGION_FULL)
                break;
            if (Source->Kind == DL_REGION_FULL)
            {
                DlpRegionSetFull(Region);
                break;
            }
            for (i = 0; i < Source->Count; i++)
                DlpRegionUnion(Region, &Source->Rects[i]);
            break;

        case DL_REGION_AND:
            if (DlpRegionIsEmpty(Source))
                DlpRegionSetEmpty(Region);
            break;

        default:
            break;
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
DlpRegionGetData(
    _In_ PDL_REGION Region,
    _Inout_ PULONG Count,
    _Out_writes_opt_(*Count) RECT* Rects,
    _Out_ PULONG Kind)
{
    ULONG Needed = Region->Kind == DL_REGION_RECTS ? Region->Count : 0;

    *Kind = Region->Kind;
    if (Needed > *Count || (Needed && !Rects))
    {
        *Count = Needed;
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Needed)
        RtlCopyMemory(Rects, Region->Rects, Needed * sizeof(*Rects));
    *Count = Needed;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
DlpRegionGetBounds(
    _In_ PDL_REGION Region,
    _Out_ RECT* Bounds)
{
    if (Region->Kind == DL_REGION_FULL)
    {
        Bounds->left = Bounds->top = MINLONG;
        Bounds->right = Bounds->bottom = MAXLONG;
    }
    else if (Region->Kind == DL_REGION_RECTS && Region->Count)
    {
        DlpRegionBounds(Region, Bounds);
    }
    else
    {
        RtlZeroMemory(Bounds, sizeof(*Bounds));
    }
}

static const DL_REGION_VTBL DlpRegionVtbl =
{
    DlpRegionRelease,
    DlpRegionIsEmpty,
    DlpRegionSetEmpty,
    DlpRegionIsFull,
    DlpRegionSetFull,
    DlpRegionAddRect,
    DlpRegionCombine,
    DlpRegionGetData,
    DlpRegionGetBounds
};

/**
 * @brief Creates an empty region with one reference.
 */
NTSTATUS
NTAPI
CreateRegion(
    _Out_ PVOID* Region)
{
    PDL_REGION Created;

    *Region = NULL;
    Created = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Created), TAG_DXGK_IMPORT);
    if (!Created)
        return STATUS_NO_MEMORY;

    RtlZeroMemory(Created, sizeof(*Created));
    Created->Vtbl = &DlpRegionVtbl;
    Created->RefCount = 1;
    Created->Kind = DL_REGION_EMPTY;
    *Region = Created;
    return STATUS_SUCCESS;
}

/**
 * @brief Creates a region holding one rectangle.
 */
NTSTATUS
NTAPI
CreateRegionFromRect(
    _In_ const RECT* Rect,
    _Out_ PVOID* Region)
{
    NTSTATUS Status = CreateRegion(Region);

    if (NT_SUCCESS(Status) && Rect && DlpIsValidRect(Rect))
        DlpRegionUnion(*Region, Rect);
    return Status;
}

/**
 * @brief Frees pool win32k handed to dxgkrnl, such as a composition security descriptor.
 */
VOID
NTAPI
Win32FreePool(
    _In_ PVOID Pool)
{
    if (Pool)
        ExFreePool(Pool);
}

/**
 * @brief Builds the security descriptor composition objects are created with.
 *
 * SYSTEM gets all access, the Window Manager group (the DWM) read and write, and the caller's
 * user CallerAccess. The whole descriptor is one allocation for Win32FreePool.
 */
NTSTATUS
NTAPI
UserAllocDefaultCompositionSecurityDescriptor(
    _In_ ACCESS_MASK CallerAccess,
    _Out_ PSECURITY_DESCRIPTOR* Descriptor)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    SECURITY_SUBJECT_CONTEXT Subject;
    PTOKEN_USER User = NULL;
    UCHAR WindowManagerBuffer[FIELD_OFFSET(SID, SubAuthority) + 2 * sizeof(ULONG)];
    PSID WindowManager = (PSID)WindowManagerBuffer;
    PSECURITY_DESCRIPTOR Created;
    PACL Dacl;
    ULONG AclSize;
    NTSTATUS Status;

    *Descriptor = NULL;

    /* S-1-5-90-0, the group the DWM runs in */
    RtlInitializeSid(WindowManager, &NtAuthority, 2);
    *RtlSubAuthoritySid(WindowManager, 0) = 90;
    *RtlSubAuthoritySid(WindowManager, 1) = 0;

    if (CallerAccess)
    {
        SeCaptureSubjectContext(&Subject);
        Status = SeQueryInformationToken(SeQuerySubjectContextToken(&Subject), TokenUser, (PVOID*)&User);
        SeReleaseSubjectContext(&Subject);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    AclSize = sizeof(ACL) +
              FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(SeExports->SeLocalSystemSid) +
              FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(WindowManager);
    if (User)
        AclSize += FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + RtlLengthSid(User->User.Sid);

    Created = ExAllocatePoolWithTag(PagedPool, sizeof(SECURITY_DESCRIPTOR) + AclSize, TAG_DXGK_IMPORT);
    if (!Created)
    {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    Dacl = (PACL)((PUCHAR)Created + sizeof(SECURITY_DESCRIPTOR));
    Status = RtlCreateSecurityDescriptor(Created, SECURITY_DESCRIPTOR_REVISION);
    if (NT_SUCCESS(Status))
        Status = RtlCreateAcl(Dacl, AclSize, ACL_REVISION);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, GENERIC_ALL, SeExports->SeLocalSystemSid);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, 0x3, WindowManager);
    if (NT_SUCCESS(Status) && User)
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, CallerAccess, User->User.Sid);
    if (NT_SUCCESS(Status))
        Status = RtlSetDaclSecurityDescriptor(Created, TRUE, Dacl, FALSE);

    if (NT_SUCCESS(Status))
        *Descriptor = Created;
    else
        ExFreePoolWithTag(Created, TAG_DXGK_IMPORT);

Exit:
    if (User)
        ExFreePool(User);
    return Status;
}

/**
 * @brief The registered DWM's process with a reference for UserDereferenceDwmProcess, or NULL.
 */
PEPROCESS
NTAPI
UserReferenceDwmProcess(VOID)
{
    return IntDwmReferenceProcess();
}

VOID
NTAPI
UserDereferenceDwmProcess(
    _In_opt_ PEPROCESS Process)
{
    if (Process)
        ObDereferenceObject(Process);
}

BOOL
NTAPI
UserUnsafeIsCurrentProcessDwm(VOID)
{
    return IntDwmIsProcess(PsGetCurrentProcess());
}

BOOL
NTAPI
UserUnsafeIsProcessDwm(
    _In_opt_ PEPROCESS Process)
{
    return Process && IntDwmIsProcess(Process);
}

/* The console session is never disconnected */
BOOL
NTAPI
UserIsDisconnectConnection(VOID)
{
    return FALSE;
}

/* Composed means the DWM shows what is presented to composition surfaces, so it needs its token thread */
static
BOOLEAN
DlpIsDesktopComposed(VOID)
{
    return IntDwmIsActive() && gbDwmTokenThread;
}

BOOL
NTAPI
UserIsCurrentThreadDesktopComposed(VOID)
{
    return DlpIsDesktopComposed();
}

/**
 * @brief Tells whether a window exists and whether its desktop is composed.
 */
BOOL
NTAPI
UserIsWindowDesktopComposed(
    _In_ HWND hWnd,
    _Out_ PBOOL Composed)
{
    PWND Window;

    *Composed = FALSE;
    UserEnterShared();
    Window = UserGetWindowObject(hWnd);
    if (Window)
        *Composed = DlpIsDesktopComposed();
    UserLeave();
    return Window != NULL;
}

VOID
NTAPI
UserEnterUserCritSecShared(VOID)
{
    UserEnterShared();
}

VOID
NTAPI
UserLeaveUserCritSec(VOID)
{
    UserLeave();
}

BOOL
NTAPI
UserIsUserCritSecIn(VOID)
{
    return UserIsEntered();
}

VOID
NTAPI
GreLockDwmState(VOID)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&gDwmStateLock, TRUE);
}

VOID
NTAPI
GreUnlockDwmState(VOID)
{
    ExReleaseResourceLite(&gDwmStateLock);
    KeLeaveCriticalRegion();
}

BOOL
NTAPI
GreIsDwmStateLocked(VOID)
{
    return ExIsResourceAcquiredSharedLite(&gDwmStateLock) != 0;
}

/**
 * @brief Takes the DWM state lock unless the caller already holds it or the user lock, and
 *        references the DWM's process. DCompositionReleaseDwmStateLock undoes both.
 */
VOID
NTAPI
DCompositionAcquireDwmStateLock(
    _Out_ PEPROCESS* Process,
    _Out_ PBOOLEAN Taken)
{
    *Taken = FALSE;
    if (!UserIsEntered() && !GreIsDwmStateLocked())
    {
        GreLockDwmState();
        *Taken = TRUE;
    }
    *Process = IntDwmReferenceProcess();
}

VOID
NTAPI
DCompositionReleaseDwmStateLock(
    _In_opt_ PEPROCESS Process,
    _In_ BOOLEAN Taken)
{
    if (Process)
        ObDereferenceObject(Process);
    if (Taken)
        GreUnlockDwmState();
}

/**
 * @brief Opens the event GDI system memory present tokens are announced on, into the DWM.
 */
NTSTATUS
NTAPI
GreSfmOpenTokenEvent(
    _Out_ PHANDLE Event)
{
    if (!IntDwmIsProcess(PsGetCurrentProcess()))
        return STATUS_ACCESS_DENIED;
    if (!gpSfmTokenEvent)
        return STATUS_NOT_SUPPORTED;

    return ObOpenObjectByPointer(gpSfmTokenEvent, 0, NULL, SYNCHRONIZE, *ExEventObjectType, KernelMode, Event);
}

/**
 * @brief Hands the DWM queued GDI system memory present tokens. ReactOS redirects GDI through
 *        sprites instead, so the queue stays empty and its event is never signalled.
 */
NTSTATUS
NTAPI
GreSfmGetNotificationTokens(
    _In_ ULONG BufferSize,
    _Out_ PULONG Count,
    _Out_writes_bytes_opt_(BufferSize) PVOID Buffer)
{
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(Buffer);

    if (!IntDwmIsProcess(PsGetCurrentProcess()))
        return STATUS_ACCESS_DENIED;

    *Count = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief Records that the DWM entered its token thread, from the NtTokenManagerThread forwarder.
 */
VOID
NTAPI
DlNoteTokenThread(VOID)
{
    if (IntDwmIsProcess(PsGetCurrentProcess()))
        gbDwmTokenThread = TRUE;
}

/**
 * @brief Forgets the token thread of a DWM that stopped.
 */
VOID
NTAPI
DlForgetTokenThread(VOID)
{
    gbDwmTokenThread = FALSE;
}

/**
 * @brief Sets up what the exports above need, before dxgkrnl first looks them up.
 */
NTSTATUS
NTAPI
DlInitializeDxgkImports(VOID)
{
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Handle;
    NTSTATUS Status;

    Status = ExInitializeResourceLite(&gDwmStateLock);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateEvent(&Handle, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(Handle, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode,
                                       (PVOID*)&gpSfmTokenEvent, NULL);
    ZwClose(Handle);
    return Status;
}

/* EOF */
