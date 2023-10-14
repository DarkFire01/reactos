/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL, See COPYING in the top level directory
 * FILE:            hal/halx86/amd64/x86bios.c
 * PURPOSE:
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
//#define NDEBUG
#include <debug.h>

#include <fast486.h>

/* GLOBALS *******************************************************************/

/* This page serves as fallback for pages used by Mm */
PFN_NUMBER x86BiosFallbackPfn;

BOOLEAN x86BiosIsInitialized;
LONG x86BiosBufferIsAllocated = 0;
PUCHAR x86BiosMemoryMapping;

/* This the physical address of the bios buffer */
ULONG64 x86BiosBufferPhysical;

VOID
NTAPI
DbgDumpPage(PUCHAR MemBuffer, USHORT Segment)
{

}

VOID
NTAPI
HalInitializeBios(
    _In_ ULONG Phase,
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
   
}

NTSTATUS
NTAPI
x86BiosAllocateBuffer(
    _In_ ULONG *Size,
    _In_ USHORT *Segment,
    _In_ USHORT *Offset)
{
    return 1;
}

NTSTATUS
NTAPI
x86BiosFreeBuffer(
    _In_ USHORT Segment,
    _In_ USHORT Offset)
{
    return 1;
}

NTSTATUS
NTAPI
x86BiosReadMemory(
    _In_ USHORT Segment,
    _In_ USHORT Offset,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size)
{
        return 1;
}

NTSTATUS
NTAPI
x86BiosWriteMemory(
    _In_ USHORT Segment,
    _In_ USHORT Offset,
    _In_reads_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size)
{

    /* Return success */
    return 1;
}

static
VOID
FASTCALL
x86MemRead(
    PFAST486_STATE State,
    ULONG Address,
    PVOID Buffer,
    ULONG Size)
{

}

static
VOID
FASTCALL
x86MemWrite(
    PFAST486_STATE State,
    ULONG Address,
    PVOID Buffer,
    ULONG Size)
{

}

static
BOOLEAN
ValidatePort(
    USHORT Port,
    UCHAR Size,
    BOOLEAN IsWrite)
{
   return FALSE;
}

static
VOID
FASTCALL
x86IoRead(
    PFAST486_STATE State,
    USHORT Port,
    PVOID Buffer,
    ULONG DataCount,
    UCHAR DataSize)
{

}

static
VOID
FASTCALL
x86IoWrite(
    PFAST486_STATE State,
    USHORT Port,
    PVOID Buffer,
    ULONG DataCount,
    UCHAR DataSize)
{
  
}

static
VOID
FASTCALL
x86BOP(
    PFAST486_STATE State,
    UCHAR BopCode)
{
  //  ASSERT(FALSE);
}

static
UCHAR
FASTCALL
x86IntAck (
    PFAST486_STATE State)
{
   // ASSERT(FALSE);
    return 0;
}

BOOLEAN
NTAPI
x86BiosCall(
    _In_ ULONG InterruptNumber,
    _Inout_ PX86_BIOS_REGISTERS Registers)
{
   return FALSE;
}

#ifdef _M_AMD64
BOOLEAN
NTAPI
HalpBiosDisplayReset(VOID)
{
#if 0
    X86_BIOS_REGISTERS Registers;
    ULONG OldEflags;

    /* Save flags and disable interrupts */
    OldEflags = __readeflags();
    _disable();

    /* Set AH = 0 (Set video mode), AL = 0x12 (640x480x16 vga) */
    Registers.Eax = 0x12;

    /* Call INT 0x10 */
    x86BiosCall(0x10, &Registers);

    // FIXME: check result

    /* Restore previous flags */
    __writeeflags(OldEflags);
    return TRUE;
#else
    /* This x64 HAL does NOT currently handle display reset (TODO) */
    return FALSE;
#endif
}
#endif // _M_AMD64
