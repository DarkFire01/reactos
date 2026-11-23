/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI generic address access, FFH registers are MSRs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/* Width of the smallest access that covers BitWidth + BitOffset */
static
UCHAR
NTAPI
PpmLibCoveringAccess(
    _In_ PPPMLIB_GAS Register)
{
    ULONG Bits = (ULONG)Register->BitWidth + Register->BitOffset;

    if (Bits <= 8)
        return 8;
    if (Bits <= 16)
        return 16;
    if (Bits <= 32)
        return 32;
    return 64;
}

/**
 * @brief Converts the ACPI access size code into a width in bits.
 *
 * @remarks A value that already is a width is kept, an unknown code is
 * derived from the field size. PCC registers are left alone.
 */
VOID
NTAPI
PpmLibTranslateAccessSize(
    _Inout_ PPPMLIB_GAS Register)
{
    switch (Register->AccessBits)
    {
        case 8:
        case 16:
        case 32:
        case 64:
            return;

        case 1:
        case 2:
        case 3:
        case 4:
            if (Register->SpaceId != PPMLIB_SPACE_PCC)
                Register->AccessBits = (UCHAR)(4 << Register->AccessBits);
            return;

        default:
            if (Register->SpaceId != PPMLIB_SPACE_PCC)
                Register->AccessBits = PpmLibCoveringAccess(Register);
            return;
    }
}

/**
 * @brief Checks that a register can be used for a PM block or a state register.
 */
BOOLEAN
NTAPI
PpmLibIsValidRegister(
    _In_ PPPMLIB_GAS Register)
{
    switch (Register->SpaceId)
    {
        case PPMLIB_SPACE_IO:
            if (Register->Address == 0 || Register->Address > 0xFFFF)
                return FALSE;
            /* Fall through */
        case PPMLIB_SPACE_MEMORY:
            return (Register->BitWidth != 0 &&
                    (ULONG)Register->BitWidth + Register->BitOffset <= Register->AccessBits);

        case 2:
        case 3:
        case 4:
        case PPMLIB_SPACE_PCC:
        case PPMLIB_SPACE_FFH:
            return TRUE;

        default:
            return FALSE;
    }
}

/**
 * @brief TRUE unless the register is the all zero null register.
 */
BOOLEAN
NTAPI
PpmLibRegisterPresent(
    _In_ PPPMLIB_GAS Register)
{
    return (Register->SpaceId != PPMLIB_SPACE_MEMORY || Register->Address != 0);
}

/**
 * @brief Reads the whole register without applying the bit field.
 *
 * @remarks System memory and PCC registers have no path here and read as 0.
 */
ULONG64
NTAPI
PpmLibReadRaw(
    _In_ PPPMLIB_GAS Register)
{
    if (Register->SpaceId == PPMLIB_SPACE_IO)
    {
        switch (Register->AccessBits)
        {
            case 8:
                return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)Register->Address);
            case 16:
                return READ_PORT_USHORT((PUSHORT)(ULONG_PTR)Register->Address);
            case 32:
                return READ_PORT_ULONG((PULONG)(ULONG_PTR)Register->Address);
            default:
                return 0;
        }
    }

    if (Register->SpaceId == PPMLIB_SPACE_FFH && Register->AccessBits == 64)
        return __readmsr((ULONG)Register->Address);

    return 0;
}

/**
 * @brief Writes the whole register without applying the bit field.
 */
VOID
NTAPI
PpmLibWriteRaw(
    _In_ PPPMLIB_GAS Register,
    _In_ ULONG64 Value)
{
    if (Register->SpaceId == PPMLIB_SPACE_IO)
    {
        switch (Register->AccessBits)
        {
            case 8:
                WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Register->Address, (UCHAR)Value);
                break;
            case 16:
                WRITE_PORT_USHORT((PUSHORT)(ULONG_PTR)Register->Address, (USHORT)Value);
                break;
            case 32:
                WRITE_PORT_ULONG((PULONG)(ULONG_PTR)Register->Address, (ULONG)Value);
                break;
            default:
                break;
        }
        return;
    }

    if (Register->SpaceId == PPMLIB_SPACE_FFH && Register->AccessBits == 64)
        __writemsr((ULONG)Register->Address, Value);
}

/**
 * @brief Places Value into the bit field of Field inside Raw.
 */
ULONG64
NTAPI
PpmLibMergeField(
    _In_ PPPMLIB_GAS Field,
    _In_ ULONG64 Raw,
    _In_ ULONG64 Value)
{
    ULONG64 Mask;

    if (Field->BitWidth >= 64 && Field->BitOffset == 0)
        return Value;

    Mask = (Field->BitWidth >= 64) ? ~0ULL : ((1ULL << Field->BitWidth) - 1);
    Mask <<= Field->BitOffset;
    return (Raw & ~Mask) | ((Value << Field->BitOffset) & Mask);
}

/**
 * @brief Reads the bit field a register describes.
 */
ULONG64
NTAPI
PpmLibReadRegister(
    _In_opt_ PPPMLIB_GAS Register)
{
    ULONG64 Value;

    if (Register == NULL)
        return 0;

    if (Register->SpaceId == PPMLIB_SPACE_STATIC)
        return (ULONG)Register->Address;

    Value = PpmLibReadRaw(Register);
    if (Value == 0)
        return 0;

    Value >>= Register->BitOffset;
    if (Register->BitWidth < 64)
        Value &= (1ULL << Register->BitWidth) - 1;

    return Value;
}

/**
 * @brief Writes the bit field a register describes, keeping the other bits.
 */
VOID
NTAPI
PpmLibWriteRegister(
    _In_opt_ PPPMLIB_GAS Register,
    _In_ ULONG64 Value)
{
    UCHAR FullWidth;

    if (Register == NULL)
        return;

    FullWidth = (Register->SpaceId == PPMLIB_SPACE_PCC) ? 64 : Register->AccessBits;
    if (Register->BitOffset != 0 || Register->BitWidth != FullWidth)
        Value = PpmLibMergeField(Register, PpmLibReadRaw(Register), Value);

    PpmLibWriteRaw(Register, Value);
}

/**
 * @brief Reads APERF and MPERF, zeroing both when Reset is set.
 *
 * @remarks Both vendors use the architectural MSRs for this.
 */
VOID
NTAPI
PpmLibReadAperfMperf(
    _In_opt_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual)
{
    UNREFERENCED_PARAMETER(Context);

    *Actual = __readmsr(PPMLIB_MSR_APERF);
    *Reference = __readmsr(PPMLIB_MSR_MPERF);
    if (Reset)
    {
        __writemsr(PPMLIB_MSR_MPERF, 0);
        __writemsr(PPMLIB_MSR_APERF, 0);
    }
}

/* EOF */
