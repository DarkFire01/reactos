/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Converting between an interface index, its locally unique
 *              identifier and its name
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

/*
 * A NET_LUID is an opaque handle for an interface, and nothing outside these
 * functions may read it apart. It is kept here as the interface index in
 * NetLuidIndex and the interface's type in IfType, so that an index converts
 * to a LUID and back without asking the stack twice.
 *
 * The name a LUID converts to is the one Windows uses, "<type>_<index>", as
 * in "ethernet_2" or "loopback_1".
 */

#include "iphlpapi_private.h"
#include <limits.h>
#include <wchar.h>
#include <strsafe.h>
#include <netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

static const struct
{
    DWORD IfType;
    PCWSTR Name;
} IfTypeNames[] =
{
    { IF_TYPE_OTHER,               L"other"     },
    { IF_TYPE_ETHERNET_CSMACD,     L"ethernet"  },
    { IF_TYPE_ISO88025_TOKENRING,  L"tokenring" },
    { IF_TYPE_FDDI,                L"fddi"      },
    { IF_TYPE_PPP,                 L"ppp"       },
    { IF_TYPE_SOFTWARE_LOOPBACK,   L"loopback"  },
    { IF_TYPE_ATM,                 L"atm"       },
    { IF_TYPE_IEEE80211,           L"wireless"  },
    { IF_TYPE_TUNNEL,              L"tunnel"    },
    { IF_TYPE_IEEE1394,            L"ieee1394"  },
};

static
PCWSTR
IfTypeToName(
    _In_ DWORD IfType)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(IfTypeNames); i++)
    {
        if (IfTypeNames[i].IfType == IfType)
            return IfTypeNames[i].Name;
    }

    /* Windows names anything it does not know for its number */
    return NULL;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceIndexToLuid(
    _In_ NET_IFINDEX InterfaceIndex,
    _Out_ PNET_LUID InterfaceLuid)
{
    MIB_IFROW IfRow;
    DWORD Error;

    TRACE("InterfaceIndex %lu, InterfaceLuid %p\n", InterfaceIndex, InterfaceLuid);

    if (!InterfaceLuid)
        return ERROR_INVALID_PARAMETER;

    /* Answer with an empty LUID whatever happens below, so that a caller
       which does not look at the status does not go on to read its own
       uninitialised stack */
    InterfaceLuid->Value = 0;

    ZeroMemory(&IfRow, sizeof(IfRow));
    IfRow.dwIndex = InterfaceIndex;

    Error = GetIfEntry(&IfRow);
    if (Error != NO_ERROR)
    {
        TRACE("No interface with index %lu: %lu\n", InterfaceIndex, Error);
        return ERROR_FILE_NOT_FOUND;
    }

    InterfaceLuid->Info.Reserved = 0;
    InterfaceLuid->Info.NetLuidIndex = InterfaceIndex;
    InterfaceLuid->Info.IfType = IfRow.dwType;

    return NO_ERROR;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceLuidToIndex(
    _In_ const NET_LUID *InterfaceLuid,
    _Out_ PNET_IFINDEX InterfaceIndex)
{
    TRACE("InterfaceLuid %p, InterfaceIndex %p\n", InterfaceLuid, InterfaceIndex);

    if (!InterfaceLuid || !InterfaceIndex)
        return ERROR_INVALID_PARAMETER;

    *InterfaceIndex = (NET_IFINDEX)InterfaceLuid->Info.NetLuidIndex;

    return NO_ERROR;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceLuidToNameW(
    _In_ const NET_LUID *InterfaceLuid,
    _Out_writes_(Length) PWSTR InterfaceName,
    _In_ SIZE_T Length)
{
    WCHAR Buffer[IF_MAX_STRING_SIZE + 1];
    PCWSTR TypeName;
    HRESULT Result;
    SIZE_T Needed;

    TRACE("InterfaceLuid %p, InterfaceName %p, Length %Iu\n",
          InterfaceLuid, InterfaceName, Length);

    if (!InterfaceLuid || !InterfaceName)
        return ERROR_INVALID_PARAMETER;

    TypeName = IfTypeToName((DWORD)InterfaceLuid->Info.IfType);
    if (TypeName)
    {
        Result = StringCchPrintfW(Buffer, ARRAYSIZE(Buffer), L"%ls_%lu",
                                  TypeName, (ULONG)InterfaceLuid->Info.NetLuidIndex);
    }
    else
    {
        Result = StringCchPrintfW(Buffer, ARRAYSIZE(Buffer), L"iftype%lu_%lu",
                                  (ULONG)InterfaceLuid->Info.IfType,
                                  (ULONG)InterfaceLuid->Info.NetLuidIndex);
    }

    if (FAILED(Result))
        return ERROR_INVALID_PARAMETER;

    Needed = wcslen(Buffer) + 1;
    if (Length < Needed)
        return ERROR_NOT_ENOUGH_MEMORY;

    CopyMemory(InterfaceName, Buffer, Needed * sizeof(WCHAR));

    return NO_ERROR;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceLuidToNameA(
    _In_ const NET_LUID *InterfaceLuid,
    _Out_writes_(Length) PSTR InterfaceName,
    _In_ SIZE_T Length)
{
    WCHAR Buffer[IF_MAX_STRING_SIZE + 1];
    NETIO_STATUS Status;
    int Written;

    TRACE("InterfaceLuid %p, InterfaceName %p, Length %Iu\n",
          InterfaceLuid, InterfaceName, Length);

    if (!InterfaceName)
        return ERROR_INVALID_PARAMETER;

    Status = ConvertInterfaceLuidToNameW(InterfaceLuid, Buffer, ARRAYSIZE(Buffer));
    if (Status != NO_ERROR)
        return Status;

    /* The names are all ASCII, so the character count carries over */
    Written = WideCharToMultiByte(CP_ACP, 0, Buffer, -1, InterfaceName,
                                  (int)min(Length, (SIZE_T)INT_MAX), NULL, NULL);
    if (Written == 0)
        return ERROR_NOT_ENOUGH_MEMORY;

    return NO_ERROR;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceNameToLuidW(
    _In_ const WCHAR *InterfaceName,
    _Out_ PNET_LUID InterfaceLuid)
{
    PCWSTR Separator;
    PWSTR End;
    ULONG Index, i;
    SIZE_T TypeLength;

    TRACE("InterfaceName %s, InterfaceLuid %p\n",
          debugstr_w(InterfaceName), InterfaceLuid);

    if (!InterfaceLuid)
        return ERROR_INVALID_PARAMETER;

    InterfaceLuid->Value = 0;

    if (!InterfaceName)
        return ERROR_INVALID_NAME;

    /* The name reads "<type>_<index>", and a type may not hold an underscore,
       so the last one separates the two */
    Separator = wcsrchr(InterfaceName, L'_');
    if (!Separator || Separator == InterfaceName || Separator[1] == UNICODE_NULL)
        return ERROR_INVALID_NAME;

    Index = wcstoul(Separator + 1, &End, 10);
    if (*End != UNICODE_NULL)
        return ERROR_INVALID_NAME;

    TypeLength = Separator - InterfaceName;

    for (i = 0; i < ARRAYSIZE(IfTypeNames); i++)
    {
        if (wcslen(IfTypeNames[i].Name) == TypeLength &&
            _wcsnicmp(IfTypeNames[i].Name, InterfaceName, TypeLength) == 0)
        {
            InterfaceLuid->Info.NetLuidIndex = Index;
            InterfaceLuid->Info.IfType = IfTypeNames[i].IfType;
            return NO_ERROR;
        }
    }

    /* The form we fall back on for a type we have no name for */
    if (TypeLength > 6 && _wcsnicmp(InterfaceName, L"iftype", 6) == 0)
    {
        ULONG IfType = wcstoul(InterfaceName + 6, &End, 10);

        if (End == Separator)
        {
            InterfaceLuid->Info.NetLuidIndex = Index;
            InterfaceLuid->Info.IfType = IfType;
            return NO_ERROR;
        }
    }

    return ERROR_INVALID_NAME;
}

/*
 * @implemented
 */
NETIOAPI_API
ConvertInterfaceNameToLuidA(
    _In_ const CHAR *InterfaceName,
    _Out_ PNET_LUID InterfaceLuid)
{
    WCHAR Buffer[IF_MAX_STRING_SIZE + 1];

    TRACE("InterfaceName %s, InterfaceLuid %p\n",
          debugstr_a(InterfaceName), InterfaceLuid);

    if (!InterfaceLuid)
        return ERROR_INVALID_PARAMETER;

    InterfaceLuid->Value = 0;

    if (!InterfaceName)
        return ERROR_INVALID_NAME;

    if (MultiByteToWideChar(CP_ACP, 0, InterfaceName, -1, Buffer,
                            ARRAYSIZE(Buffer)) == 0)
    {
        return ERROR_INVALID_NAME;
    }

    return ConvertInterfaceNameToLuidW(Buffer, InterfaceLuid);
}
