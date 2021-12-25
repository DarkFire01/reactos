/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Timing source file
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

VOID
RefiStallProcessor(EFI_SYSTEM_TABLE* SystemTable, UINTN d)
{
    SystemTable->BootServices->Stall(d * 1000);
}

VOID
RefiItoa(ULONG32 n, UINT16* buffer, UINT32 basenumber)
{
	ULONG32 hold;
	UINT32 i, j;
	hold = n;
	i = 0;

	do{
		hold = n % basenumber;
		buffer[i++] = (hold < 10) ? (hold + '0') : (hold + 'a' - 10);
	} while(n /= basenumber);
	buffer[i--] = 0;
	
	for(j = 0; j < i; j++, i--)
	{
		hold = buffer[j];
		buffer[j] = buffer[i];
		buffer[i] = hold;
	}
}