/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Small C++ helpers shared by every part of the driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Pool tags, as they show up in a pool dump */
#define USB4DR_TAG_DRIVER       'dD4U'
#define USB4DR_TAG_HOST         'hD4U'
#define USB4DR_TAG_ROUTER       'rD4U'
#define USB4DR_TAG_DROM         'mD4U'
#define USB4DR_TAG_PORT         'pD4U'
#define USB4DR_TAG_TUNNEL       'nD4U'
#define USB4DR_TAG_CHILD        'cD4U'

/* Objects live in WDF context memory; this builds them in place */
inline
PVOID
operator new(
    _In_ size_t Size,
    _In_ PVOID Where) noexcept
{
    UNREFERENCED_PARAMETER(Size);
    return Where;
}

inline
VOID
operator delete(
    _In_ PVOID Memory,
    _In_ PVOID Where) noexcept
{
    UNREFERENCED_PARAMETER(Memory);
    UNREFERENCED_PARAMETER(Where);
}

/** Holds a spin lock for the lifetime of the guard. */
class Usb4DrSpinLockGuard
{
public:
    explicit
    Usb4DrSpinLockGuard(
        _Inout_ PKSPIN_LOCK Lock)
        : m_Lock(Lock)
    {
        KeAcquireSpinLock(m_Lock, &m_OldIrql);
    }

    ~Usb4DrSpinLockGuard()
    {
        KeReleaseSpinLock(m_Lock, m_OldIrql);
    }

    Usb4DrSpinLockGuard(const Usb4DrSpinLockGuard&) = delete;
    Usb4DrSpinLockGuard& operator=(const Usb4DrSpinLockGuard&) = delete;

private:
    PKSPIN_LOCK m_Lock;
    KIRQL m_OldIrql;
};

/** Holds a WDF spin lock for the lifetime of the guard. */
class Usb4DrWdfLockGuard
{
public:
    explicit
    Usb4DrWdfLockGuard(
        _In_ WDFSPINLOCK Lock)
        : m_Lock(Lock)
    {
        WdfSpinLockAcquire(m_Lock);
    }

    ~Usb4DrWdfLockGuard()
    {
        WdfSpinLockRelease(m_Lock);
    }

    Usb4DrWdfLockGuard(const Usb4DrWdfLockGuard&) = delete;
    Usb4DrWdfLockGuard& operator=(const Usb4DrWdfLockGuard&) = delete;

private:
    WDFSPINLOCK m_Lock;
};

/** Holds a WDF wait lock for the lifetime of the guard. PASSIVE_LEVEL only. */
class Usb4DrWaitLockGuard
{
public:
    explicit
    Usb4DrWaitLockGuard(
        _In_ WDFWAITLOCK Lock)
        : m_Lock(Lock)
    {
        WdfWaitLockAcquire(m_Lock, NULL);
    }

    ~Usb4DrWaitLockGuard()
    {
        WdfWaitLockRelease(m_Lock);
    }

    Usb4DrWaitLockGuard(const Usb4DrWaitLockGuard&) = delete;
    Usb4DrWaitLockGuard& operator=(const Usb4DrWaitLockGuard&) = delete;

private:
    WDFWAITLOCK m_Lock;
};

/* A LIST_ENTRY with NULL links means "not on any list" throughout this driver */
FORCEINLINE
VOID
NTAPI
Usb4DrClearListEntry(
    _Out_ PLIST_ENTRY Entry)
{
    Entry->Flink = NULL;
    Entry->Blink = NULL;
}

FORCEINLINE
BOOLEAN
NTAPI
Usb4DrIsListEntryLinked(
    _In_ const LIST_ENTRY* Entry)
{
    return Entry->Flink != NULL;
}

/** Milliseconds to a relative KeSetTimer/KeWaitForSingleObject interval. */
FORCEINLINE
LARGE_INTEGER
NTAPI
Usb4DrRelativeMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    return Interval;
}

/** Milliseconds to a relative WDF_REQUEST_SEND_OPTIONS timeout. */
FORCEINLINE
LONGLONG
NTAPI
Usb4DrWdfTimeoutMs(
    _In_ ULONG Milliseconds)
{
    return WDF_REL_TIMEOUT_IN_MS(Milliseconds);
}

/** Sleeps for Milliseconds. PASSIVE_LEVEL only. */
FORCEINLINE
VOID
NTAPI
Usb4DrSleepMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval = Usb4DrRelativeMs(Milliseconds);

    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/** Extracts the field selected by Mask, shifted down to bit 0. */
FORCEINLINE
ULONG
NTAPI
Usb4DrField(
    _In_ ULONG Value,
    _In_ ULONG Mask)
{
    ULONG Shift = 0;

    if (Mask == 0)
        return 0;

    while (!(Mask & (1UL << Shift)))
        Shift++;

    return (Value & Mask) >> Shift;
}
