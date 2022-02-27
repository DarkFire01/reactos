/*
 * PROJECT:     ROSUEFI
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROSEFI Utility File
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#include "include/rosefip.h"

/* ROSEFI Utils  *********************************************/

int 
memcmp(const void* aptr, const void* bptr, size_t n){
	const unsigned char* a = aptr, *b = bptr;
	for (size_t i = 0; i < n; i++){
		if (a[i] < b[i]) return -1;
		else if (a[i] > b[i]) return 1;
	}
	return 0;
}

PUCHAR hex_to_str(PUCHAR s, UINT32 v) {
    char *end, *p;

    if (v == 0) {
        *s = '0';
        s++;

        *s = 0;
        return s;
    }

    end = s;

    {
        UINT32 n = v;

        while (n != 0) {
            end++;
            n >>= 4;
        }
    }

    *end = 0;

    p = end;

    while (v != 0) {
        p = &p[-1];

        if ((v & 0xf) >= 10)
            *p = (v & 0xf) - 10 + 'a';
        else
            *p = (v & 0xf) + '0';

        v >>= 4;
    }

    return end;
}


VOID
RefiItoa(unsigned long int n, unsigned short int* buffer, int basenumber)
{
	unsigned long int hold;
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

#if 0
ULONG32 
RefiStrlen(UCHAR16* str)
{
	const char* strCount = str;

	while (*strCount++);
	return strCount - str - 1;
}

#endif
/* UEFI Specific *********************************************/
VOID
RefiStallProcessor(EFI_SYSTEM_TABLE* SystemTable, UINTN d)
{
    SystemTable->BootServices->Stall(d * 1000);
}