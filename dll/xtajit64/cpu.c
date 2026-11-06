/*
 * PROJECT:     ReactOS x64 emulator
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     x64 instruction interpreter
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "xtajit64.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

/* An x64 ret the native side returns to when an entry thunk leaves the return address on the stack */
PUCHAR XtapX64Ret;

typedef struct _XTA_INSN
{
    ULONG_PTR Start;
    ULONG_PTR Next;
    UCHAR Rex;
    BOOLEAN OpSize16;
    BOOLEAN RepE;
    BOOLEAN RepNE;
    BOOLEAN SegGs;
    UCHAR Size;
    UCHAR ModRm;
    UCHAR Reg;
    BOOLEAN IsMem;
    UCHAR RmReg;
    ULONG_PTR Address;
} XTA_INSN, *PXTA_INSN;

/* FUNCTIONS ****************************************************************/

static
PULONG64
Gpr(
    _In_ PARM64EC_NT_CONTEXT Context)
{
    /* Rax to R15 are contiguous in the x64 CONTEXT */
    return &Context->X8;
}

static
ULONG64
SizeMask(
    _In_ UCHAR Size)
{
    return (Size == 8) ? ~0ULL : ((1ULL << (Size * 8)) - 1);
}

static
ULONG64
SignBit(
    _In_ UCHAR Size)
{
    return 1ULL << (Size * 8 - 1);
}

static
LONG64
SignExtend(
    _In_ ULONG64 Value,
    _In_ UCHAR Size)
{
    ULONG Shift = 64 - Size * 8;
    return ((LONG64)(Value << Shift)) >> Shift;
}

static
ULONG64
FetchImm(
    _Inout_ PXTA_INSN Insn,
    _In_ UCHAR Size)
{
    ULONG64 Value = 0;
    RtlCopyMemory(&Value, (PVOID)Insn->Next, Size);
    Insn->Next += Size;
    return Value;
}

/* Byte registers 4 to 7 are AH to BH unless a REX prefix is present */
static
ULONG64
ReadReg(
    _In_ PARM64EC_NT_CONTEXT Context,
    _In_ PXTA_INSN Insn,
    _In_ UCHAR Reg,
    _In_ UCHAR Size)
{
    if (Size == 1 && !Insn->Rex && Reg >= 4 && Reg < 8)
        return (Gpr(Context)[Reg - 4] >> 8) & 0xFF;

    return Gpr(Context)[Reg] & SizeMask(Size);
}

static
VOID
WriteReg(
    _In_ PARM64EC_NT_CONTEXT Context,
    _In_ PXTA_INSN Insn,
    _In_ UCHAR Reg,
    _In_ UCHAR Size,
    _In_ ULONG64 Value)
{
    PULONG64 Regs = Gpr(Context);

    if (Size == 1 && !Insn->Rex && Reg >= 4 && Reg < 8)
    {
        Regs[Reg - 4] = (Regs[Reg - 4] & ~0xFF00ULL) | ((Value & 0xFF) << 8);
        return;
    }

    switch (Size)
    {
        case 1: Regs[Reg] = (Regs[Reg] & ~0xFFULL) | (Value & 0xFF); break;
        case 2: Regs[Reg] = (Regs[Reg] & ~0xFFFFULL) | (Value & 0xFFFF); break;
        /* 32-bit results clear the upper half */
        case 4: Regs[Reg] = Value & 0xFFFFFFFF; break;
        default: Regs[Reg] = Value; break;
    }
}

static
ULONG64
ReadMem(
    _In_ ULONG_PTR Address,
    _In_ UCHAR Size)
{
    ULONG64 Value = 0;
    RtlCopyMemory(&Value, (PVOID)Address, Size);
    return Value;
}

static
VOID
WriteMem(
    _In_ ULONG_PTR Address,
    _In_ UCHAR Size,
    _In_ ULONG64 Value)
{
    RtlCopyMemory((PVOID)Address, &Value, Size);
}

/* Decodes ModRM, SIB and displacement. ImmSize is the immediate that follows, RIP relative addressing needs it. */
static
VOID
DecodeModRm(
    _In_ PXTA_THREAD Thread,
    _Inout_ PXTA_INSN Insn,
    _In_ UCHAR ImmSize)
{
    PULONG64 Regs = Gpr(Thread->CpuArea->ContextAmd64);
    UCHAR Mod, Rm;
    ULONG64 Address = 0;
    BOOLEAN RipRelative = FALSE;

    Insn->ModRm = *(PUCHAR)Insn->Next++;
    Mod = Insn->ModRm >> 6;
    Insn->Reg = ((Insn->ModRm >> 3) & 7) | ((Insn->Rex & 4) << 1);
    Rm = Insn->ModRm & 7;

    if (Mod == 3)
    {
        Insn->IsMem = FALSE;
        Insn->RmReg = Rm | ((Insn->Rex & 1) << 3);
        return;
    }

    Insn->IsMem = TRUE;

    if (Rm == 4)
    {
        UCHAR Sib = *(PUCHAR)Insn->Next++;
        UCHAR Scale = Sib >> 6;
        UCHAR Index = ((Sib >> 3) & 7) | ((Insn->Rex & 2) << 2);
        UCHAR Base = (Sib & 7) | ((Insn->Rex & 1) << 3);

        if (Index != REG_RSP)
            Address = Regs[Index] << Scale;

        if ((Base & 7) == REG_RBP && Mod == 0)
            Address += (LONG64)(LONG)FetchImm(Insn, 4);
        else
            Address += Regs[Base];
    }
    else if (Rm == 5 && Mod == 0)
    {
        RipRelative = TRUE;
        Address = (LONG64)(LONG)FetchImm(Insn, 4);
    }
    else
    {
        Address = Regs[Rm | ((Insn->Rex & 1) << 3)];
    }

    if (Mod == 1)
        Address += (LONG64)(CHAR)FetchImm(Insn, 1);
    else if (Mod == 2)
        Address += (LONG64)(LONG)FetchImm(Insn, 4);

    if (RipRelative)
        Address += Insn->Next + ImmSize;

    if (Insn->SegGs)
        Address += Thread->Gs;

    Insn->Address = (ULONG_PTR)Address;
}

static
ULONG64
ReadRm(
    _In_ PXTA_THREAD Thread,
    _In_ PXTA_INSN Insn,
    _In_ UCHAR Size)
{
    if (Insn->IsMem)
        return ReadMem(Insn->Address, Size);
    return ReadReg(Thread->CpuArea->ContextAmd64, Insn, Insn->RmReg, Size);
}

static
VOID
WriteRm(
    _In_ PXTA_THREAD Thread,
    _In_ PXTA_INSN Insn,
    _In_ UCHAR Size,
    _In_ ULONG64 Value)
{
    if (Insn->IsMem)
        WriteMem(Insn->Address, Size, Value);
    else
        WriteReg(Thread->CpuArea->ContextAmd64, Insn, Insn->RmReg, Size, Value);
}

static
BOOLEAN
Parity(
    _In_ ULONG64 Value)
{
    UCHAR Byte = (UCHAR)Value;
    Byte ^= Byte >> 4;
    Byte ^= Byte >> 2;
    Byte ^= Byte >> 1;
    return !(Byte & 1);
}

static
VOID
SetFlags(
    _Inout_ PARM64EC_NT_CONTEXT Context,
    _In_ ULONG Mask,
    _In_ ULONG Value)
{
    Context->AMD64_EFlags = (Context->AMD64_EFlags & ~Mask) | (Value & Mask);
}

static
VOID
FlagsLogic(
    _Inout_ PARM64EC_NT_CONTEXT Context,
    _In_ ULONG64 Result,
    _In_ UCHAR Size)
{
    ULONG Flags = 0;

    Result &= SizeMask(Size);
    if (!Result) Flags |= FLAG_ZF;
    if (Result & SignBit(Size)) Flags |= FLAG_SF;
    if (Parity(Result)) Flags |= FLAG_PF;
    SetFlags(Context, FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF, Flags);
}

/* Arithmetic with flags. Op 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp. */
static
ULONG64
Alu(
    _Inout_ PARM64EC_NT_CONTEXT Context,
    _In_ UCHAR Op,
    _In_ ULONG64 Left,
    _In_ ULONG64 Right,
    _In_ UCHAR Size)
{
    ULONG64 Mask = SizeMask(Size), Sign = SignBit(Size);
    ULONG64 Carry = (Context->AMD64_EFlags & FLAG_CF) ? 1 : 0;
    ULONG64 Result;
    ULONG Flags = 0;

    Left &= Mask;
    Right &= Mask;

    switch (Op)
    {
        case 0:
        case 2:
            if (Op == 0) Carry = 0;
            Result = (Left + Right + Carry) & Mask;
            if (Result < Left || (Carry && Result == Left)) Flags |= FLAG_CF;
            if (((Left ^ Result) & (Right ^ Result)) & Sign) Flags |= FLAG_OF;
            break;

        case 3:
        case 5:
        case 7:
            if (Op != 3) Carry = 0;
            Result = (Left - Right - Carry) & Mask;
            if (Left < Right || (Carry && Left == Right)) Flags |= FLAG_CF;
            if (((Left ^ Right) & (Left ^ Result)) & Sign) Flags |= FLAG_OF;
            break;

        case 1: Result = Left | Right; break;
        case 4: Result = Left & Right; break;
        default: Result = Left ^ Right; break;
    }

    if (!Result) Flags |= FLAG_ZF;
    if (Result & Sign) Flags |= FLAG_SF;
    if (Parity(Result)) Flags |= FLAG_PF;
    if ((Left ^ Right ^ Result) & 0x10) Flags |= FLAG_AF;
    SetFlags(Context, FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF, Flags);

    return Result;
}

static
BOOLEAN
Condition(
    _In_ PARM64EC_NT_CONTEXT Context,
    _In_ UCHAR Code)
{
    ULONG Flags = Context->AMD64_EFlags;
    BOOLEAN Cf = !!(Flags & FLAG_CF), Zf = !!(Flags & FLAG_ZF);
    BOOLEAN Sf = !!(Flags & FLAG_SF), Of = !!(Flags & FLAG_OF), Pf = !!(Flags & FLAG_PF);
    BOOLEAN Result;

    switch (Code >> 1)
    {
        case 0: Result = Of; break;
        case 1: Result = Cf; break;
        case 2: Result = Zf; break;
        case 3: Result = Cf || Zf; break;
        case 4: Result = Sf; break;
        case 5: Result = Pf; break;
        case 6: Result = Sf != Of; break;
        default: Result = Zf || (Sf != Of); break;
    }

    return (Code & 1) ? !Result : Result;
}

static
VOID
Push(
    _Inout_ PARM64EC_NT_CONTEXT Context,
    _In_ ULONG64 Value)
{
    Context->Sp -= 8;
    *(PULONG64)Context->Sp = Value;
}

static
ULONG64
Pop(
    _Inout_ PARM64EC_NT_CONTEXT Context)
{
    ULONG64 Value = *(PULONG64)Context->Sp;
    Context->Sp += 8;
    return Value;
}

static
ULONG64
Shift(
    _Inout_ PARM64EC_NT_CONTEXT Context,
    _In_ UCHAR Op,
    _In_ ULONG64 Value,
    _In_ UCHAR Count,
    _In_ UCHAR Size)
{
    ULONG64 Mask = SizeMask(Size);
    UCHAR Bits = Size * 8;
    ULONG64 Result = Value & Mask;
    ULONG Flags = 0;

    Count &= (Size == 8) ? 63 : 31;
    if (!Count)
        return Result;

    switch (Op)
    {
        case 0:
            Count %= Bits;
            Result = ((Result << Count) | (Result >> ((Bits - Count) % Bits))) & Mask;
            if (Result & 1) Flags |= FLAG_CF;
            SetFlags(Context, FLAG_CF, Flags);
            return Result;

        case 1:
            Count %= Bits;
            Result = ((Result >> Count) | (Result << ((Bits - Count) % Bits))) & Mask;
            if (Result & SignBit(Size)) Flags |= FLAG_CF;
            SetFlags(Context, FLAG_CF, Flags);
            return Result;

        case 4:
        case 6:
            if ((Value >> (Bits - Count)) & 1) Flags |= FLAG_CF;
            Result = (Value << Count) & Mask;
            break;

        case 5:
            if ((Value >> (Count - 1)) & 1) Flags |= FLAG_CF;
            Result = (Value & Mask) >> Count;
            break;

        default:
            if (((ULONG64)SignExtend(Value, Size) >> (Count - 1)) & 1) Flags |= FLAG_CF;
            Result = (ULONG64)(SignExtend(Value, Size) >> Count) & Mask;
            break;
    }

    if (!Result) Flags |= FLAG_ZF;
    if (Result & SignBit(Size)) Flags |= FLAG_SF;
    if (Parity(Result)) Flags |= FLAG_PF;
    SetFlags(Context, FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_SF, Flags);
    return Result;
}

/* 128 by 64 bit unsigned division, the quotient has to fit in 64 bits */
static
ULONG64
Divide128(
    _In_ ULONG64 High,
    _In_ ULONG64 Low,
    _In_ ULONG64 Divisor,
    _Out_ PULONG64 Remainder)
{
    ULONG64 Quotient = 0;
    int i;

    for (i = 63; i >= 0; i--)
    {
        BOOLEAN Carry = (High >> 63) != 0;
        High = (High << 1) | (Low >> 63);
        Low <<= 1;
        Quotient <<= 1;
        if (Carry || High >= Divisor)
        {
            High -= Divisor;
            Quotient |= 1;
        }
    }

    *Remainder = High;
    return Quotient;
}

/* Continues execution at Target, leaving the interpreter when that is native code */
static
BOOLEAN
Branch(
    _Inout_ PXTA_THREAD Thread,
    _In_ ULONG64 Target)
{
    Thread->CpuArea->ContextAmd64->Pc = Target;
    return RtlIsEcCode(Target);
}

static
VOID
Unsupported(
    _In_ PXTA_INSN Insn)
{
    PUCHAR Bytes = (PUCHAR)Insn->Start;

    DbgPrint("XTAJIT64: unsupported x64 instruction at %p: %02x %02x %02x %02x %02x %02x\n",
             (PVOID)Insn->Start, Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], Bytes[5]);
    NtTerminateProcess(NtCurrentProcess(), STATUS_ILLEGAL_INSTRUCTION);
}

/* Executes one two byte opcode, returns TRUE when control went to native code */
static
BOOLEAN
Execute0F(
    _Inout_ PXTA_THREAD Thread,
    _Inout_ PXTA_INSN Insn)
{
    PARM64EC_NT_CONTEXT Context = Thread->CpuArea->ContextAmd64;
    UCHAR Op = *(PUCHAR)Insn->Next++;
    UCHAR Size = Insn->Size;
    ULONG64 Value;

    /* Jcc rel32 */
    if (Op >= 0x80 && Op <= 0x8F)
    {
        LONG Rel = (LONG)FetchImm(Insn, 4);
        Context->Pc = Insn->Next;
        if (Condition(Context, Op & 0xF))
            return Branch(Thread, Insn->Next + Rel);
        return FALSE;
    }

    /* CMOVcc */
    if (Op >= 0x40 && Op <= 0x4F)
    {
        DecodeModRm(Thread, Insn, 0);
        Value = ReadRm(Thread, Insn, Size);
        if (Condition(Context, Op & 0xF))
            WriteReg(Context, Insn, Insn->Reg, Size, Value);
        else if (Size == 4)
            WriteReg(Context, Insn, Insn->Reg, 4, ReadReg(Context, Insn, Insn->Reg, 4));
        Context->Pc = Insn->Next;
        return FALSE;
    }

    /* SETcc */
    if (Op >= 0x90 && Op <= 0x9F)
    {
        DecodeModRm(Thread, Insn, 0);
        WriteRm(Thread, Insn, 1, Condition(Context, Op & 0xF));
        Context->Pc = Insn->Next;
        return FALSE;
    }

    switch (Op)
    {
        /* NOP r/m */
        case 0x1F:
            DecodeModRm(Thread, Insn, 0);
            break;

        /* MOVUPS, MOVUPD, MOVSD, MOVSS loads and stores */
        case 0x10:
        case 0x11:
        {
            UCHAR Width = Insn->RepE ? 4 : (Insn->RepNE ? 8 : 16);
            PUCHAR Xmm;

            DecodeModRm(Thread, Insn, 0);
            Xmm = (PUCHAR)&Context->V[Insn->Reg];
            if (Op == 0x10)
            {
                if (Insn->IsMem && Width < 16)
                    RtlZeroMemory(Xmm, 16);
                RtlCopyMemory(Xmm, Insn->IsMem ? (PVOID)Insn->Address : (PVOID)&Context->V[Insn->RmReg], Width);
            }
            else
            {
                RtlCopyMemory(Insn->IsMem ? (PVOID)Insn->Address : (PVOID)&Context->V[Insn->RmReg], Xmm, Width);
            }
            break;
        }

        /* MOVAPS and MOVDQA/MOVDQU, the same move for an interpreter */
        case 0x28:
        case 0x6F:
            DecodeModRm(Thread, Insn, 0);
            RtlCopyMemory(&Context->V[Insn->Reg], Insn->IsMem ? (PVOID)Insn->Address : (PVOID)&Context->V[Insn->RmReg], 16);
            break;

        case 0x29:
        case 0x7F:
            DecodeModRm(Thread, Insn, 0);
            RtlCopyMemory(Insn->IsMem ? (PVOID)Insn->Address : (PVOID)&Context->V[Insn->RmReg], &Context->V[Insn->Reg], 16);
            break;

        /* XORPS and PXOR */
        case 0x57:
        case 0xEF:
        {
            PULONG64 Dest, Src;
            DecodeModRm(Thread, Insn, 0);
            Dest = (PULONG64)&Context->V[Insn->Reg];
            Src = Insn->IsMem ? (PULONG64)Insn->Address : (PULONG64)&Context->V[Insn->RmReg];
            Dest[0] ^= Src[0];
            Dest[1] ^= Src[1];
            break;
        }

        /* MOVD and MOVQ to an XMM register */
        case 0x6E:
            DecodeModRm(Thread, Insn, 0);
            RtlZeroMemory(&Context->V[Insn->Reg], 16);
            Value = ReadRm(Thread, Insn, (Insn->Rex & 8) ? 8 : 4);
            RtlCopyMemory(&Context->V[Insn->Reg], &Value, 8);
            break;

        /* MOVD and MOVQ from an XMM register, or MOVQ xmm, xmm/m64 with F3 */
        case 0x7E:
            DecodeModRm(Thread, Insn, 0);
            if (Insn->RepE)
            {
                Value = Insn->IsMem ? ReadMem(Insn->Address, 8) : *(PULONG64)&Context->V[Insn->RmReg];
                RtlZeroMemory(&Context->V[Insn->Reg], 16);
                RtlCopyMemory(&Context->V[Insn->Reg], &Value, 8);
            }
            else
            {
                RtlCopyMemory(&Value, &Context->V[Insn->Reg], 8);
                WriteRm(Thread, Insn, (Insn->Rex & 8) ? 8 : 4, Value);
            }
            break;

        /* MOVQ xmm/m64, xmm */
        case 0xD6:
            DecodeModRm(Thread, Insn, 0);
            RtlCopyMemory(&Value, &Context->V[Insn->Reg], 8);
            if (Insn->IsMem)
                WriteMem(Insn->Address, 8, Value);
            else
            {
                RtlZeroMemory(&Context->V[Insn->RmReg], 16);
                RtlCopyMemory(&Context->V[Insn->RmReg], &Value, 8);
            }
            break;

        /* CPUID, report a plain x64 processor */
        case 0xA2:
        {
            PULONG64 Regs = Gpr(Context);
            ULONG Leaf = (ULONG)Regs[REG_RAX];
            Regs[REG_RAX] = (Leaf == 0) ? 1 : 0;
            Regs[REG_RBX] = (Leaf == 0) ? 0x756E6547 : 0;
            Regs[REG_RDX] = (Leaf == 0) ? 0x49656E69 : ((Leaf == 1) ? 0x06808001 : 0);
            Regs[REG_RCX] = (Leaf == 0) ? 0x6C65746E : ((Leaf == 1) ? 0x00000001 : 0);
            break;
        }

        /* IMUL r, r/m */
        case 0xAF:
            DecodeModRm(Thread, Insn, 0);
            Value = (ULONG64)(SignExtend(ReadReg(Context, Insn, Insn->Reg, Size), Size) *
                              SignExtend(ReadRm(Thread, Insn, Size), Size));
            WriteReg(Context, Insn, Insn->Reg, Size, Value);
            break;

        /* CMPXCHG */
        case 0xB0:
        case 0xB1:
        {
            UCHAR OpSize = (Op == 0xB0) ? 1 : Size;
            ULONG64 Current;
            DecodeModRm(Thread, Insn, 0);
            Current = ReadRm(Thread, Insn, OpSize);
            Alu(Context, 7, ReadReg(Context, Insn, REG_RAX, OpSize), Current, OpSize);
            if (Context->AMD64_EFlags & FLAG_ZF)
                WriteRm(Thread, Insn, OpSize, ReadReg(Context, Insn, Insn->Reg, OpSize));
            else
                WriteReg(Context, Insn, REG_RAX, OpSize, Current);
            break;
        }

        /* MOVZX and MOVSX */
        case 0xB6:
        case 0xB7:
        case 0xBE:
        case 0xBF:
        {
            UCHAR From = (Op & 1) ? 2 : 1;
            DecodeModRm(Thread, Insn, 0);
            Value = ReadRm(Thread, Insn, From);
            if (Op >= 0xBE)
                Value = (ULONG64)SignExtend(Value, From);
            WriteReg(Context, Insn, Insn->Reg, Size, Value);
            break;
        }

        /* XADD */
        case 0xC0:
        case 0xC1:
        {
            UCHAR OpSize = (Op == 0xC0) ? 1 : Size;
            ULONG64 Dest, Src;
            DecodeModRm(Thread, Insn, 0);
            Dest = ReadRm(Thread, Insn, OpSize);
            Src = ReadReg(Context, Insn, Insn->Reg, OpSize);
            WriteRm(Thread, Insn, OpSize, Alu(Context, 0, Dest, Src, OpSize));
            WriteReg(Context, Insn, Insn->Reg, OpSize, Dest);
            break;
        }

        /* BSWAP */
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        {
            UCHAR Reg = (Op & 7) | ((Insn->Rex & 1) << 3);
            Value = ReadReg(Context, Insn, Reg, Size);
            Value = (Size == 8) ? _byteswap_uint64(Value) : _byteswap_ulong((ULONG)Value);
            WriteReg(Context, Insn, Reg, Size, Value);
            break;
        }

        default:
            Unsupported(Insn);
            return TRUE;
    }

    Context->Pc = Insn->Next;
    return FALSE;
}

/* Executes one instruction, returns TRUE when control went to native code */
static
BOOLEAN
Step(
    _Inout_ PXTA_THREAD Thread)
{
    PARM64EC_NT_CONTEXT Context = Thread->CpuArea->ContextAmd64;
    PULONG64 Regs = Gpr(Context);
    XTA_INSN Insn = { 0 };
    UCHAR Op, Size;
    ULONG64 Value;

    Insn.Start = Insn.Next = Context->Pc;

    /* Legacy prefixes, then REX */
    for (;;)
    {
        Op = *(PUCHAR)Insn.Next;
        if (Op == 0x66) Insn.OpSize16 = TRUE;
        else if (Op == 0xF3) Insn.RepE = TRUE;
        else if (Op == 0xF2) Insn.RepNE = TRUE;
        else if (Op == 0x65) Insn.SegGs = TRUE;
        else if (Op != 0xF0 && Op != 0x2E && Op != 0x3E && Op != 0x26 && Op != 0x36 && Op != 0x64) break;
        Insn.Next++;
    }
    if ((Op & 0xF0) == 0x40)
    {
        Insn.Rex = Op;
        Insn.Next++;
        Op = *(PUCHAR)Insn.Next;
    }
    Insn.Next++;

    Size = (Insn.Rex & 8) ? 8 : (Insn.OpSize16 ? 2 : 4);
    Insn.Size = Size;

    /* ALU r/m, r and r, r/m and accumulator forms */
    if (Op < 0x40 && (Op & 7) < 6)
    {
        UCHAR AluOp = Op >> 3;
        UCHAR Form = Op & 7;
        UCHAR OpSize = (Form & 1) ? Size : 1;
        ULONG64 Result;

        if (Form < 4)
        {
            DecodeModRm(Thread, &Insn, 0);
            if (Form & 2)
            {
                Result = Alu(Context, AluOp, ReadReg(Context, &Insn, Insn.Reg, OpSize), ReadRm(Thread, &Insn, OpSize), OpSize);
                if (AluOp != 7)
                    WriteReg(Context, &Insn, Insn.Reg, OpSize, Result);
            }
            else
            {
                Result = Alu(Context, AluOp, ReadRm(Thread, &Insn, OpSize), ReadReg(Context, &Insn, Insn.Reg, OpSize), OpSize);
                if (AluOp != 7)
                    WriteRm(Thread, &Insn, OpSize, Result);
            }
        }
        else
        {
            Value = (ULONG64)SignExtend(FetchImm(&Insn, (OpSize == 1) ? 1 : ((OpSize == 2) ? 2 : 4)), (OpSize == 1) ? 1 : ((OpSize == 2) ? 2 : 4));
            Result = Alu(Context, AluOp, ReadReg(Context, &Insn, REG_RAX, OpSize), Value, OpSize);
            if (AluOp != 7)
                WriteReg(Context, &Insn, REG_RAX, OpSize, Result);
        }

        Context->Pc = Insn.Next;
        return FALSE;
    }

    /* PUSH r, POP r */
    if (Op >= 0x50 && Op <= 0x5F)
    {
        UCHAR Reg = (Op & 7) | ((Insn.Rex & 1) << 3);
        if (Op < 0x58)
        {
            Push(Context, Regs[Reg]);
        }
        else
        {
            Regs[Reg] = Pop(Context);
        }
        Context->Pc = Insn.Next;
        return FALSE;
    }

    /* Jcc rel8 */
    if (Op >= 0x70 && Op <= 0x7F)
    {
        CHAR Rel = (CHAR)FetchImm(&Insn, 1);
        Context->Pc = Insn.Next;
        if (Condition(Context, Op & 0xF))
            return Branch(Thread, Insn.Next + Rel);
        return FALSE;
    }

    /* MOV r8, imm8 and MOV r, imm */
    if (Op >= 0xB0 && Op <= 0xBF)
    {
        UCHAR Reg = (Op & 7) | ((Insn.Rex & 1) << 3);
        if (Op < 0xB8)
            WriteReg(Context, &Insn, Reg, 1, FetchImm(&Insn, 1));
        else
            WriteReg(Context, &Insn, Reg, Size, FetchImm(&Insn, (Size == 8) ? 8 : Size));
        Context->Pc = Insn.Next;
        return FALSE;
    }

    /* XCHG rAX, r */
    if (Op >= 0x91 && Op <= 0x97)
    {
        UCHAR Reg = (Op & 7) | ((Insn.Rex & 1) << 3);
        Value = ReadReg(Context, &Insn, Reg, Size);
        WriteReg(Context, &Insn, Reg, Size, ReadReg(Context, &Insn, REG_RAX, Size));
        WriteReg(Context, &Insn, REG_RAX, Size, Value);
        Context->Pc = Insn.Next;
        return FALSE;
    }

    switch (Op)
    {
        case 0x0F:
            return Execute0F(Thread, &Insn);

        /* MOVSXD */
        case 0x63:
            DecodeModRm(Thread, &Insn, 0);
            WriteReg(Context, &Insn, Insn.Reg, Size, (ULONG64)(LONG64)(LONG)ReadRm(Thread, &Insn, 4));
            break;

        /* PUSH imm */
        case 0x68:
            Push(Context, (ULONG64)(LONG64)(LONG)FetchImm(&Insn, 4));
            break;
        case 0x6A:
            Push(Context, (ULONG64)(LONG64)(CHAR)FetchImm(&Insn, 1));
            break;

        /* IMUL r, r/m, imm */
        case 0x69:
        case 0x6B:
        {
            UCHAR ImmSize = (Op == 0x6B) ? 1 : ((Size == 2) ? 2 : 4);
            LONG64 Left;
            DecodeModRm(Thread, &Insn, ImmSize);
            Left = SignExtend(ReadRm(Thread, &Insn, Size), Size);
            Value = (ULONG64)(Left * SignExtend(FetchImm(&Insn, ImmSize), ImmSize));
            WriteReg(Context, &Insn, Insn.Reg, Size, Value);
            break;
        }

        /* Group 1, ALU r/m, imm */
        case 0x80:
        case 0x81:
        case 0x83:
        {
            UCHAR OpSize = (Op == 0x80) ? 1 : Size;
            UCHAR ImmSize = (Op == 0x81) ? ((Size == 2) ? 2 : 4) : 1;
            ULONG64 Result;

            DecodeModRm(Thread, &Insn, ImmSize);
            Value = (ULONG64)SignExtend(FetchImm(&Insn, ImmSize), ImmSize);
            Result = Alu(Context, Insn.Reg & 7, ReadRm(Thread, &Insn, OpSize), Value, OpSize);
            if ((Insn.Reg & 7) != 7)
                WriteRm(Thread, &Insn, OpSize, Result);
            break;
        }

        /* TEST r/m, r */
        case 0x84:
        case 0x85:
        {
            UCHAR OpSize = (Op == 0x84) ? 1 : Size;
            DecodeModRm(Thread, &Insn, 0);
            FlagsLogic(Context, ReadRm(Thread, &Insn, OpSize) & ReadReg(Context, &Insn, Insn.Reg, OpSize), OpSize);
            break;
        }

        /* XCHG r/m, r */
        case 0x86:
        case 0x87:
        {
            UCHAR OpSize = (Op == 0x86) ? 1 : Size;
            DecodeModRm(Thread, &Insn, 0);
            Value = ReadRm(Thread, &Insn, OpSize);
            WriteRm(Thread, &Insn, OpSize, ReadReg(Context, &Insn, Insn.Reg, OpSize));
            WriteReg(Context, &Insn, Insn.Reg, OpSize, Value);
            break;
        }

        /* MOV */
        case 0x88:
            DecodeModRm(Thread, &Insn, 0);
            WriteRm(Thread, &Insn, 1, ReadReg(Context, &Insn, Insn.Reg, 1));
            break;
        case 0x89:
            DecodeModRm(Thread, &Insn, 0);
            WriteRm(Thread, &Insn, Size, ReadReg(Context, &Insn, Insn.Reg, Size));
            break;
        case 0x8A:
            DecodeModRm(Thread, &Insn, 0);
            WriteReg(Context, &Insn, Insn.Reg, 1, ReadRm(Thread, &Insn, 1));
            break;
        case 0x8B:
            DecodeModRm(Thread, &Insn, 0);
            WriteReg(Context, &Insn, Insn.Reg, Size, ReadRm(Thread, &Insn, Size));
            break;

        /* LEA, the segment does not apply */
        case 0x8D:
            Insn.SegGs = FALSE;
            DecodeModRm(Thread, &Insn, 0);
            WriteReg(Context, &Insn, Insn.Reg, Size, Insn.Address);
            break;

        /* POP r/m */
        case 0x8F:
            DecodeModRm(Thread, &Insn, 0);
            WriteRm(Thread, &Insn, 8, Pop(Context));
            break;

        case 0x90:
            break;

        /* CWDE, CDQE */
        case 0x98:
            if (Size == 8)
                Regs[REG_RAX] = (ULONG64)(LONG64)(LONG)Regs[REG_RAX];
            else
                WriteReg(Context, &Insn, REG_RAX, Size, (ULONG64)SignExtend(Regs[REG_RAX], Size / 2));
            break;

        /* CDQ, CQO */
        case 0x99:
            WriteReg(Context, &Insn, REG_RDX, Size, (Regs[REG_RAX] & SignBit(Size)) ? ~0ULL : 0);
            break;

        /* TEST accumulator, imm */
        case 0xA8:
            FlagsLogic(Context, Regs[REG_RAX] & FetchImm(&Insn, 1), 1);
            break;
        case 0xA9:
            FlagsLogic(Context, Regs[REG_RAX] & (ULONG64)SignExtend(FetchImm(&Insn, (Size == 2) ? 2 : 4), (Size == 2) ? 2 : 4), Size);
            break;

        /* MOVS and STOS, with REP */
        case 0xA4:
        case 0xA5:
        case 0xAA:
        case 0xAB:
        {
            UCHAR OpSize = (Op & 1) ? Size : 1;
            LONG64 Step = (Context->AMD64_EFlags & FLAG_DF) ? -(LONG64)OpSize : OpSize;
            ULONG64 Count = Insn.RepE ? Regs[REG_RCX] : 1;

            while (Count--)
            {
                if (Op <= 0xA5)
                {
                    WriteMem(Regs[REG_RDI], OpSize, ReadMem(Regs[REG_RSI], OpSize));
                    Regs[REG_RSI] += Step;
                }
                else
                {
                    WriteMem(Regs[REG_RDI], OpSize, Regs[REG_RAX]);
                }
                Regs[REG_RDI] += Step;
            }
            if (Insn.RepE)
                Regs[REG_RCX] = 0;
            break;
        }

        /* Group 2, shifts by imm8, 1 and CL */
        case 0xC0:
        case 0xC1:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        {
            UCHAR OpSize = (Op & 1) ? Size : 1;
            UCHAR Count;
            DecodeModRm(Thread, &Insn, (Op <= 0xC1) ? 1 : 0);
            if (Op <= 0xC1)
                Count = (UCHAR)FetchImm(&Insn, 1);
            else if (Op <= 0xD1)
                Count = 1;
            else
                Count = (UCHAR)Regs[REG_RCX];
            WriteRm(Thread, &Insn, OpSize, Shift(Context, Insn.Reg & 7, ReadRm(Thread, &Insn, OpSize), Count, OpSize));
            break;
        }

        /* RET */
        case 0xC2:
        {
            USHORT Release = (USHORT)FetchImm(&Insn, 2);
            Value = Pop(Context);
            Context->Sp += Release;
            return Branch(Thread, Value);
        }
        case 0xC3:
            return Branch(Thread, Pop(Context));

        /* MOV r/m, imm */
        case 0xC6:
            DecodeModRm(Thread, &Insn, 1);
            WriteRm(Thread, &Insn, 1, FetchImm(&Insn, 1));
            break;
        case 0xC7:
        {
            UCHAR ImmSize = (Size == 2) ? 2 : 4;
            DecodeModRm(Thread, &Insn, ImmSize);
            WriteRm(Thread, &Insn, Size, (ULONG64)SignExtend(FetchImm(&Insn, ImmSize), ImmSize));
            break;
        }

        /* INT3 and the fast fail interrupt */
        case 0xCC:
            DbgPrint("XTAJIT64: breakpoint at %p\n", (PVOID)Insn.Start);
            NtTerminateProcess(NtCurrentProcess(), STATUS_BREAKPOINT);
            return TRUE;
        case 0xCD:
            DbgPrint("XTAJIT64: interrupt %02x at %p\n", *(PUCHAR)Insn.Next, (PVOID)Insn.Start);
            NtTerminateProcess(NtCurrentProcess(), STATUS_STACK_BUFFER_OVERRUN);
            return TRUE;

        /* CALL rel32, JMP rel32, JMP rel8 */
        case 0xE8:
        {
            LONG Rel = (LONG)FetchImm(&Insn, 4);
            Push(Context, Insn.Next);
            return Branch(Thread, Insn.Next + Rel);
        }
        case 0xE9:
        {
            LONG Rel = (LONG)FetchImm(&Insn, 4);
            return Branch(Thread, Insn.Next + Rel);
        }
        case 0xEB:
        {
            CHAR Rel = (CHAR)FetchImm(&Insn, 1);
            return Branch(Thread, Insn.Next + Rel);
        }

        /* Group 3 */
        case 0xF6:
        case 0xF7:
        {
            UCHAR OpSize = (Op == 0xF6) ? 1 : Size;
            UCHAR Sub;
            DecodeModRm(Thread, &Insn, 0);
            Sub = Insn.Reg & 7;
            if (Sub < 2)
            {
                UCHAR ImmSize = (OpSize == 1) ? 1 : ((OpSize == 2) ? 2 : 4);
                /* The immediate follows the displacement, RIP relative operands need its size */
                if (Insn.IsMem && (Insn.ModRm & 0xC7) == 0x05)
                    Insn.Address += ImmSize;
                Value = (ULONG64)SignExtend(FetchImm(&Insn, ImmSize), ImmSize);
                FlagsLogic(Context, ReadRm(Thread, &Insn, OpSize) & Value, OpSize);
            }
            else if (Sub == 2)
            {
                WriteRm(Thread, &Insn, OpSize, ~ReadRm(Thread, &Insn, OpSize));
            }
            else if (Sub == 3)
            {
                Value = ReadRm(Thread, &Insn, OpSize);
                WriteRm(Thread, &Insn, OpSize, Alu(Context, 5, 0, Value, OpSize));
            }
            else if (OpSize == 8 && Sub < 6)
            {
                ULONG64 Operand = ReadRm(Thread, &Insn, 8);
                ULONG64 High = (Sub == 4) ? __umulh(Regs[REG_RAX], Operand) :
                                            (ULONG64)__mulh((LONG64)Regs[REG_RAX], (LONG64)Operand);
                Regs[REG_RAX] = Regs[REG_RAX] * Operand;
                Regs[REG_RDX] = High;
            }
            else if (OpSize == 8)
            {
                ULONG64 Divisor = ReadRm(Thread, &Insn, 8);
                if (!Divisor)
                {
                    DbgPrint("XTAJIT64: division by zero at %p\n", (PVOID)Insn.Start);
                    NtTerminateProcess(NtCurrentProcess(), STATUS_INTEGER_DIVIDE_BY_ZERO);
                    return TRUE;
                }
                if (Sub == 6)
                {
                    ULONG64 Remainder;
                    Value = Divide128(Regs[REG_RDX], Regs[REG_RAX], Divisor, &Remainder);
                    Regs[REG_RAX] = Value;
                    Regs[REG_RDX] = Remainder;
                }
                else
                {
                    /* Divide the magnitudes, then fix up the signs */
                    BOOLEAN Negative = (Regs[REG_RDX] >> 63) != 0;
                    BOOLEAN DivisorNegative = (Divisor >> 63) != 0;
                    ULONG64 High = Regs[REG_RDX], Low = Regs[REG_RAX], Remainder;

                    if (Negative)
                    {
                        Low = ~Low + 1;
                        High = ~High + (Low == 0);
                    }
                    if (DivisorNegative)
                        Divisor = ~Divisor + 1;

                    Value = Divide128(High, Low, Divisor, &Remainder);
                    Regs[REG_RAX] = (Negative != DivisorNegative) ? ~Value + 1 : Value;
                    Regs[REG_RDX] = Negative ? ~Remainder + 1 : Remainder;
                }
            }
            else
            {
                /* Narrower multiply and divide work on RDX:RAX pieces of OpSize */
                ULONG64 Low = ReadReg(Context, &Insn, REG_RAX, OpSize);
                ULONG64 Operand = ReadRm(Thread, &Insn, OpSize);
                ULONG64 Mask = SizeMask(OpSize);
                UCHAR Bits = OpSize * 8;

                if (Sub == 4 || Sub == 5)
                {
                    ULONG64 Product = (Sub == 4) ? Low * Operand :
                                      (ULONG64)(SignExtend(Low, OpSize) * SignExtend(Operand, OpSize));
                    if (OpSize == 1)
                    {
                        WriteReg(Context, &Insn, REG_RAX, 2, Product);
                    }
                    else
                    {
                        WriteReg(Context, &Insn, REG_RAX, OpSize, Product & Mask);
                        WriteReg(Context, &Insn, REG_RDX, OpSize, (Product >> Bits) & Mask);
                    }
                }
                else
                {
                    ULONG64 Dividend = (OpSize == 1) ? (Regs[REG_RAX] & 0xFFFF) :
                                       ((ReadReg(Context, &Insn, REG_RDX, OpSize) << Bits) | Low);
                    if (!Operand)
                    {
                        DbgPrint("XTAJIT64: division by zero at %p\n", (PVOID)Insn.Start);
                        NtTerminateProcess(NtCurrentProcess(), STATUS_INTEGER_DIVIDE_BY_ZERO);
                        return TRUE;
                    }
                    if (Sub == 6)
                    {
                        Low = Dividend / Operand;
                        Value = Dividend % Operand;
                    }
                    else
                    {
                        LONG64 SignedDividend = SignExtend(Dividend, OpSize * 2);
                        Low = (ULONG64)(SignedDividend / SignExtend(Operand, OpSize));
                        Value = (ULONG64)(SignedDividend % SignExtend(Operand, OpSize));
                    }
                    if (OpSize == 1)
                    {
                        WriteReg(Context, &Insn, REG_RAX, 2, (Low & 0xFF) | ((Value & 0xFF) << 8));
                    }
                    else
                    {
                        WriteReg(Context, &Insn, REG_RAX, OpSize, Low);
                        WriteReg(Context, &Insn, REG_RDX, OpSize, Value);
                    }
                }
            }
            break;
        }

        /* CLD, STD */
        case 0xFC:
            Context->AMD64_EFlags &= ~FLAG_DF;
            break;
        case 0xFD:
            Context->AMD64_EFlags |= FLAG_DF;
            break;

        /* Groups 4 and 5 */
        case 0xFE:
        case 0xFF:
        {
            UCHAR OpSize = (Op == 0xFE) ? 1 : Size;
            UCHAR Sub;
            ULONG Carry;

            DecodeModRm(Thread, &Insn, 0);
            Sub = Insn.Reg & 7;
            switch (Sub)
            {
                /* INC and DEC leave CF alone */
                case 0:
                case 1:
                    Carry = Context->AMD64_EFlags & FLAG_CF;
                    Value = Alu(Context, Sub ? 5 : 0, ReadRm(Thread, &Insn, OpSize), 1, OpSize);
                    SetFlags(Context, FLAG_CF, Carry);
                    WriteRm(Thread, &Insn, OpSize, Value);
                    break;

                case 2:
                    Value = ReadRm(Thread, &Insn, 8);
                    Push(Context, Insn.Next);
                    return Branch(Thread, Value);

                case 4:
                    return Branch(Thread, ReadRm(Thread, &Insn, 8));

                case 6:
                    Push(Context, ReadRm(Thread, &Insn, 8));
                    break;

                default:
                    Unsupported(&Insn);
                    return TRUE;
            }
            break;
        }

        default:
            Unsupported(&Insn);
            return TRUE;
    }

    Context->Pc = Insn.Next;
    return FALSE;
}

/**
 * @brief
 * Runs x64 code until it transfers control to ARM64 or ARM64EC code.
 *
 * @return
 * Whether the x64 code called native code or returned to it.
 */
XTA_EXIT
NTAPI
XtapInterpret(
    _Inout_ PXTA_THREAD Thread)
{
    PARM64EC_NT_CONTEXT Context = Thread->CpuArea->ContextAmd64;

    for (;;)
    {
        if (!Step(Thread))
            continue;

        /* Native code may come straight back to native code, run it until x64 code is next */
        do
        {
            /* Returning to the native code that entered this run ends it */
            if (Context->Pc == Thread->ReturnPc && Context->Sp == Thread->ReturnSp)
                return XtaExitReturnNative;

#if DBG
            DPRINT1("xtajit64: x64 calls %p (rcx %p, rdx %p)\n", (PVOID)Context->Pc, (PVOID)Context->X0, (PVOID)Context->X1);
#endif
            Thread->NativeTarget = Context->Pc;
            XtapRunNative(Thread);
        } while (RtlIsEcCode(Context->Pc));
    }
}

/* EOF */
