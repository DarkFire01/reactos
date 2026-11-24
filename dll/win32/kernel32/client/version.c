/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/version.c
 * PURPOSE:         Version functions
 * PROGRAMMER:      Ariadne (ariadne@xs4all.nl)
                    Ged Murphy (gedmurphy@reactos.org)
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
VerifyVersionInfoW(IN LPOSVERSIONINFOEXW lpVersionInformation,
                   IN DWORD dwTypeMask,
                   IN DWORDLONG dwlConditionMask)
{
    return TRUE; // MIkhail Tyukin remove
    NTSTATUS Status;

    Status = RtlVerifyVersionInfo((PRTL_OSVERSIONINFOEXW)lpVersionInformation,
                                  dwTypeMask,
                                  dwlConditionMask);
    switch (Status)
    {
        case STATUS_INVALID_PARAMETER:
            SetLastError(ERROR_BAD_ARGUMENTS);
            return FALSE;

        case STATUS_REVISION_MISMATCH:
            if (lpVersionInformation)
            {
                DPRINT1("VerifyVersionInfo -- Version mismatch(%d.%d.%d:%d)\n",
                        (dwTypeMask & VER_MAJORVERSION) ? lpVersionInformation->dwMajorVersion : -1,
                        (dwTypeMask & VER_MINORVERSION) ? lpVersionInformation->dwMinorVersion : -1,
                        (dwTypeMask & VER_BUILDNUMBER) ? lpVersionInformation->dwBuildNumber : -1,
                        (dwTypeMask & VER_PLATFORMID) ? lpVersionInformation->dwPlatformId : -1);
            }
            else
            {
                DPRINT1("VerifyVersionInfo -- Version mismatch(NULL)\n");
            }
            SetLastError(ERROR_OLD_WIN_VERSION);
            return FALSE;

        default:
            /* RtlVerifyVersionInfo shouldn't report any other failure code! */
            ASSERT(NT_SUCCESS(Status));
            return TRUE;
    }
}
