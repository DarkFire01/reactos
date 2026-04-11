/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     READ/WRITE_REGISTER_* for ARM64 (memory-mapped device access)
 */

#include <ntoskrnl.h>

UCHAR
NTAPI
READ_REGISTER_UCHAR(PUCHAR Register)
{
    return *(volatile UCHAR *)Register;
}

USHORT
NTAPI
READ_REGISTER_USHORT(PUSHORT Register)
{
    return *(volatile USHORT *)Register;
}

ULONG
NTAPI
READ_REGISTER_ULONG(PULONG Register)
{
    return *(volatile ULONG *)Register;
}

VOID
NTAPI
READ_REGISTER_BUFFER_UCHAR(
    _In_reads_(Count) PUCHAR Register,
    _Out_writes_all_(Count) PUCHAR Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Buffer, Register, Count);
}

VOID
NTAPI
READ_REGISTER_BUFFER_USHORT(
    _In_reads_(Count) PUSHORT Register,
    _Out_writes_all_(Count) PUSHORT Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Buffer, Register, Count * sizeof(USHORT));
}

VOID
NTAPI
READ_REGISTER_BUFFER_ULONG(
    _In_reads_(Count) PULONG Register,
    _Out_writes_all_(Count) PULONG Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Buffer, Register, Count * sizeof(ULONG));
}

VOID
NTAPI
WRITE_REGISTER_UCHAR(PUCHAR Register, UCHAR Value)
{
    *(volatile UCHAR *)Register = Value;
    KeMemoryBarrier();
}

VOID
NTAPI
WRITE_REGISTER_USHORT(PUSHORT Register, USHORT Value)
{
    *(volatile USHORT *)Register = Value;
    KeMemoryBarrier();
}

VOID
NTAPI
WRITE_REGISTER_ULONG(PULONG Register, ULONG Value)
{
    *(volatile ULONG *)Register = Value;
    KeMemoryBarrier();
}

VOID
NTAPI
WRITE_REGISTER_BUFFER_UCHAR(
    _Out_writes_all_(Count) PUCHAR Register,
    _In_reads_(Count) PUCHAR Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Register, Buffer, Count);
    KeMemoryBarrier();
}

VOID
NTAPI
WRITE_REGISTER_BUFFER_USHORT(
    _Out_writes_all_(Count) PUSHORT Register,
    _In_reads_(Count) PUSHORT Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Register, Buffer, Count * sizeof(USHORT));
    KeMemoryBarrier();
}

VOID
NTAPI
WRITE_REGISTER_BUFFER_ULONG(
    _Out_writes_all_(Count) PULONG Register,
    _In_reads_(Count) PULONG Buffer,
    _In_ ULONG Count)
{
    RtlCopyMemory(Register, Buffer, Count * sizeof(ULONG));
    KeMemoryBarrier();
}
