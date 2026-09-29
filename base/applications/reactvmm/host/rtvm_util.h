/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The few library pieces this needs, since the one here has none
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The compiler is new enough for whatever the language offers. The library
 * beside it is not: the STLport in this tree predates fixed width integers and
 * every owning pointer worth the name, and nothing else in the tree builds C++
 * against it. So the language is used freely and the handful of library pieces
 * that are actually wanted are written here, where they are small enough to
 * read in one sitting.
 */

#pragma once

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace rtvm
{

/*
 * Anything owning a handle. The traits say what an empty one looks like and
 * how to give it back, so the same class covers a file and a library.
 */
template<typename Traits>
class Unique
{
public:
    using Value = typename Traits::Value;

    Unique() noexcept : m_Value(Traits::Empty()) {}
    explicit Unique(Value Held) noexcept : m_Value(Held) {}

    ~Unique() { Reset(); }

    Unique(Unique &&Other) noexcept : m_Value(Other.Release()) {}

    Unique &operator=(Unique &&Other) noexcept
    {
        if (this != &Other)
            Reset(Other.Release());

        return *this;
    }

    Unique(const Unique &) = delete;
    Unique &operator=(const Unique &) = delete;

    Value Get() const noexcept { return m_Value; }
    explicit operator bool() const noexcept { return m_Value != Traits::Empty(); }

    void Reset(Value Held = Traits::Empty()) noexcept
    {
        if (m_Value != Traits::Empty())
            Traits::Close(m_Value);

        m_Value = Held;
    }

    /* Hands ownership out, leaving this one empty */
    Value Release() noexcept
    {
        Value Held = m_Value;

        m_Value = Traits::Empty();
        return Held;
    }

private:
    Value m_Value;
};

struct FileTraits
{
    using Value = HANDLE;
    static HANDLE Empty() noexcept { return INVALID_HANDLE_VALUE; }
    static void Close(HANDLE Held) noexcept { CloseHandle(Held); }
};

struct ObjectTraits
{
    using Value = HANDLE;
    static HANDLE Empty() noexcept { return nullptr; }
    static void Close(HANDLE Held) noexcept { CloseHandle(Held); }
};

struct LibraryTraits
{
    using Value = HMODULE;
    static HMODULE Empty() noexcept { return nullptr; }

    /*
     * Deliberately kept. A module is free to have left a thread or a callback
     * behind, and unloading under one of those costs more than the handle does.
     */
    static void Close(HMODULE) noexcept {}
};

using UniqueFile = Unique<FileTraits>;
using UniqueObject = Unique<ObjectTraits>;
using UniqueLibrary = Unique<LibraryTraits>;

/*
 * Owns whatever it points at, and is the only one that does. Enough of the
 * shape of the real thing to read the same at the call site.
 */
template<typename T>
class Owned
{
public:
    Owned() noexcept : m_Pointer(nullptr) {}
    explicit Owned(T *Pointer) noexcept : m_Pointer(Pointer) {}

    ~Owned() { Reset(); }

    Owned(Owned &&Other) noexcept : m_Pointer(Other.Release()) {}

    Owned &operator=(Owned &&Other) noexcept
    {
        if (this != &Other)
            Reset(Other.Release());

        return *this;
    }

    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;

    T *Get() const noexcept { return m_Pointer; }
    T *operator->() const noexcept { return m_Pointer; }
    T &operator*() const noexcept { return *m_Pointer; }
    explicit operator bool() const noexcept { return m_Pointer != nullptr; }

    void Reset(T *Pointer = nullptr) noexcept
    {
        delete m_Pointer;
        m_Pointer = Pointer;
    }

    T *Release() noexcept
    {
        T *Pointer = m_Pointer;

        m_Pointer = nullptr;
        return Pointer;
    }

private:
    T *m_Pointer;
};

/*
 * A list with its room decided when it is declared. A machine's hardware is
 * counted in tens, so growing is a problem it does not have, and a fixed one
 * cannot fail to grow at an awkward moment.
 */
template<typename T, ULONG Capacity>
class Array
{
public:
    Array() noexcept : m_Count(0) {}

    bool Add(const T &Value) noexcept
    {
        if (m_Count >= Capacity)
            return false;

        m_Items[m_Count] = Value;
        m_Count++;
        return true;
    }

    /* Takes the last one back out, for walking a list backwards as it empties */
    bool Take(T &Value) noexcept
    {
        if (m_Count == 0)
            return false;

        m_Count--;
        Value = m_Items[m_Count];
        return true;
    }

    void Clear() noexcept { m_Count = 0; }

    ULONG Count() const noexcept { return m_Count; }
    bool Full() const noexcept { return m_Count >= Capacity; }

    T &operator[](ULONG Index) noexcept { return m_Items[Index]; }
    const T &operator[](ULONG Index) const noexcept { return m_Items[Index]; }

    /* Enough for a range based loop, which is all anything here wants */
    T *begin() noexcept { return m_Items; }
    T *end() noexcept { return m_Items + m_Count; }
    const T *begin() const noexcept { return m_Items; }
    const T *end() const noexcept { return m_Items + m_Count; }

private:
    T m_Items[Capacity];
    ULONG m_Count;
};

/* A name or a path, owned outright, with no allocation behind it */
template<ULONG Capacity>
class Text
{
public:
    Text() noexcept { m_Text[0] = '\0'; }

    bool Set(const char *Value) noexcept
    {
        if (Value == nullptr)
        {
            m_Text[0] = '\0';
            return true;
        }

        size_t Length = strlen(Value);

        if (Length >= Capacity)
            return false;

        memcpy(m_Text, Value, Length + 1);
        return true;
    }

    /* Takes only what comes before the separator, for splitting a request */
    bool SetUpTo(const char *Value, char Separator) noexcept
    {
        const char *Found = strchr(Value, Separator);
        size_t Length = (Found != nullptr) ? (size_t)(Found - Value) : strlen(Value);

        if (Length >= Capacity)
            return false;

        memcpy(m_Text, Value, Length);
        m_Text[Length] = '\0';
        return true;
    }

    const char *Get() const noexcept { return m_Text; }
    bool Empty() const noexcept { return m_Text[0] == '\0'; }

    operator const char *() const noexcept { return m_Text; }

private:
    char m_Text[Capacity];
};

/* Holds a critical section for as long as it is in scope */
class Locked
{
public:
    explicit Locked(CRITICAL_SECTION &Section) noexcept : m_Section(Section)
    {
        EnterCriticalSection(&m_Section);
    }

    ~Locked() { LeaveCriticalSection(&m_Section); }

    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;

private:
    CRITICAL_SECTION &m_Section;
};

} /* namespace rtvm */
