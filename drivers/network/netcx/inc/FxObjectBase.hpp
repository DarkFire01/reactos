/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     C++ object living in the context space of a WDF object
 *
 * The object is placement constructed into a WDF context and destroyed from
 * the WDF destroy callback, so it never owns the memory it occupies. The
 * last template parameter is false for every user and changes nothing.
 */

#pragma once

#include <new.h>

template <
    typename THandle,
    typename TObject,
    TObject *(*GetObjectFromHandle)(THandle),
    bool Unused>
class CFxObject
{
public:

    THandle
    GetFxObject(
        void
    ) const
    {
        return m_FxObject;
    }

    /* Destruction is tied to the WDF object, whatever the caller's other callbacks are. */
    static
    void
    _SetObjectAttributes(
        _Inout_ WDF_OBJECT_ATTRIBUTES *Attributes)
    {
        Attributes->EvtDestroyCallback = _OnDestroy;
    }

    /* The context memory belongs to WDF. */
    void
    operator delete(
        _In_ void *Object)
    {
        UNREFERENCED_PARAMETER(Object);
    }

protected:

    CFxObject(
        _In_ THandle FxObject) :
        m_FxObject(FxObject),
        m_Constructed(true)
    {
    }

    virtual
    ~CFxObject(
        void
    ) = default;

    virtual
    void
    OnCleanup(
        void)
    {
    }

    /* WDF zeroes the context, so a null handle means the constructor never ran. */
    static
    void
    _OnDestroy(
        _In_ WDFOBJECT Object)
    {
        CFxObject *object = GetObjectFromHandle(reinterpret_cast<THandle>(Object));

        if (object->m_FxObject != nullptr)
            delete object;
    }

    THandle const
        m_FxObject;

    bool
        m_Constructed;
};

/* Deleting the WDF object is what destroys the C++ object living in its context. */
struct CFxObjectDeleter
{
    template <typename T>
    void
    operator()(
        _In_ T *Object) const
    {
        WdfObjectDelete(Object->GetFxObject());
    }
};
