/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI Utils
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

UINT32 
UefiStrlen(PUCHAR str)
{
	const char* strCount = str;

	while (*strCount++);
	return strCount - str - 1;
}

VOID
UefiItoa(ULONG64 n, CHAR16* buffer, int basenumber)
{
	ULONG64 hold;
	int i, j;
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