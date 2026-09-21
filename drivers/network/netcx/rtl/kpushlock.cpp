/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Push lock wrapper
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * None of these enter a critical region. The caller does that, the same way
 * it would around ExAcquirePushLockExclusiveEx directly.
 */

#include <ntddk.h>
#include <KPushLock.h>

/* No lock owner boosting is asked for, so every call passes no flags. */
#define KPUSH_LOCK_FLAGS    0

_Use_decl_annotations_
PAGED
KPushLock::KPushLock() noexcept
{
    InitializeInner();
}

_Use_decl_annotations_
PAGED
KPushLock::~KPushLock()
{
}

_Use_decl_annotations_
PAGED
void
KPushLockManualConstruct::Initialize()
{
    InitializeInner();
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::InitializeInner()
{
    ExInitializePushLock(&m_Lock);

#if DBG
    m_ExclusiveOwner = nullptr;
#endif
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::AcquireShared()
{
    ExAcquirePushLockSharedEx(&m_Lock, KPUSH_LOCK_FLAGS);
}

/* A shared hold is released without saying so, which the flag free call allows. */
_Use_decl_annotations_
PAGED
void
KPushLockBase::ReleaseShared()
{
    ExReleasePushLockEx(&m_Lock, KPUSH_LOCK_FLAGS);
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::AcquireExclusive()
{
    ExAcquirePushLockExclusiveEx(&m_Lock, KPUSH_LOCK_FLAGS);

#if DBG
    m_ExclusiveOwner = KeGetCurrentThread();
#endif
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::ReleaseExclusive()
{
#if DBG
    m_ExclusiveOwner = nullptr;
#endif

    ExReleasePushLockExclusiveEx(&m_Lock, KPUSH_LOCK_FLAGS);
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::AssertLockHeld()
{
    NT_ASSERT(m_Lock != 0);
}

_Use_decl_annotations_
PAGED
void
KPushLockBase::AssertLockNotHeld()
{
#if DBG
    NT_ASSERT(m_ExclusiveOwner != KeGetCurrentThread());
#endif
}
