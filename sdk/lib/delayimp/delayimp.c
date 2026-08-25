/*
 * PROJECT:     ReactOS delayimport Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Implementation of delayimport library
 * COPYRIGHT:   Copyright 2009 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2016 Mark Jansen
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <delayimp.h>

/**** Linker magic: provide default (NULL) pointers in separate
 **** compilation units (pfnDliNotifyHook2.c and pfnDliFailureHook2.c),
 **** so as to allow the user to override these ****/

/* The actual symbols we use */
extern PfnDliHook __pfnDliNotifyHook2;
extern PfnDliHook __pfnDliFailureHook2;


/**** Helper functions to convert from RVA to address ****/

FORCEINLINE
unsigned
IndexFromPImgThunkData(PCImgThunkData pData, PCImgThunkData pBase)
{
    return pData - pBase;
}

extern const IMAGE_DOS_HEADER __ImageBase;

FORCEINLINE
PVOID
PFromRva(RVA rva)
{
    return (PVOID)(((ULONG_PTR)(rva)) + ((ULONG_PTR)&__ImageBase));
}


/**** Say what could not be resolved ****

 A delay load that fails leaves nothing behind to look at: the thunk jumps to
 whatever it was handed, so an unresolved import reads as a call to address
 zero, a long way from the name that was actually missing. Name it here, on
 the debug port, so the module and the function are in the log.

 Only kernel32 is used, which the helper already needs for LoadLibraryA. ****/

static
void
DelayLoadReportFailure(
    LPCSTR Dll,
    const DelayLoadProc *Proc,
    DWORD Error)
{
    CHAR Buffer[256];
    CHAR Number[16];
    DWORD Value;
    int i;

    lstrcpynA(Buffer, "delayimp: ", sizeof(Buffer));
    lstrcatA(Buffer, Dll ? Dll : "<no name>");
    lstrcatA(Buffer, "!");

    if (Proc->fImportByName)
    {
        lstrcatA(Buffer, Proc->szProcName ? Proc->szProcName : "<no name>");
    }
    else
    {
        lstrcatA(Buffer, "#");
        Value = Proc->dwOrdinal;
        i = sizeof(Number) - 1;
        Number[i] = '\0';
        do
        {
            Number[--i] = (CHAR)('0' + (Value % 10));
            Value /= 10;
        } while (Value != 0 && i > 0);
        lstrcatA(Buffer, &Number[i]);
    }

    lstrcatA(Buffer, " could not be resolved, error ");

    Value = Error;
    i = sizeof(Number) - 1;
    Number[i] = '\0';
    do
    {
        Number[--i] = (CHAR)('0' + (Value % 10));
        Value /= 10;
    } while (Value != 0 && i > 0);
    lstrcatA(Buffer, &Number[i]);
    lstrcatA(Buffer, "\n");

    OutputDebugStringA(Buffer);
}

/**** load helper ****/

FARPROC WINAPI
__delayLoadHelper2(PCImgDelayDescr pidd, PImgThunkData pIATEntry)
{
    DelayLoadInfo dli = {0};
    int index;
    PImgThunkData pIAT;
    PImgThunkData pINT;
    HMODULE *phMod;

    pIAT = PFromRva(pidd->rvaIAT);
    pINT = PFromRva(pidd->rvaINT);
    phMod = PFromRva(pidd->rvaHmod);
    index = IndexFromPImgThunkData(pIATEntry, pIAT);

    dli.cb = sizeof(dli);
    dli.pidd = pidd;
    dli.ppfn = (FARPROC*)&pIAT[index].u1.Function;
    dli.szDll = PFromRva(pidd->rvaDLLName);
    dli.dlp.fImportByName = !IMAGE_SNAP_BY_ORDINAL(pINT[index].u1.Ordinal);
    if (dli.dlp.fImportByName)
    {
        /* u1.AddressOfData points to a IMAGE_IMPORT_BY_NAME struct */
        PIMAGE_IMPORT_BY_NAME piibn = PFromRva((RVA)pINT[index].u1.AddressOfData);
        dli.dlp.szProcName = (LPCSTR)&piibn->Name;
    }
    else
    {
        dli.dlp.dwOrdinal = IMAGE_ORDINAL(pINT[index].u1.Ordinal);
    }

    if (__pfnDliNotifyHook2)
    {
        dli.pfnCur = __pfnDliNotifyHook2(dliStartProcessing, &dli);
        if (dli.pfnCur)
        {
            pIAT[index].u1.Function = (DWORD_PTR)dli.pfnCur;
            if (__pfnDliNotifyHook2)
                __pfnDliNotifyHook2(dliNoteEndProcessing, &dli);

            return dli.pfnCur;
        }
    }

    dli.hmodCur = *phMod;

    if (dli.hmodCur == NULL)
    {
        if (__pfnDliNotifyHook2)
            dli.hmodCur = (HMODULE)__pfnDliNotifyHook2(dliNotePreLoadLibrary, &dli);
        if (dli.hmodCur == NULL)
        {
            dli.hmodCur = LoadLibraryA(dli.szDll);
            if (dli.hmodCur == NULL)
            {
                dli.dwLastError = GetLastError();
                if (__pfnDliFailureHook2)
                    dli.hmodCur = (HMODULE)__pfnDliFailureHook2(dliFailLoadLib, &dli);

                if (dli.hmodCur == NULL)
                {
                    ULONG_PTR args[] = { (ULONG_PTR)&dli };

                    DelayLoadReportFailure(dli.szDll, &dli.dlp, dli.dwLastError);
                    RaiseException(VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND), 0, 1, args);

                    /* If we survive the exception, we are expected to use pfnCur directly.. */
                    return dli.pfnCur;
                }
            }
        }
        *phMod = dli.hmodCur;
    }

    dli.dwLastError = ERROR_SUCCESS;

    if (__pfnDliNotifyHook2)
        dli.pfnCur = (FARPROC)__pfnDliNotifyHook2(dliNotePreGetProcAddress, &dli);
    if (dli.pfnCur == NULL)
    {
        /* dli.dlp.szProcName might also contain the ordinal */
        dli.pfnCur = GetProcAddress(dli.hmodCur, dli.dlp.szProcName);
        if (dli.pfnCur == NULL)
        {
            dli.dwLastError = GetLastError();
            if (__pfnDliFailureHook2)
               dli.pfnCur = __pfnDliFailureHook2(dliFailGetProc, &dli);

            if (dli.pfnCur == NULL)
            {
                ULONG_PTR args[] = { (ULONG_PTR)&dli };

                DelayLoadReportFailure(dli.szDll, &dli.dlp, dli.dwLastError);
                RaiseException(VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND), 0, 1, args);

                /* Surviving the exception does not make the import resolvable.
                   Leave the thunk pointing at this helper rather than writing
                   the NULL into it: a later call then comes back through here
                   and raises again, where a patched-in NULL would instead be
                   called as a function and fault at address zero, naming
                   nothing. */
                return dli.pfnCur;
            }
        }
    }

    pIAT[index].u1.Function = (DWORD_PTR)dli.pfnCur;
    dli.dwLastError = ERROR_SUCCESS;

    if (__pfnDliNotifyHook2)
        __pfnDliNotifyHook2(dliNoteEndProcessing, &dli);

    return dli.pfnCur;
}

