/*
 *  ReactOS kernel
 *  Copyright (C) 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Computer name functions
 * FILE:            dll/win32/kernel32/client/compname.c
 * PROGRAMERS:      Eric Kohl
 *                  Katayama Hirofumi MZ
 */

/* INCLUDES ******************************************************************/

#include <k32.h>
#include <windns.h>

#define NDEBUG
#include <debug.h>

typedef NTSTATUS (WINAPI *FN_DnsValidateName_W)(LPCWSTR, DNS_NAME_FORMAT);

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameA(LPSTR lpBuffer, LPDWORD lpnSize)
{
    BOOL ret;

    ret = GetComputerNameExA(ComputerNameNetBIOS, lpBuffer, lpnSize);
    if (!ret && GetLastError() == ERROR_MORE_DATA)
        SetLastError(ERROR_BUFFER_OVERFLOW);

    return ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameW(LPWSTR lpBuffer, LPDWORD lpnSize)
{
    BOOL ret;

    ret = GetComputerNameExW(ComputerNameNetBIOS, lpBuffer, lpnSize);
    if (!ret && GetLastError() == ERROR_MORE_DATA)
        SetLastError(ERROR_BUFFER_OVERFLOW);

    return ret;
}

/*
 * @implemented
 */
BOOL
WINAPI
DnsHostnameToComputerNameA(LPCSTR Hostname,
                           LPSTR ComputerName,
                           LPDWORD nSize)
{
    DWORD len;

    DPRINT("(%s, %p, %p)\n", Hostname, ComputerName, nSize);

    if (!Hostname || !nSize)
        return FALSE;

    len = lstrlenA(Hostname);

    if (len > MAX_COMPUTERNAME_LENGTH)
        len = MAX_COMPUTERNAME_LENGTH;

    if (*nSize < len)
    {
        *nSize = len;
        return FALSE;
    }

    if (!ComputerName) return FALSE;

    memcpy(ComputerName, Hostname, len);
    ComputerName[len + 1] = 0;
    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
DnsHostnameToComputerNameW(LPCWSTR hostname,
                           LPWSTR computername,
                           LPDWORD size)
{
    DWORD len;

    DPRINT("(%s, %p, %p): stub\n", hostname, computername, size);

    if (!hostname || !size) return FALSE;
    len = lstrlenW(hostname);

    if (len > MAX_COMPUTERNAME_LENGTH)
        len = MAX_COMPUTERNAME_LENGTH;

    if (*size < len)
    {
        *size = len;
        return FALSE;
    }
    if (!computername) return FALSE;

    memcpy(computername, hostname, len * sizeof(WCHAR));
    computername[len + 1] = 0;
    return TRUE;
}
