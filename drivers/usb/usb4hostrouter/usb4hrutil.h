/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Small C++ helpers shared by every part of the driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Pool tags, as they show up in a pool dump */
#define USB4HR_TAG_DRIVER       'dR4U'
#define USB4HR_TAG_HARDWARE     'hR4U'
#define USB4HR_TAG_RING         'rR4U'
#define USB4HR_TAG_CONFIG       'cR4U'
#define USB4HR_TAG_TOPOLOGY     'tR4U'
#define USB4HR_TAG_TUNNEL       'nR4U'
#define USB4HR_TAG_CHILD        'pR4U'

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
class Usb4HrSpinLockGuard
{
public:
    explicit
    Usb4HrSpinLockGuard(
        _Inout_ PKSPIN_LOCK Lock)
        : m_Lock(Lock)
    {
        KeAcquireSpinLock(m_Lock, &m_OldIrql);
    }

    ~Usb4HrSpinLockGuard()
    {
        KeReleaseSpinLock(m_Lock, m_OldIrql);
    }

    Usb4HrSpinLockGuard(const Usb4HrSpinLockGuard&) = delete;
    Usb4HrSpinLockGuard& operator=(const Usb4HrSpinLockGuard&) = delete;

private:
    PKSPIN_LOCK m_Lock;
    KIRQL m_OldIrql;
};

/** Holds a WDF spin lock for the lifetime of the guard. */
class Usb4HrWdfLockGuard
{
public:
    explicit
    Usb4HrWdfLockGuard(
        _In_ WDFSPINLOCK Lock)
        : m_Lock(Lock)
    {
        WdfSpinLockAcquire(m_Lock);
    }

    ~Usb4HrWdfLockGuard()
    {
        WdfSpinLockRelease(m_Lock);
    }

    Usb4HrWdfLockGuard(const Usb4HrWdfLockGuard&) = delete;
    Usb4HrWdfLockGuard& operator=(const Usb4HrWdfLockGuard&) = delete;

private:
    WDFSPINLOCK m_Lock;
};

/** Holds a WDF wait lock for the lifetime of the guard. PASSIVE_LEVEL only. */
class Usb4HrWaitLockGuard
{
public:
    explicit
    Usb4HrWaitLockGuard(
        _In_ WDFWAITLOCK Lock)
        : m_Lock(Lock)
    {
        WdfWaitLockAcquire(m_Lock, NULL);
    }

    ~Usb4HrWaitLockGuard()
    {
        WdfWaitLockRelease(m_Lock);
    }

    Usb4HrWaitLockGuard(const Usb4HrWaitLockGuard&) = delete;
    Usb4HrWaitLockGuard& operator=(const Usb4HrWaitLockGuard&) = delete;

private:
    WDFWAITLOCK m_Lock;
};

/* A LIST_ENTRY with NULL links means "not on any list" throughout this driver */
FORCEINLINE
VOID
NTAPI
Usb4HrClearListEntry(
    _Out_ PLIST_ENTRY Entry)
{
    Entry->Flink = NULL;
    Entry->Blink = NULL;
}

FORCEINLINE
BOOLEAN
NTAPI
Usb4HrIsListEntryLinked(
    _In_ const LIST_ENTRY* Entry)
{
    return Entry->Flink != NULL;
}

/** Milliseconds to a relative KeSetTimer/KeDelayExecutionThread interval. */
FORCEINLINE
LARGE_INTEGER
NTAPI
Usb4HrRelativeMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    return Interval;
}

/** Sleeps for Milliseconds. PASSIVE_LEVEL only. */
FORCEINLINE
VOID
NTAPI
Usb4HrSleepMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval = Usb4HrRelativeMs(Milliseconds);

    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}
