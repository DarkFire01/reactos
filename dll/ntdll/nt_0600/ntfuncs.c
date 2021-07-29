#include <stdarg.h>

#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <winwlx.h>
#include <ndk/rtltypes.h>
#include <ndk/umfuncs.h>
#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

NTSTATUS
NtQueryLicenseValue(
    PUNICODE_STRING ValueName,
    ULONG *Type,
    PVOID Data,
    ULONG DataSize,
    ULONG *ResultDataSize)
{
    DPRINT1("A vista function: NtQueryLicenseValue has been called and a hacky value has been given");
    return STATUS_SUCCESS;
}