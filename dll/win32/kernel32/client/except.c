/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/except.c
 * PURPOSE:         Exception functions
 * PROGRAMMER:      Ariadne (ariadne@xs4all.nl)
 *                  Modified from WINE [ Onno Hovers, (onno@stack.urc.tue.nl) ]
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 */
BOOL
WINAPI
IsBadReadPtr(IN LPCVOID lp,
             IN UINT_PTR ucb)
{
    ULONG PageSize;
    BOOLEAN Result = FALSE;
    volatile CHAR *Current;
    PCHAR Last;

    /* Quick cases */
    if (!ucb) return FALSE;
    if (!lp) return TRUE;

    /* Get the page size */
    PageSize = SystemInfo.dwPageSize;

    /* Calculate start and end */
    Current = (volatile CHAR*)lp;
    Last = (PCHAR)((ULONG_PTR)lp + ucb - 1);

    /* Another quick failure case */
    if (Last < Current) return TRUE;

    /* Enter SEH */
    _SEH2_TRY
    {
        /* Do an initial probe */
        *Current;

        /* Align the addresses */
        Current = (volatile CHAR *)ALIGN_DOWN_POINTER_BY(Current, PageSize);
        Last = (PCHAR)ALIGN_DOWN_POINTER_BY(Last, PageSize);

        /* Probe the entire range */
        while (Current != Last)
        {
            Current += PageSize;
            *Current;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* We hit an exception, so return true */
        Result = TRUE;
    }
    _SEH2_END

    /* Return exception status */
    return Result;
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadHugeReadPtr(LPCVOID lp,
                 UINT_PTR ucb)
{
    /* Implementation is the same on 32-bit */
    return IsBadReadPtr(lp, ucb);
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadCodePtr(FARPROC lpfn)
{
    /* Executing has the same privileges as reading */
    return IsBadReadPtr((LPVOID)lpfn, 1);
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadWritePtr(IN LPVOID lp,
              IN UINT_PTR ucb)
{
    ULONG PageSize;
    BOOLEAN Result = FALSE;
    volatile CHAR *Current;
    PCHAR Last;

    /* Quick cases */
    if (!ucb) return FALSE;
    if (!lp) return TRUE;

    /* Get the page size */
    PageSize = SystemInfo.dwPageSize;

    /* Calculate start and end */
    Current = (volatile CHAR*)lp;
    Last = (PCHAR)((ULONG_PTR)lp + ucb - 1);

    /* Another quick failure case */
    if (Last < Current) return TRUE;

    /* Enter SEH */
    _SEH2_TRY
    {
        /* Do an initial probe */
        *Current = *Current;

        /* Align the addresses */
        Current = (volatile CHAR *)ALIGN_DOWN_POINTER_BY(Current, PageSize);
        Last = (PCHAR)ALIGN_DOWN_POINTER_BY(Last, PageSize);

        /* Probe the entire range */
        while (Current != Last)
        {
            Current += PageSize;
            *Current = *Current;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* We hit an exception, so return true */
        Result = TRUE;
    }
    _SEH2_END

    /* Return exception status */
    return Result;
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadHugeWritePtr(IN LPVOID lp,
                  IN UINT_PTR ucb)
{
    /* Implementation is the same on 32-bit */
    return IsBadWritePtr(lp, ucb);
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadStringPtrW(IN LPCWSTR lpsz,
                IN UINT_PTR ucchMax)
{
    BOOLEAN Result = FALSE;
    volatile WCHAR *Current;
    PWCHAR Last;
    WCHAR Char;

    /* Quick cases */
    if (!ucchMax) return FALSE;
    if (!lpsz) return TRUE;

    /* Calculate start and end */
    Current = (volatile WCHAR*)lpsz;
    Last = (PWCHAR)((ULONG_PTR)lpsz + (ucchMax * 2) - 2);

    /* Enter SEH */
    _SEH2_TRY
    {
        /* Probe the entire range */
        Char = *Current++;
        while ((Char) && (Current != Last)) Char = *Current++;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* We hit an exception, so return true */
        Result = TRUE;
    }
    _SEH2_END

    /* Return exception status */
    return Result;
}

/*
 * @implemented
 */
BOOL
NTAPI
IsBadStringPtrA(IN LPCSTR lpsz,
                IN UINT_PTR ucchMax)
{
    BOOLEAN Result = FALSE;
    volatile CHAR *Current;
    PCHAR Last;
    CHAR Char;

    /* Quick cases */
    if (!ucchMax) return FALSE;
    if (!lpsz) return TRUE;

    /* Calculate start and end */
    Current = (volatile CHAR*)lpsz;
    Last = (PCHAR)((ULONG_PTR)lpsz + ucchMax - 1);

    /* Enter SEH */
    _SEH2_TRY
    {
        /* Probe the entire range */
        Char = *Current++;
        while ((Char) && (Current != Last)) Char = *Current++;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        /* We hit an exception, so return true */
        Result = TRUE;
    }
    _SEH2_END

    /* Return exception status */
    return Result;
}