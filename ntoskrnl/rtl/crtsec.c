/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bounds checked C runtime routines exported to drivers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>
#include <errno.h>
#include <stdarg.h>

#define NDEBUG
#include <debug.h>

/* The kernel has no errno, every failure is reported through the return value only */

int __cdecl _vsnprintf(char *Buffer, size_t Count, const char *Format, va_list Args);
int __cdecl _vsnwprintf(wchar_t *Buffer, size_t Count, const wchar_t *Format, va_list Args);

errno_t
__cdecl
memcpy_s(
    _Out_writes_bytes_to_opt_(DestSize, Count) void *Dest,
    _In_ size_t DestSize,
    _In_reads_bytes_opt_(Count) const void *Src,
    _In_ size_t Count)
{
    if (Count == 0)
        return 0;

    if (!Dest)
        return EINVAL;

    if (!Src)
    {
        RtlZeroMemory(Dest, DestSize);
        return EINVAL;
    }

    if (DestSize < Count)
    {
        RtlZeroMemory(Dest, DestSize);
        return ERANGE;
    }

    RtlCopyMemory(Dest, Src, Count);
    return 0;
}

errno_t
__cdecl
memmove_s(
    _Out_writes_bytes_to_opt_(DestSize, Count) void *Dest,
    _In_ size_t DestSize,
    _In_reads_bytes_opt_(Count) const void *Src,
    _In_ size_t Count)
{
    if (Count == 0)
        return 0;

    if (!Dest || !Src)
        return EINVAL;

    if (DestSize < Count)
        return ERANGE;

    RtlMoveMemory(Dest, Src, Count);
    return 0;
}

errno_t
__cdecl
strcpy_s(
    _Out_writes_z_(DestSize) char *Dest,
    _In_ size_t DestSize,
    _In_z_ const char *Src)
{
    size_t i;

    if (!Dest || DestSize == 0)
        return EINVAL;

    if (!Src)
    {
        Dest[0] = '\0';
        return EINVAL;
    }

    for (i = 0; i < DestSize; i++)
    {
        Dest[i] = Src[i];
        if (Src[i] == '\0')
            return 0;
    }

    Dest[0] = '\0';
    return ERANGE;
}


errno_t
__cdecl
strncpy_s(
    _Out_writes_z_(DestSize) char *Dest,
    _In_ size_t DestSize,
    _In_reads_or_z_opt_(Count) const char *Src,
    _In_ size_t Count)
{
    size_t i;

    if (Count == 0 && !Dest && DestSize == 0)
        return 0;

    if (!Dest || DestSize == 0)
        return EINVAL;

    if (Count == 0)
    {
        Dest[0] = '\0';
        return 0;
    }

    if (!Src)
    {
        Dest[0] = '\0';
        return EINVAL;
    }

    for (i = 0; i < DestSize; i++)
    {
        if (i == Count || Src[i] == '\0')
        {
            Dest[i] = '\0';
            return 0;
        }
        Dest[i] = Src[i];
    }

    /* Out of room before the source or the count ran out */
    if (Count == _TRUNCATE)
    {
        Dest[DestSize - 1] = '\0';
        return STRUNCATE;
    }

    Dest[0] = '\0';
    return ERANGE;
}


errno_t
__cdecl
strcat_s(
    _Inout_updates_z_(DestSize) char *Dest,
    _In_ size_t DestSize,
    _In_z_ const char *Src)
{
    size_t End, i;

    if (!Dest || DestSize == 0)
        return EINVAL;

    if (!Src)
    {
        Dest[0] = '\0';
        return EINVAL;
    }

    for (End = 0; End < DestSize && Dest[End] != '\0'; End++);

    /* The destination was not terminated within its size */
    if (End == DestSize)
    {
        Dest[0] = '\0';
        return EINVAL;
    }

    for (i = 0; End + i < DestSize; i++)
    {
        Dest[End + i] = Src[i];
        if (Src[i] == '\0')
            return 0;
    }

    Dest[0] = '\0';
    return ERANGE;
}


char *
__cdecl
strtok_s(
    _Inout_opt_z_ char *String,
    _In_z_ const char *Delimiters,
    _Inout_ _Deref_prepost_opt_z_ char **Context)
{
    const char *d;
    char *Token;

    if (!Delimiters || !Context || (!String && !*Context))
        return NULL;

    if (!String)
        String = *Context;

    /* Skip leading delimiters */
    for (; *String; String++)
    {
        for (d = Delimiters; *d && *d != *String; d++);
        if (!*d)
            break;
    }

    if (!*String)
    {
        *Context = String;
        return NULL;
    }

    Token = String;
    for (; *String; String++)
    {
        for (d = Delimiters; *d && *d != *String; d++);
        if (*d)
        {
            *String++ = '\0';
            break;
        }
    }

    *Context = String;
    return Token;
}

/* Formats into Buffer, Limit excludes the terminator, returns the length or -1 when it did not fit */
static
int
CrtpFormatA(
    _Out_writes_(Limit + 1) char *Buffer,
    _In_ size_t Limit,
    _In_z_ const char *Format,
    _In_ va_list Args)
{
    int Length = _vsnprintf(Buffer, Limit, Format, Args);

    /* An exact fit comes back unterminated, there is always room for the terminator here */
    if (Length < 0 || (size_t)Length > Limit)
    {
        Buffer[Limit] = '\0';
        return -1;
    }

    Buffer[Length] = '\0';
    return Length;
}

static
int
CrtpFormatW(
    _Out_writes_(Limit + 1) wchar_t *Buffer,
    _In_ size_t Limit,
    _In_z_ const wchar_t *Format,
    _In_ va_list Args)
{
    int Length = _vsnwprintf(Buffer, Limit, Format, Args);

    if (Length < 0 || (size_t)Length > Limit)
    {
        Buffer[Limit] = L'\0';
        return -1;
    }

    Buffer[Length] = L'\0';
    return Length;
}

int
__cdecl
_vsnprintf_s(
    _Out_writes_opt_(BufferSize) char *Buffer,
    _In_ size_t BufferSize,
    _In_ size_t Count,
    _In_z_ _Printf_format_string_ const char *Format,
    _In_ va_list Args)
{
    size_t Limit;
    int Length;

    if (!Buffer || BufferSize == 0 || !Format)
        return -1;

    /* Count only matters when it is the tighter bound */
    if (Count != _TRUNCATE && Count < BufferSize)
        Limit = Count;
    else
        Limit = BufferSize - 1;

    Length = CrtpFormatA(Buffer, Limit, Format, Args);
    if (Length >= 0)
        return Length;

    if (Count == _TRUNCATE || Count < BufferSize)
        return -1;

    Buffer[0] = '\0';
    return -1;
}

int
__cdecl
_vsnwprintf_s(
    _Out_writes_opt_(BufferSize) wchar_t *Buffer,
    _In_ size_t BufferSize,
    _In_ size_t Count,
    _In_z_ _Printf_format_string_ const wchar_t *Format,
    _In_ va_list Args)
{
    size_t Limit;
    int Length;

    if (!Buffer || BufferSize == 0 || !Format)
        return -1;

    if (Count != _TRUNCATE && Count < BufferSize)
        Limit = Count;
    else
        Limit = BufferSize - 1;

    Length = CrtpFormatW(Buffer, Limit, Format, Args);
    if (Length >= 0)
        return Length;

    if (Count == _TRUNCATE || Count < BufferSize)
        return -1;

    Buffer[0] = L'\0';
    return -1;
}

int
__cdecl
_snprintf_s(
    _Out_writes_opt_(BufferSize) char *Buffer,
    _In_ size_t BufferSize,
    _In_ size_t Count,
    _In_z_ _Printf_format_string_ const char *Format,
    ...)
{
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = _vsnprintf_s(Buffer, BufferSize, Count, Format, Args);
    va_end(Args);
    return Length;
}

int
__cdecl
_snwprintf_s(
    _Out_writes_opt_(BufferSize) wchar_t *Buffer,
    _In_ size_t BufferSize,
    _In_ size_t Count,
    _In_z_ _Printf_format_string_ const wchar_t *Format,
    ...)
{
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = _vsnwprintf_s(Buffer, BufferSize, Count, Format, Args);
    va_end(Args);
    return Length;
}

int
__cdecl
vsprintf_s(
    _Out_writes_(BufferSize) char *Buffer,
    _In_ size_t BufferSize,
    _In_z_ _Printf_format_string_ const char *Format,
    _In_ va_list Args)
{
    int Length;

    if (!Buffer || BufferSize == 0 || !Format)
        return -1;

    /* Unlike the counted variant, output that does not fit is an error */
    Length = CrtpFormatA(Buffer, BufferSize - 1, Format, Args);
    if (Length < 0)
        Buffer[0] = '\0';
    return Length;
}

int
__cdecl
vswprintf_s(
    _Out_writes_(BufferSize) wchar_t *Buffer,
    _In_ size_t BufferSize,
    _In_z_ _Printf_format_string_ const wchar_t *Format,
    _In_ va_list Args)
{
    int Length;

    if (!Buffer || BufferSize == 0 || !Format)
        return -1;

    Length = CrtpFormatW(Buffer, BufferSize - 1, Format, Args);
    if (Length < 0)
        Buffer[0] = L'\0';
    return Length;
}

int
__cdecl
sprintf_s(
    _Out_writes_(BufferSize) char *Buffer,
    _In_ size_t BufferSize,
    _In_z_ _Printf_format_string_ const char *Format,
    ...)
{
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = vsprintf_s(Buffer, BufferSize, Format, Args);
    va_end(Args);
    return Length;
}

int
__cdecl
swprintf_s(
    _Out_writes_(BufferSize) wchar_t *Buffer,
    _In_ size_t BufferSize,
    _In_z_ _Printf_format_string_ const wchar_t *Format,
    ...)
{
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = vswprintf_s(Buffer, BufferSize, Format, Args);
    va_end(Args);
    return Length;
}
