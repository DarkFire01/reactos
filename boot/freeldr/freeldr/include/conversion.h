/*
 * PROJECT:         EFI Windows Loader
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            boot/freeldr/freeldr/windows/conversion.c
 * PURPOSE:         Physical <-> Virtual addressing mode conversions (arch-specific)
 * PROGRAMMERS:     Aleksey Bragin (aleksey@reactos.org)
 */

#pragma once

/* Arch-specific addresses translation implementation */
FORCEINLINE
PVOID
VaToPa(PVOID Va)
{
    return (PVOID)((ULONG_PTR)Va);
}

FORCEINLINE
PVOID
PaToVa(PVOID Pa)
{
    return (PVOID)((ULONG_PTR)Pa);
}
