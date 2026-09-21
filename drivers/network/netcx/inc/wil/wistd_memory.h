/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     wistd::unique_ptr
 *
 * Move only, no exceptions, and the deleter is carried as a type rather than
 * a stored function pointer so an empty one costs nothing.
 */

#pragma once

#include <wil/wistd_type_traits.h>

namespace wistd
{

template<typename T>
struct default_delete
{
    void operator()(T * Pointer) const
    {
        delete Pointer;
    }
};

template<typename T, typename Deleter = default_delete<T>>
class unique_ptr
{
public:

    typedef T element_type;
    typedef T * pointer;
    typedef Deleter deleter_type;

    unique_ptr() noexcept : m_pointer(nullptr) { }
    unique_ptr(decltype(nullptr)) noexcept : m_pointer(nullptr) { }
    explicit unique_ptr(pointer Pointer) noexcept : m_pointer(Pointer) { }

    unique_ptr(unique_ptr && Other) noexcept : m_pointer(Other.release()) { }

    /* Lets a unique_ptr to a derived type move into one to its base. */
    template<typename U, typename E>
    unique_ptr(unique_ptr<U, E> && Other) noexcept : m_pointer(Other.release()) { }

    ~unique_ptr()
    {
        reset();
    }

    unique_ptr & operator=(unique_ptr && Other) noexcept
    {
        if (this != wistd::addressof(Other))
            reset(Other.release());

        return *this;
    }

    unique_ptr & operator=(decltype(nullptr)) noexcept
    {
        reset();
        return *this;
    }

    unique_ptr(unique_ptr const &) = delete;
    unique_ptr & operator=(unique_ptr const &) = delete;

    pointer get() const noexcept { return m_pointer; }

    pointer release() noexcept
    {
        pointer Released = m_pointer;
        m_pointer = nullptr;
        return Released;
    }

    void reset(pointer Pointer = nullptr) noexcept
    {
        pointer Old = m_pointer;
        m_pointer = Pointer;

        if (Old != nullptr)
            Deleter()(Old);
    }

    void swap(unique_ptr & Other) noexcept
    {
        pointer Temp = m_pointer;
        m_pointer = Other.m_pointer;
        Other.m_pointer = Temp;
    }

    explicit operator bool() const noexcept { return m_pointer != nullptr; }

    typename wistd::remove_pointer<pointer>::type & operator*() const { return *m_pointer; }
    pointer operator->() const noexcept { return m_pointer; }

private:

    pointer m_pointer;
};

template<typename T, typename D>
inline bool operator==(unique_ptr<T, D> const & Pointer, decltype(nullptr)) noexcept
{
    return Pointer.get() == nullptr;
}

template<typename T, typename D>
inline bool operator!=(unique_ptr<T, D> const & Pointer, decltype(nullptr)) noexcept
{
    return Pointer.get() != nullptr;
}

}
