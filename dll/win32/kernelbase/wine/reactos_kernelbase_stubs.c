/*
 * ReactOS-only symbols referenced by Wine-sourced kernelbase code but not
 * provided by ntdll import libraries in this configuration.
 */

#ifdef __REACTOS__

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

NTSTATUS WINAPI RtlGetLocaleFileMappingAddress(void **base, LCID *lcid, LARGE_INTEGER *size)
{
    (void)base;
    (void)lcid;
    (void)size;
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* __REACTOS__ */
