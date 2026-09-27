/*
 * Copyright 2023 Christopher S. Denton
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "ntsecapi.h"
#include "guiddef.h"

BOOL WINAPI ProcessPrng(BYTE *data, SIZE_T size)
{
    return RtlGenRandom(data, size);
}

/**
 * @brief Draws a version 4 random GUID from the process PRNG.
 *
 * @param[out] Guid Receives the generated value.
 *
 * @return TRUE on success.
 */
BOOL
WINAPI
ProcessPrngGuid(
    _Out_ GUID *Guid)
{
    PUCHAR Bytes = (PUCHAR)Guid;

    if (!ProcessPrng(Bytes, sizeof(*Guid)))
        return FALSE;

    /* Variant 1, then version 4 */
    Bytes[8] = (Bytes[8] & 0x3F) | 0x80;
    Guid->Data3 = (Guid->Data3 & 0x0FFF) | 0x4000;
    return TRUE;
}
