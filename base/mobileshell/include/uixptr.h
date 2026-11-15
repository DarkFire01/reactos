/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Minimal COM smart pointer
 */

#pragma once

#include <windows.h>

template <typename T>
class UixPtr
{
public:
    UixPtr() : m_Ptr(NULL)
    {
    }

    UixPtr(
        _In_opt_ T *Ptr) : m_Ptr(Ptr)
    {
        if (m_Ptr)
            m_Ptr->AddRef();
    }

    UixPtr(
        _In_ const UixPtr &Other) : m_Ptr(Other.m_Ptr)
    {
        if (m_Ptr)
            m_Ptr->AddRef();
    }

    ~UixPtr()
    {
        Reset();
    }

    UixPtr &
    operator=(
        _In_ const UixPtr &Other)
    {
        if (this != &Other)
        {
            if (Other.m_Ptr)
                Other.m_Ptr->AddRef();
            Reset();
            m_Ptr = Other.m_Ptr;
        }
        return *this;
    }

    UixPtr &
    operator=(
        _In_opt_ T *Ptr)
    {
        if (Ptr)
            Ptr->AddRef();
        Reset();
        m_Ptr = Ptr;
        return *this;
    }

    VOID
    Reset(VOID)
    {
        T *Ptr = m_Ptr;

        m_Ptr = NULL;
        if (Ptr)
            Ptr->Release();
    }

    T *
    Get(VOID) const
    {
        return m_Ptr;
    }

    T **
    Put(VOID)
    {
        Reset();
        return &m_Ptr;
    }

    VOID **
    PutVoid(VOID)
    {
        Reset();
        return reinterpret_cast<VOID **>(&m_Ptr);
    }

    T *
    operator->() const
    {
        return m_Ptr;
    }

    explicit operator bool() const
    {
        return m_Ptr != NULL;
    }

    T *
    Detach(VOID)
    {
        T *Ptr = m_Ptr;

        m_Ptr = NULL;
        return Ptr;
    }

    template <typename U>
    HRESULT
    As(
        _Out_ UixPtr<U> *Other) const
    {
        if (!m_Ptr)
            return E_POINTER;
        return m_Ptr->QueryInterface(__uuidof(U), Other->PutVoid());
    }

private:
    T *m_Ptr;
};
