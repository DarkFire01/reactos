/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for UserHandleGrantAccess and the job UI restriction callouts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#include <strsafe.h>

#define READY_EVENT L"user32_apitest_UserHandleGrantAccess_ready"
#define QUIT_EVENT  L"user32_apitest_UserHandleGrantAccess_quit"

/* What sandbox::Job::Init(JOB_LOCKDOWN) asks for */
#define JOB_LOCKDOWN_UI (JOB_OBJECT_UILIMIT_HANDLES         | \
                         JOB_OBJECT_UILIMIT_READCLIPBOARD   | \
                         JOB_OBJECT_UILIMIT_WRITECLIPBOARD  | \
                         JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS| \
                         JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | \
                         JOB_OBJECT_UILIMIT_GLOBALATOMS     | \
                         JOB_OBJECT_UILIMIT_DESKTOP         | \
                         JOB_OBJECT_UILIMIT_EXITWINDOWS)

static
HANDLE
CreateRestrictedJob(_In_ ULONG Restrictions)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hJob;

    hJob = CreateJobObjectW(NULL, NULL);
    if (hJob == NULL)
    {
        skip("CreateJobObject failed with %lu\n", GetLastError());
        return NULL;
    }

    if (Restrictions != 0)
    {
        Info.UIRestrictionsClass = Restrictions;
        if (!SetInformationJobObject(hJob,
                                     JobObjectBasicUIRestrictions,
                                     &Info,
                                     sizeof(Info)))
        {
            skip("Setting restrictions 0x%lx failed with %lu\n",
                 Restrictions, GetLastError());
            CloseHandle(hJob);
            return NULL;
        }
    }

    return hJob;
}

static
void
test_GrantAccess(void)
{
    HANDLE hJob;
    HWND hWnd;
    BOOL Success;

    hWnd = CreateWindowExW(0,
                           L"Static",
                           L"UserHandleGrantAccess",
                           WS_POPUP,
                           0, 0, 10, 10,
                           NULL, NULL, NULL, NULL);
    ok(hWnd != NULL, "CreateWindowEx failed with %lu\n", GetLastError());
    if (hWnd == NULL)
        return;

    /* A job that restricts nothing keeps no granted list */
    hJob = CreateRestrictedJob(0);
    if (hJob != NULL)
    {
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
        ok(Success == FALSE, "Granting to an unrestricted job succeeded\n");
        ok_err(ERROR_INVALID_PARAMETER);
        CloseHandle(hJob);
    }

    /* The same is true of a job that restricts something else */
    hJob = CreateRestrictedJob(JOB_OBJECT_UILIMIT_EXITWINDOWS);
    if (hJob != NULL)
    {
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
        ok(Success == TRUE, "Granting to a restricted job failed with %lu\n",
           GetLastError());
        CloseHandle(hJob);
    }

    hJob = CreateRestrictedJob(JOB_OBJECT_UILIMIT_HANDLES);
    if (hJob != NULL)
    {
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
        ok(Success == TRUE, "Granting failed with %lu\n", GetLastError());

        /* Granting the same handle twice is not an error */
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
        ok(Success == TRUE, "Granting twice failed with %lu\n", GetLastError());

        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, FALSE);
        ok(Success == TRUE, "Revoking failed with %lu\n", GetLastError());

        /* Nor is revoking an access that was never granted */
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess(hWnd, hJob, FALSE);
        ok(Success == TRUE, "Revoking twice failed with %lu\n", GetLastError());

        /* Only a handle that exists can be granted */
        SetLastError(0xDEADBEEF);
        Success = UserHandleGrantAccess((HANDLE)(ULONG_PTR)0x0000BEEF, hJob, TRUE);
        ok(Success == FALSE, "Granting a handle that does not exist succeeded\n");

        {
            HWND hWndGone;

            hWndGone = CreateWindowExW(0, L"Static", NULL, WS_POPUP,
                                       0, 0, 10, 10,
                                       NULL, NULL, NULL, NULL);
            if (hWndGone != NULL)
            {
                DestroyWindow(hWndGone);

                SetLastError(0xDEADBEEF);
                Success = UserHandleGrantAccess(hWndGone, hJob, TRUE);
                ok(Success == FALSE, "Granting a destroyed window succeeded\n");

                /* Revoking a destroyed handle is still allowed */
                SetLastError(0xDEADBEEF);
                Success = UserHandleGrantAccess(hWndGone, hJob, FALSE);
                ok(Success == TRUE, "Revoking a destroyed window failed with %lu\n",
                   GetLastError());
            }
        }

        /* Enough handles to make the granted list grow more than once */
        {
            HWND Windows[16];
            ULONG i;

            for (i = 0; i < _countof(Windows); i++)
            {
                Windows[i] = CreateWindowExW(0, L"Static", NULL, WS_POPUP,
                                             0, 0, 10, 10,
                                             NULL, NULL, NULL, NULL);
                if (Windows[i] == NULL)
                {
                    skip("CreateWindowEx failed with %lu\n", GetLastError());
                    break;
                }

                Success = UserHandleGrantAccess(Windows[i], hJob, TRUE);
                ok(Success == TRUE, "Granting window %lu failed with %lu\n",
                   i, GetLastError());
            }

            while (i-- > 0)
            {
                Success = UserHandleGrantAccess(Windows[i], hJob, FALSE);
                ok(Success == TRUE, "Revoking window %lu failed with %lu\n",
                   i, GetLastError());
                DestroyWindow(Windows[i]);
            }
        }

        /* Destroying a granted window has to withdraw the grant. The list
           cannot be read from here, so this only shows it stays intact. */
        {
            HWND Windows[8];
            ULONG i;

            for (i = 0; i < _countof(Windows); i++)
            {
                Windows[i] = CreateWindowExW(0, L"Static", NULL, WS_POPUP,
                                             0, 0, 10, 10,
                                             NULL, NULL, NULL, NULL);
                if (Windows[i] == NULL)
                {
                    skip("CreateWindowEx failed with %lu\n", GetLastError());
                    break;
                }

                Success = UserHandleGrantAccess(Windows[i], hJob, TRUE);
                ok(Success == TRUE, "Granting window %lu failed with %lu\n",
                   i, GetLastError());
            }

            /* Destroy them without revoking first */
            while (i-- > 0)
                DestroyWindow(Windows[i]);

            /* The list has to still work afterwards */
            SetLastError(0xDEADBEEF);
            Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
            ok(Success == TRUE, "Granting after a sweep failed with %lu\n",
               GetLastError());
            SetLastError(0xDEADBEEF);
            Success = UserHandleGrantAccess(hWnd, hJob, FALSE);
            ok(Success == TRUE, "Revoking after a sweep failed with %lu\n",
               GetLastError());
        }

        /* Close the job with handles still granted, to free the list */
        Success = UserHandleGrantAccess(hWnd, hJob, TRUE);
        ok(Success == TRUE, "Granting failed with %lu\n", GetLastError());
        CloseHandle(hJob);
    }

    /* A handle that is not a job at all */
    SetLastError(0xDEADBEEF);
    Success = UserHandleGrantAccess(hWnd, GetCurrentProcess(), TRUE);
    ok(Success == FALSE, "Granting against a process handle succeeded\n");

    SetLastError(0xDEADBEEF);
    Success = UserHandleGrantAccess(hWnd, NULL, TRUE);
    ok(Success == FALSE, "Granting against a NULL job succeeded\n");

    DestroyWindow(hWnd);
}

static
HANDLE
StartChildEx(_In_ PCWSTR Arguments, _Out_ PHANDLE Thread)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine,
                    sizeof(CommandLine),
                    L"\"%ls\" UserHandleGrantAccess %ls",
                    FileName,
                    Arguments);

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    if (!CreateProcessW(FileName,
                        CommandLine,
                        NULL,
                        NULL,
                        FALSE,
                        0,
                        NULL,
                        NULL,
                        &StartupInfo,
                        &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        *Thread = NULL;
        return NULL;
    }

    *Thread = ProcessInfo.hThread;
    return ProcessInfo.hProcess;
}

/*
 * Runs a child through a job. RestrictFirst TRUE assigns into an already
 * restricted job, so the kernel hands the process over from the assignment
 * path; FALSE restricts afterwards, so win32k has to find it itself.
 */
static
void
test_ProcessInJob(_In_ BOOL RestrictFirst)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hReady, hQuit, hJob, hProcess, hThread;
    DWORD Wait;
    BOOL Success;

    hReady = CreateEventW(NULL, TRUE, FALSE, READY_EVENT);
    hQuit = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT);
    if (hReady == NULL || hQuit == NULL)
    {
        skip("CreateEvent failed with %lu\n", GetLastError());
        if (hReady) CloseHandle(hReady);
        if (hQuit) CloseHandle(hQuit);
        return;
    }

    ResetEvent(hReady);
    ResetEvent(hQuit);

    hJob = CreateRestrictedJob(RestrictFirst ? JOB_LOCKDOWN_UI : 0);
    if (hJob == NULL)
    {
        CloseHandle(hReady);
        CloseHandle(hQuit);
        return;
    }

    hProcess = StartChildEx(L"child", &hThread);
    if (hProcess == NULL)
    {
        CloseHandle(hJob);
        CloseHandle(hReady);
        CloseHandle(hQuit);
        return;
    }

    /* The callout is only made for a process that is a win32k client */
    Wait = WaitForSingleObject(hReady, 10000);
    ok(Wait == WAIT_OBJECT_0, "The child did not become ready, wait returned %lu\n", Wait);

    if (Wait == WAIT_OBJECT_0)
    {
        SetLastError(0xDEADBEEF);
        Success = AssignProcessToJobObject(hJob, hProcess);
        if (!Success && GetLastError() == ERROR_ACCESS_DENIED)
        {
            /* The test itself is running in a job that does not allow
               breakaway, so the child inherited it */
            skip("The test is already running in a job\n");
        }
        else
        {
            ok(Success == TRUE, "AssignProcessToJobObject failed with %lu\n",
               GetLastError());

            /* Restricted up front, the child leaves the job by exiting.
               Otherwise restrict it now and lift it again, so win32k has to
               let go of a process it still holds. */
            if (!RestrictFirst)
            {
                Info.UIRestrictionsClass = JOB_LOCKDOWN_UI;
                SetLastError(0xDEADBEEF);
                Success = SetInformationJobObject(hJob,
                                                  JobObjectBasicUIRestrictions,
                                                  &Info,
                                                  sizeof(Info));
                ok(Success == TRUE, "Restricting a populated job failed with %lu\n",
                   GetLastError());

                Info.UIRestrictionsClass = 0;
                SetLastError(0xDEADBEEF);
                Success = SetInformationJobObject(hJob,
                                                  JobObjectBasicUIRestrictions,
                                                  &Info,
                                                  sizeof(Info));
                ok(Success == TRUE, "Clearing the restrictions failed with %lu\n",
                   GetLastError());
            }
        }
    }

    SetEvent(hQuit);
    Wait = WaitForSingleObject(hProcess, 10000);
    ok(Wait == WAIT_OBJECT_0, "The child did not exit, wait returned %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(hProcess, 1);

    CloseHandle(hThread);
    CloseHandle(hProcess);
    CloseHandle(hJob);
    CloseHandle(hQuit);
    CloseHandle(hReady);
}

/* The child: become a win32k client, say so, and wait to be let go */
static
void
RunChild(void)
{
    HANDLE hReady, hQuit;

    /* Any USER call connects us to win32k */
    GetDesktopWindow();

    hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, READY_EVENT);
    hQuit = OpenEventW(SYNCHRONIZE, FALSE, QUIT_EVENT);

    if (hReady != NULL)
    {
        SetEvent(hReady);
        CloseHandle(hReady);
    }

    if (hQuit != NULL)
    {
        WaitForSingleObject(hQuit, 30000);
        CloseHandle(hQuit);
    }
}

/* What the child reports back through its exit code */
#define CHILD_DENIED    0
#define CHILD_ALLOWED   1
#define CHILD_BROKEN    2

#define JOB_ATOM_NAME L"user32_apitest_UserHandleGrantAccess_atom"

/*
 * The child of the enforcement tests: become a win32k client, wait to be put
 * into the restricted job, then try the one operation it was asked to try.
 */
static
int
RunRestrictedChild(_In_ PCSTR Operation, _In_opt_ PCSTR Argument)
{
    HANDLE hReady, hQuit;
    int Result = CHILD_BROKEN;

    /* Any USER call connects us to win32k */
    GetDesktopWindow();

    hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, READY_EVENT);
    hQuit = OpenEventW(SYNCHRONIZE, FALSE, QUIT_EVENT);
    if (hReady == NULL || hQuit == NULL)
        goto Quit;

    SetEvent(hReady);
    if (WaitForSingleObject(hQuit, 30000) != WAIT_OBJECT_0)
        goto Quit;

    /* We are in the job now */
    if (!strcmp(Operation, "handles"))
    {
        HWND hWnd = (HWND)(ULONG_PTR)strtoul(Argument, NULL, 16);

        /* A window of the parent, which is not in our job */
        Result = SetWindowPos(hWnd, NULL, 0, 0, 1, 1,
                              SWP_NOZORDER | SWP_NOACTIVATE)
                 ? CHILD_ALLOWED : CHILD_DENIED;
    }
    else if (!strcmp(Operation, "desktop"))
    {
        HDESK hDesk = CreateDesktopW(L"user32_apitest_jobdesk", NULL, NULL,
                                     0, GENERIC_ALL, NULL);
        Result = (hDesk != NULL) ? CHILD_ALLOWED : CHILD_DENIED;
        if (hDesk != NULL)
            CloseDesktop(hDesk);
    }
    else if (!strcmp(Operation, "display"))
    {
        LONG lRet = ChangeDisplaySettingsW(NULL, CDS_TEST);
        Result = (lRet == DISP_CHANGE_SUCCESSFUL) ? CHILD_ALLOWED : CHILD_DENIED;
    }
    else if (!strcmp(Operation, "sysparams"))
    {
        BOOL bBeep = FALSE;

        /* Reading is still allowed, only changing is not */
        if (!SystemParametersInfoW(SPI_GETBEEP, 0, &bBeep, 0))
            Result = CHILD_BROKEN;
        else if (SystemParametersInfoW(SPI_SETBEEP, bBeep, NULL, 0))
            Result = CHILD_ALLOWED;
        else
            Result = CHILD_DENIED;
    }
    else if (!strcmp(Operation, "atoms"))
    {
        /* This is not refused, it goes into a table of the job instead, so
           the parent is the one that can tell whether it worked */
        Result = GlobalAddAtomW(JOB_ATOM_NAME) != 0 ? CHILD_DENIED : CHILD_BROKEN;
    }

Quit:
    if (hReady != NULL)
        CloseHandle(hReady);
    if (hQuit != NULL)
        CloseHandle(hQuit);

    return Result;
}

/* Runs one child inside a job with the given restrictions, returns its verdict */
static
DWORD
RunChildInJob(_In_ ULONG Restrictions, _In_ PCWSTR Arguments)
{
    HANDLE hReady, hQuit, hJob, hProcess, hThread;
    DWORD Wait, ExitCode = CHILD_BROKEN;

    hReady = CreateEventW(NULL, TRUE, FALSE, READY_EVENT);
    hQuit = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT);
    if (hReady == NULL || hQuit == NULL)
    {
        skip("CreateEvent failed with %lu\n", GetLastError());
        if (hReady) CloseHandle(hReady);
        if (hQuit) CloseHandle(hQuit);
        return CHILD_BROKEN;
    }

    ResetEvent(hReady);
    ResetEvent(hQuit);

    hJob = CreateRestrictedJob(Restrictions);
    if (hJob == NULL)
    {
        CloseHandle(hReady);
        CloseHandle(hQuit);
        return CHILD_BROKEN;
    }

    hProcess = StartChildEx(Arguments, &hThread);
    if (hProcess == NULL)
    {
        CloseHandle(hJob);
        CloseHandle(hReady);
        CloseHandle(hQuit);
        return CHILD_BROKEN;
    }

    Wait = WaitForSingleObject(hReady, 10000);
    if (Wait != WAIT_OBJECT_0)
    {
        ok(0, "The child did not become ready, wait returned %lu\n", Wait);
    }
    else if (!AssignProcessToJobObject(hJob, hProcess))
    {
        skip("AssignProcessToJobObject failed with %lu\n", GetLastError());
    }
    else
    {
        SetEvent(hQuit);

        Wait = WaitForSingleObject(hProcess, 15000);
        if (Wait != WAIT_OBJECT_0)
            ok(0, "The child did not exit, wait returned %lu\n", Wait);
        else if (!GetExitCodeProcess(hProcess, &ExitCode))
            ok(0, "GetExitCodeProcess failed with %lu\n", GetLastError());
    }

    SetEvent(hQuit);
    if (WaitForSingleObject(hProcess, 5000) != WAIT_OBJECT_0)
        TerminateProcess(hProcess, CHILD_BROKEN);

    CloseHandle(hThread);
    CloseHandle(hProcess);
    CloseHandle(hJob);
    CloseHandle(hQuit);
    CloseHandle(hReady);

    return ExitCode;
}

static
void
test_Enforcement(void)
{
    WCHAR Arguments[128];
    HWND hWnd;
    DWORD Result;
    ATOM Atom;

    /* A window of ours, which the child in the job does not own */
    hWnd = CreateWindowExW(0, L"Static", NULL, WS_POPUP, 0, 0, 10, 10,
                           NULL, NULL, NULL, NULL);
    ok(hWnd != NULL, "CreateWindowEx failed with %lu\n", GetLastError());
    if (hWnd != NULL)
    {
        StringCbPrintfW(Arguments,
                        sizeof(Arguments),
                        L"restricted handles %lx",
                        (ULONG)(ULONG_PTR)hWnd);

        Result = RunChildInJob(JOB_OBJECT_UILIMIT_HANDLES, Arguments);
        ok(Result == CHILD_DENIED,
           "A restricted job reached a window of another job, result %lu\n", Result);

        /* Without the restriction the very same call has to work, otherwise
           the test above proves nothing */
        Result = RunChildInJob(JOB_OBJECT_UILIMIT_EXITWINDOWS, Arguments);
        ok(Result == CHILD_ALLOWED,
           "An unrestricted job could not reach the window, result %lu\n", Result);

        DestroyWindow(hWnd);
    }

    Result = RunChildInJob(JOB_OBJECT_UILIMIT_DESKTOP, L"restricted desktop");
    ok(Result == CHILD_DENIED, "CreateDesktop was allowed, result %lu\n", Result);

    Result = RunChildInJob(JOB_OBJECT_UILIMIT_DISPLAYSETTINGS, L"restricted display");
    ok(Result == CHILD_DENIED, "ChangeDisplaySettings was allowed, result %lu\n", Result);

    Result = RunChildInJob(JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS, L"restricted sysparams");
    ok(Result == CHILD_DENIED, "SystemParametersInfo was allowed, result %lu\n", Result);

    /* The atom the child added has to have gone into a table of its own */
    GlobalDeleteAtom(GlobalFindAtomW(JOB_ATOM_NAME));
    Result = RunChildInJob(JOB_OBJECT_UILIMIT_GLOBALATOMS, L"restricted atoms");
    ok(Result == CHILD_DENIED, "The child could not add its atom, result %lu\n", Result);

    SetLastError(0xDEADBEEF);
    Atom = GlobalFindAtomW(JOB_ATOM_NAME);
    ok(Atom == 0, "The atom of a restricted job is visible to us as 0x%x\n", Atom);
    if (Atom != 0)
        GlobalDeleteAtom(Atom);
}

START_TEST(UserHandleGrantAccess)
{
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        RunChild();
        return;
    }
    if (argc >= 4 && !strcmp(argv[2], "restricted"))
    {
        ExitProcess(RunRestrictedChild(argv[3], argc >= 5 ? argv[4] : NULL));
    }

    test_GrantAccess();
    test_ProcessInJob(TRUE);
    test_ProcessInJob(FALSE);
    test_Enforcement();
}
