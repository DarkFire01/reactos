/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The Windows Implementation Library resource wrappers
 *
 * RAII for handles that are released by a free function rather than delete.
 * Only what the imported sources name is here.
 */

#pragma once

#include <wil/common.h>
#include <wil/wistd_type_traits.h>
#include <wil/wistd_memory.h>

namespace wil
{

/*
 * Turns a free function into a deleter type, so the function pointer is part
 * of the type and an instance costs nothing. Used as
 *   wistd::unique_ptr<FILE_OBJECT, wil::function_deleter<decltype(&ObfDereferenceObject), ObfDereferenceObject>>
 */
template<typename FunctionType, FunctionType Function>
struct function_deleter
{
    template<typename T>
    void operator()(T * Pointer) const
    {
        if (Pointer != nullptr)
            Function(Pointer);
    }
};

/*
 * A handle released by a free function. The function is carried as a template
 * argument so an instance costs no more than the handle itself.
 */
template<typename T, typename FunctionType, FunctionType Function>
class unique_any
{
public:

    typedef T pointer;

    unique_any() noexcept : m_value(T()) { }
    unique_any(decltype(nullptr)) noexcept : m_value(T()) { }
    explicit unique_any(T Value) noexcept : m_value(Value) { }

    unique_any(unique_any && Other) noexcept : m_value(Other.release()) { }

    ~unique_any()
    {
        reset();
    }

    unique_any & operator=(unique_any && Other) noexcept
    {
        if (this != wistd::addressof(Other))
            reset(Other.release());

        return *this;
    }

    unique_any(unique_any const &) = delete;
    unique_any & operator=(unique_any const &) = delete;

    T get() const noexcept { return m_value; }

    T release() noexcept
    {
        T Released = m_value;
        m_value = T();
        return Released;
    }

    void reset(T Value = T()) noexcept
    {
        T Old = m_value;
        m_value = Value;

        if (Old != T())
            Function(Old);
    }

    /* Hand over the slot for an out parameter, releasing what was there. */
    T * put() noexcept
    {
        reset();
        return &m_value;
    }

    T * operator&() noexcept { return put(); }

    explicit operator bool() const noexcept { return m_value != T(); }

private:

    T m_value;
};

/* Deleter for elements that own nothing. */
struct empty_deleter
{
    template<typename T>
    void operator()(T const &) const noexcept { }
};

/*
 * An array plus its count. Each element goes to ElementDeleter before the
 * array itself goes to ArrayDeleter.
 */
template<typename ValueType, typename ArrayDeleter, typename ElementDeleter = empty_deleter>
class unique_any_array_ptr
{
public:

    typedef ValueType value_type;
    typedef size_t size_type;
    typedef ValueType * pointer;

    unique_any_array_ptr() noexcept : m_ptr(nullptr), m_size(0) { }

    unique_any_array_ptr(decltype(nullptr)) noexcept : m_ptr(nullptr), m_size(0) { }

    unique_any_array_ptr(pointer Pointer, size_type Size) noexcept : m_ptr(Pointer), m_size(Size) { }

    unique_any_array_ptr(unique_any_array_ptr && Other) noexcept
        : m_ptr(Other.m_ptr), m_size(Other.m_size)
    {
        Other.m_ptr = nullptr;
        Other.m_size = 0;
    }

    unique_any_array_ptr & operator=(unique_any_array_ptr && Other) noexcept
    {
        if (this != wistd::addressof(Other))
        {
            reset();
            m_ptr = Other.m_ptr;
            m_size = Other.m_size;
            Other.m_ptr = nullptr;
            Other.m_size = 0;
        }

        return *this;
    }

    ~unique_any_array_ptr()
    {
        reset();
    }

    unique_any_array_ptr(unique_any_array_ptr const &) = delete;
    unique_any_array_ptr & operator=(unique_any_array_ptr const &) = delete;

    pointer get() const noexcept { return m_ptr; }
    size_type size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }

    pointer begin() const noexcept { return m_ptr; }
    pointer end() const noexcept { return m_ptr + m_size; }

    ValueType & operator[](size_type Index) const noexcept { return m_ptr[Index]; }

    void reset() noexcept
    {
        if (m_ptr == nullptr)
            return;

        for (size_type i = 0; i < m_size; i++)
            ElementDeleter()(m_ptr[i]);

        ArrayDeleter()(m_ptr);
        m_ptr = nullptr;
        m_size = 0;
    }

    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:

    pointer m_ptr;
    size_type m_size;
};

inline void CloseKernelHandle(HANDLE Handle) noexcept
{
    ZwClose(Handle);
}

typedef unique_any<HANDLE, decltype(&CloseKernelHandle), &CloseKernelHandle> unique_handle;
typedef unique_any<HANDLE, decltype(&CloseKernelHandle), &CloseKernelHandle> unique_kernel_handle;

/*
 * Only offered once the WDF headers are in. WDFOBJECT is a plain HANDLE, so
 * every WDF handle type converts to it and one close routine covers them all.
 */
#ifdef _WDFOBJECT_H_

inline void CloseWdfObject(WDFOBJECT Object) noexcept
{
    WdfObjectDelete(Object);
}

template <typename THandle>
using unique_wdf_any = unique_any<THandle, decltype(&CloseWdfObject), &CloseWdfObject>;

typedef unique_wdf_any<WDFOBJECT> unique_wdf_object;
typedef unique_wdf_any<WDFWORKITEM> unique_wdf_work_item;

#endif /* _WDFOBJECT_H_ */

/*
 * Runs a callable when it goes out of scope unless it was released first.
 * The usual shape is an undo action armed early and released once the
 * operation has committed.
 */
template<typename Callable>
class scope_exit_t
{
public:

    explicit scope_exit_t(Callable && Action) noexcept
        : m_action(wistd::move(Action)), m_armed(true) { }

    scope_exit_t(scope_exit_t && Other) noexcept
        : m_action(wistd::move(Other.m_action)), m_armed(Other.m_armed)
    {
        Other.m_armed = false;
    }

    ~scope_exit_t()
    {
        if (m_armed)
            m_action();
    }

    void release() noexcept { m_armed = false; }

    scope_exit_t(scope_exit_t const &) = delete;
    scope_exit_t & operator=(scope_exit_t const &) = delete;

private:

    Callable m_action;
    bool m_armed;
};

template<typename Callable>
inline scope_exit_t<Callable> scope_exit(Callable && Action) noexcept
{
    return scope_exit_t<Callable>(wistd::forward<Callable>(Action));
}

#ifdef _WDFSYNC_H_

/* Holds a WDFWAITLOCK for the life of the returned object. */
class wdf_wait_lock_release_scope_exit
{
public:

    explicit wdf_wait_lock_release_scope_exit(WDFWAITLOCK Lock) noexcept
        : m_lock(Lock) { }

    wdf_wait_lock_release_scope_exit(wdf_wait_lock_release_scope_exit && Other) noexcept
        : m_lock(Other.m_lock)
    {
        Other.m_lock = nullptr;
    }

    ~wdf_wait_lock_release_scope_exit()
    {
        if (m_lock != nullptr)
            WdfWaitLockRelease(m_lock);
    }

    wdf_wait_lock_release_scope_exit(wdf_wait_lock_release_scope_exit const &) = delete;

private:

    WDFWAITLOCK m_lock;
};

inline wdf_wait_lock_release_scope_exit
acquire_wdf_wait_lock(
    _In_ WDFWAITLOCK Lock) noexcept
{
    WdfWaitLockAcquire(Lock, nullptr);
    return wdf_wait_lock_release_scope_exit(Lock);
}

#endif /* _WDFSYNC_H_ */

/*
 * Lets a smart pointer be passed to a function that writes a raw pointer out,
 * taking ownership of whatever it wrote when the wrapper dies.
 */
template<typename SmartPointer>
class out_param_t
{
public:

    typedef typename SmartPointer::pointer pointer;

    explicit out_param_t(SmartPointer & Target) noexcept
        : m_target(Target), m_raw(nullptr) { }

    ~out_param_t()
    {
        m_target.reset(m_raw);
    }

    operator pointer *() noexcept { return &m_raw; }

private:

    SmartPointer & m_target;
    pointer m_raw;
};

template<typename SmartPointer>
inline out_param_t<SmartPointer> out_param(SmartPointer & Target) noexcept
{
    return out_param_t<SmartPointer>(Target);
}

/*
 * The classes here derive from the KALLOCATOR templates in rtl/inc/knew.h,
 * which supply a nothrow operator new, so a failed allocation returns null
 * rather than raising.
 */
template<typename T, typename... Args>
inline typename wistd::enable_if<!wistd::is_array<T>::value, wistd::unique_ptr<T>>::type
make_unique_nothrow(
    Args &&... Arguments)
{
    return wistd::unique_ptr<T>(new (std::nothrow) T(wistd::forward<Args>(Arguments)...));
}

/* Unbounded arrays only, with every element value initialized. */
template<typename T>
inline typename wistd::enable_if<wistd::is_array<T>::value, wistd::unique_ptr<T>>::type
make_unique_nothrow(
    _In_ size_t Count)
{
    typedef typename wistd::remove_extent<T>::type Element;

    return wistd::unique_ptr<T>(new (std::nothrow) Element[Count]());
}

}
