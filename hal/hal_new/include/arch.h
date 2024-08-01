#pragma once

#include "x86/halx86.h"

BOOLEAN
NTAPI
HalArchSetRealTimeClock(IN PTIME_FIELDS Time);

BOOLEAN
NTAPI
HalArchQueryRealTimeClock(OUT PTIME_FIELDS Time);

ARC_STATUS
NTAPI
HalArchSetEnvironmentVariable(IN PCH Name,
                              IN PCH Value);

ARC_STATUS
NTAPI
HalArchGetEnvironmentVariable(
    _In_ PCH Name,
    _In_ USHORT ValueLength,
    _Out_writes_z_(ValueLength) PCH Value);


VOID
NTAPI
HalpArchInitializePhase0(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock);

VOID
NTAPI
HalpArchInitializePhase1(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock);