/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fixed size pool of preallocated items handed out last in, first out
 *
 * The pool is filled once at initialization with every item it will ever
 * hold, so it is full exactly when nothing is out on loan. Owners assert that
 * on teardown.
 */

#pragma once

#include <KNew.h>
#include <KArray.h>

template <typename T>
class KStackPool :
    public NONPAGED_OBJECT<'lpsK'>
{
public:

    /* Sets how many items the pool holds and empties it. */
    PAGED
    bool
    resize(
        _In_ size_t Count)
    {
        m_CurrentIndex = 0;
        return m_Stack.resize(Count);
    }

    NONPAGED
    bool
    IsEmpty(
        void
    ) const
    {
        return m_CurrentIndex == 0;
    }

    NONPAGED
    bool
    IsFull(
        void
    ) const
    {
        return m_CurrentIndex == m_Stack.count();
    }

    NONPAGED
    void
    PushToReturn(
        _In_ T Item)
    {
        NT_FRE_ASSERT(!IsFull());

        m_Stack[m_CurrentIndex] = Item;
        m_CurrentIndex++;
    }

    /* The caller checks IsEmpty first. */
    NONPAGED
    T
    PopToUse(
        void
    )
    {
        NT_FRE_ASSERT(!IsEmpty());

        m_CurrentIndex--;
        return m_Stack[m_CurrentIndex];
    }

    /* Prebuilt pools also address their items by the index they were stored at. */
    NONPAGED
    T &
    operator[](
        _In_ size_t Index)
    {
        return m_Stack[Index];
    }

private:

    Rtl::KArray<T, NonPagedPoolNx>
        m_Stack;

    size_t
        m_CurrentIndex = 0;
};
