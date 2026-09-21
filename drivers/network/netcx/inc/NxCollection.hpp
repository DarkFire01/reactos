/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Push lock protected list of class extension objects
 *
 * Members are linked through their own m_linkage, which is why each member
 * class befriends its collection. Walks hold the lock shared, so a callback
 * must not add to or remove from the collection it is walking.
 */

#pragma once

#include <KPushLock.h>
#include <KLockHolder.h>

template <typename T>
class NxCollection
{
public:

    NxCollection(
        void)
    {
        InitializeListHead(&m_ListHead);
    }

    PAGED
    void
    Add(
        _In_ T *Item)
    {
        KLockThisExclusive lock(m_ListLock);

        InsertTailList(&m_ListHead, &Item->m_linkage);
        m_Count++;
    }

    /* An item that is not in the collection is left alone. */
    PAGED
    void
    Remove(
        _In_ T *Item)
    {
        KLockThisExclusive lock(m_ListLock);

        if (!IsLinked(&Item->m_linkage))
            return;

        RemoveEntryList(&Item->m_linkage);
        Item->m_linkage = {};
        m_Count--;
    }

    template <typename Functor>
    PAGED
    void
    ForEach(
        _In_ Functor Callback)
    {
        KLockThisShared lock(m_ListLock);

        for (LIST_ENTRY *link = m_ListHead.Flink; link != &m_ListHead; link = link->Flink)
            Callback(*CONTAINING_RECORD(link, T, m_linkage));
    }

    ULONG
    Count(
        void
    ) const
    {
        return m_Count;
    }

protected:

    LIST_ENTRY
        m_ListHead;

    ULONG
        m_Count = 0;

    mutable KPushLock
        m_ListLock;

private:

    bool
    IsLinked(
        _In_ LIST_ENTRY const *Link
    ) const
    {
        for (LIST_ENTRY const *entry = m_ListHead.Flink; entry != &m_ListHead; entry = entry->Flink)
        {
            if (entry == Link)
                return true;
        }

        return false;
    }
};
