/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The type traits the Windows Implementation Library exposes
 *
 * wistd is WIL's private copy of the parts of the standard library that a
 * kernel driver is allowed to use: no exceptions, no allocation, no runtime.
 * Only the traits the imported sources actually name are here.
 */

#pragma once

#include <ntddk.h>

namespace wistd
{

template<typename T, T v>
struct integral_constant
{
    static constexpr T value = v;
    typedef T value_type;
    typedef integral_constant type;
    constexpr operator value_type() const noexcept { return value; }
    constexpr value_type operator()() const noexcept { return value; }
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<typename T, typename U> struct is_same : false_type { };
template<typename T> struct is_same<T, T> : true_type { };

template<typename T> struct is_void : is_same<void, T> { };

template<typename T>
struct is_trivially_destructible
    : integral_constant<bool, __is_trivially_destructible(T)> { };

template<typename T>
struct is_trivially_default_constructible
    : integral_constant<bool, __is_trivially_constructible(T)> { };

template<typename T> struct is_pointer : false_type { };
template<typename T> struct is_pointer<T *> : true_type { };
template<typename T> struct is_pointer<T * const> : true_type { };
template<typename T> struct is_pointer<T * volatile> : true_type { };
template<typename T> struct is_pointer<T * const volatile> : true_type { };

template<typename T> struct remove_pointer { typedef T type; };
template<typename T> struct remove_pointer<T *> { typedef T type; };
template<typename T> struct remove_pointer<T * const> { typedef T type; };
template<typename T> struct remove_pointer<T * volatile> { typedef T type; };
template<typename T> struct remove_pointer<T * const volatile> { typedef T type; };

template<typename T> struct remove_reference { typedef T type; };
template<typename T> struct remove_reference<T &> { typedef T type; };
template<typename T> struct remove_reference<T &&> { typedef T type; };

template<typename T> struct is_array : false_type { };
template<typename T> struct is_array<T[]> : true_type { };
template<typename T, size_t N> struct is_array<T[N]> : true_type { };

template<typename T> struct remove_extent { typedef T type; };
template<typename T> struct remove_extent<T[]> { typedef T type; };
template<typename T, size_t N> struct remove_extent<T[N]> { typedef T type; };

template<bool B, typename T = void> struct enable_if { };
template<typename T> struct enable_if<true, T> { typedef T type; };

template<typename T, typename U> constexpr bool is_same_v = is_same<T, U>::value;
template<typename T> constexpr bool is_void_v = is_void<T>::value;
template<typename T> constexpr bool is_trivially_destructible_v =
    is_trivially_destructible<T>::value;
template<typename T> constexpr bool is_trivially_default_constructible_v =
    is_trivially_default_constructible<T>::value;

/* Never defined, only ever named inside an unevaluated decltype. */
template<typename T>
typename remove_reference<T>::type && declval() noexcept;

/*
 * result_of is only ever asked for the return type of a callable, which
 * decltype answers directly.
 */
template<typename T> struct result_of;
template<typename F, typename... Args>
struct result_of<F(Args...)>
{
    typedef decltype(declval<F>()(declval<Args>()...)) type;
};

template<typename T>
constexpr typename remove_reference<T>::type && move(T && Value) noexcept
{
    return static_cast<typename remove_reference<T>::type &&>(Value);
}

template<typename T>
constexpr T && forward(typename remove_reference<T>::type & Value) noexcept
{
    return static_cast<T &&>(Value);
}

template<typename T>
constexpr T && forward(typename remove_reference<T>::type && Value) noexcept
{
    return static_cast<T &&>(Value);
}

/* Takes the real address even when the type overloads operator&. */
template<typename T>
inline T * addressof(T & Value) noexcept
{
    return reinterpret_cast<T *>(
        &const_cast<char &>(reinterpret_cast<const volatile char &>(Value)));
}

}
