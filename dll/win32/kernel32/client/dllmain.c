/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/dllmain.c
 * PURPOSE:         Initialization
 * PROGRAMMERS:     Ariadne (ariadne@xs4all.nl)
 *                  Aleksey Bragin (aleksey@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

LPC_PTRTYPE(PBASE_STATIC_SERVER_DATA) BaseStaticServerData;
BOOLEAN BaseRunningInServerProcess = FALSE;

WCHAR BaseDefaultPathBuffer[6140];

HANDLE BaseNamedObjectDirectory;
HMODULE hCurrentModule = NULL;
HMODULE kernel32_handle = NULL;
PPEB Peb;
ULONG SessionId;
static BOOL DllInitialized = FALSE;

#ifdef BUILD_WOW6432
static WCHAR BaseWindowsSystemDirectoryBuffer[MAX_PATH];
static WCHAR BaseWindowsDirectoryBuffer[MAX_PATH];
#endif

/* Critical section for various kernel32 data structures */
RTL_CRITICAL_SECTION BaseDllDirectoryLock;

extern BOOL FASTCALL NlsInit(VOID);
extern VOID FASTCALL NlsUninit(VOID);

#define WIN_OBJ_DIR L"\\Windows"
#define SESSION_DIR L"\\Sessions"

/* FUNCTIONS *****************************************************************/

NTSTATUS
NTAPI
BaseCreateThreadPoolThread(IN PTHREAD_START_ROUTINE Function,
                           IN PVOID Parameter,
                           OUT PHANDLE ThreadHandle)
{
    NTSTATUS Status;

    /* Create a Win32 thread */
    *ThreadHandle = CreateRemoteThread(NtCurrentProcess(),
                                       NULL,
                                       0,
                                       Function,
                                       Parameter,
                                       CREATE_SUSPENDED,
                                       NULL);
    if (!(*ThreadHandle))
    {
        /* Get the status value if we couldn't get a handle */
        Status = NtCurrentTeb()->LastStatusValue;
        if (NT_SUCCESS(Status)) Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        /* Set success code */
        Status = STATUS_SUCCESS;
    }

    /* All done */
    return Status;
}

NTSTATUS
NTAPI
BaseExitThreadPoolThread(IN NTSTATUS ExitStatus)
{
    /* Exit the thread */
    ExitThread(ExitStatus);
    return STATUS_SUCCESS;
}

BOOL
WINAPI
DllMain(HANDLE hDll,
        DWORD dwReason,
        LPVOID lpReserved)
{
    NTSTATUS Status;
    BASESRV_API_CONNECTINFO ConnectInfo;
    ULONG ConnectInfoSize = sizeof(ConnectInfo);
    WCHAR SessionDir[256];

    DPRINT("DllMain(hInst %p, dwReason %lu)\n",
           hDll, dwReason);

    Basep8BitStringToUnicodeString = RtlAnsiStringToUnicodeString;

    /* Cache the PEB and Session ID */
    Peb = NtCurrentPeb();
    SessionId = Peb->SessionId;

    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            /* Set no filter initially */
            GlobalTopLevelExceptionFilter = RtlEncodePointer(NULL);

            /* Enable the Rtl thread pool and timer queue to use proper Win32 thread */
            RtlSetThreadPoolStartFunc(BaseCreateThreadPoolThread, BaseExitThreadPoolThread);

            /* Register the manifest prober routine */
            LdrSetDllManifestProber(BasepProbeForDllManifest);

            /* Don't bother us for each thread */
            LdrDisableThreadCalloutsForDll((PVOID)hDll);

            /* Initialize default path to NULL */
            RtlInitUnicodeString(&BaseDefaultPath, NULL);

            /* Setup the Object Directory path */
            if (!SessionId)
            {
                /* Use the raw path */
                wcscpy(SessionDir, WIN_OBJ_DIR);
            }
            else
            {
                /* Use the session path */
                _swprintf(SessionDir,
                          L"%ws\\%lu%ws",
                          SESSION_DIR,
                          SessionId,
                          WIN_OBJ_DIR);
            }

            /* Connect to the Base Server */
            Status = CsrClientConnectToServer(SessionDir,
                                              BASESRV_SERVERDLL_INDEX,
                                              &ConnectInfo,
                                              &ConnectInfoSize,
                                              &BaseRunningInServerProcess);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Failed to connect to CSR (Status %lx)\n", Status);
                NtTerminateProcess(NtCurrentProcess(), Status);
                return FALSE;
            }

            /* Get the server data */
#ifndef BUILD_WOW6432
            ASSERT(Peb->ReadOnlyStaticServerData);
            BaseStaticServerData = Peb->ReadOnlyStaticServerData[BASESRV_SERVERDLL_INDEX];
#else
            ASSERT(NtCurrentPeb64()->ReadOnlyStaticServerData);
            BaseStaticServerData =
                Wow64ReadNativePtr((UINT64)NtCurrentPeb64()->ReadOnlyStaticServerData
                                    + BASESRV_SERVERDLL_INDEX * sizeof(LPC_PVOID));
#endif
            if (!BaseStaticServerData)
            {
                DPRINT1("BaseStaticServerData NULL\n");
                __debugbreak();
            }

            /* Check if we are running a CSR Server */
            if (!BaseRunningInServerProcess)
            {
                /* Set the termination port for the thread */
                DPRINT("Creating new thread for CSR\n");
                CsrNewThread();
            }

            /* Initialize heap handle table */
            BaseDllInitializeMemoryManager();

            /* Set HMODULE for our DLL */
            kernel32_handle = hCurrentModule = hDll;

            /* Set the directories */
#ifndef BUILD_WOW6432
            BaseWindowsDirectory.Length = BaseStaticServerData->WindowsDirectory.Length;
            BaseWindowsSystemDirectory.Length = BaseStaticServerData->WindowsSystemDirectory.Length;
            BaseWindowsDirectory.MaximumLength = BaseStaticServerData->WindowsDirectory.MaximumLength;
            BaseWindowsSystemDirectory.MaximumLength = BaseStaticServerData->WindowsSystemDirectory.MaximumLength;
            BaseWindowsDirectory.Buffer = (PVOID)BaseStaticServerData->WindowsDirectory.Buffer;
            BaseWindowsSystemDirectory.Buffer = (PVOID)BaseStaticServerData->WindowsSystemDirectory.Buffer;
#else
            BaseWindowsDirectory.Length =
                WOW64_READ_WORD_FIELD(BaseStaticServerData, BASE_STATIC_SERVER_DATA, WindowsDirectory.Length);
            BaseWindowsSystemDirectory.Length =
                WOW64_READ_WORD_FIELD(BaseStaticServerData, BASE_STATIC_SERVER_DATA, WindowsSystemDirectory.Length);
            BaseWindowsDirectory.MaximumLength =
                WOW64_READ_WORD_FIELD(BaseStaticServerData, BASE_STATIC_SERVER_DATA, WindowsDirectory.MaximumLength);
            BaseWindowsSystemDirectory.MaximumLength =
                WOW64_READ_WORD_FIELD(BaseStaticServerData, BASE_STATIC_SERVER_DATA, WindowsSystemDirectory.MaximumLength);
            BaseWindowsDirectory.Buffer = BaseWindowsDirectoryBuffer;
            BaseWindowsSystemDirectory.Buffer = BaseWindowsSystemDirectoryBuffer;

            ASSERT(BaseWindowsDirectory.Length < MAX_PATH * sizeof(WCHAR));
            ASSERT(BaseWindowsSystemDirectory.Length < MAX_PATH * sizeof(WCHAR));

            Status = NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                                WOW64_READ_PTR_FIELD(
                                                    BaseStaticServerData,
                                                    BASE_STATIC_SERVER_DATA,
                                                    WindowsDirectory.Buffer),
                                                BaseWindowsDirectoryBuffer,
                                                BaseWindowsDirectory.Length,
                                                NULL);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("WOW64: Failed to set up BaseWindowsDirectory\n");
                return FALSE;
            }

            Status = NtWow64ReadVirtualMemory64(NtCurrentProcess(),
                                                WOW64_READ_PTR_FIELD(
                                                    BaseStaticServerData,
                                                    BASE_STATIC_SERVER_DATA,
                                                    WindowsSystemDirectory.Buffer),
                                                BaseWindowsSystemDirectoryBuffer,
                                                BaseWindowsSystemDirectory.Length,
                                                NULL);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("WOW64: Failed to set up BaseWindowsSystemDirectory\n");
                return FALSE;
            }
#endif

            /* Construct the default path (using the static buffer) */
            Status = RtlStringCbPrintfW(BaseDefaultPathBuffer,
                                     sizeof(BaseDefaultPathBuffer),
                                     L"%wZ;%wZ\\system;%wZ;",
                                     &BaseWindowsSystemDirectory,
                                     &BaseWindowsDirectory,
                                     &BaseWindowsDirectory);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("NLS Init failed\n");
                return FALSE;
            }

            BaseDefaultPath.Buffer = BaseDefaultPathBuffer;
            BaseDefaultPath.Length = (USHORT)wcslen(BaseDefaultPathBuffer) * sizeof(WCHAR);
            BaseDefaultPath.MaximumLength = sizeof(BaseDefaultPathBuffer);

            /* Use remaining part of the default path buffer for the append path */
            BaseDefaultPathAppend.Buffer = (PWSTR)((ULONG_PTR)BaseDefaultPathBuffer + BaseDefaultPath.Length);
            BaseDefaultPathAppend.Length = 0;
            BaseDefaultPathAppend.MaximumLength = BaseDefaultPath.MaximumLength - BaseDefaultPath.Length;

            /* Initialize command line */
            InitCommandLines();

            /* Initialize the DLL critical section */
            RtlInitializeCriticalSection(&BaseDllDirectoryLock);

            /* Initialize the National Language Support routines */
            if (!NlsInit())
            {
                DPRINT1("NLS Init failed\n");
                return FALSE;
            }

            /* Initialize Console Support */
            if (!ConDllInitialize(dwReason, SessionDir))
            {
                DPRINT1("Failed to set up console\n");
                return FALSE;
            }

            /* Initialize application certification globals */
            InitializeListHead(&BasepAppCertDllsList);
            RtlInitializeCriticalSection(&gcsAppCert);

            /* Insert more dll attach stuff here! */
            DllInitialized = TRUE;
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            if (DllInitialized != FALSE)
            {
                /* Uninitialize console support */
                ConDllInitialize(dwReason, NULL);

                /* Insert more dll detach stuff here! */
                NlsUninit();

                /* Delete DLL critical section */
                RtlDeleteCriticalSection(&BaseDllDirectoryLock);
            }
            break;
        }

        case DLL_THREAD_ATTACH:
        {
            /* ConDllInitialize sets the current console locale for the new thread */
            return ConDllInitialize(dwReason, NULL);
        }

        default:
            break;
    }

    return TRUE;
}

/* EOF */
