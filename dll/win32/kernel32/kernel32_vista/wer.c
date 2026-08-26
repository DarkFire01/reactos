/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Windows Error Reporting registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * These are the calls a process makes to say what should be collected if it
 * ever crashes: extra files and memory blocks to put in the dump, a DLL for
 * WER to load out of process to handle the exception, metadata to tag the
 * report with. There is no error reporting service here to collect any of it.
 *
 * They are still worth having, and the reason is what happens without them.
 * These names were `stub` spec entries, which means two different bad things
 * at once. The ones marked `-stub` raised EXCEPTION_WINE_STUB the moment they
 * were called - fatal to a caller that does not expect it. The plain `stub`
 * ones were not exported at all, so a module importing them statically failed
 * to load: `Failed to snap KERNEL32.dll!WerUnregisterRuntimeExceptionModule
 * for mozglue.dll` is the loader refusing to start Firefox over one of them.
 *
 * So they answer, and they answer S_OK. That is not the same as pretending
 * the work was done - nothing in the system can observe whether a report was
 * ever registered, because nothing here produces reports. What a caller does
 * with a failure, on the other hand, is very observable: it either treats
 * crash reporting as broken or gives up entirely. Accepting the registration
 * and discarding it leaves the process running with slightly poorer crash
 * dumps, which is the outcome we want.
 *
 * WerSetFlags and WerGetFlags are the exception, because they are the only
 * pair here where a caller can see what we did: Get has an out parameter, and
 * a caller that sets flags and reads them back is entitled to get them back.
 * Those are kept for real. Never return success from one of these without
 * writing the out parameter - a caller that reads its own uninitialised stack
 * and believes it is a flags word is far worse off than one told no.
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

#include <werapi.h>

/*
 * The flags last handed to WerSetFlags. Windows keeps these per process, and
 * so do we - see WerGetFlags for what that costs.
 */
static DWORD gWerFlags = 0;

#define WER_FAULT_REPORTING_VALID_FLAGS         \
    (WER_FAULT_REPORTING_FLAG_NOHEAP |          \
     WER_FAULT_REPORTING_FLAG_QUEUE |           \
     WER_FAULT_REPORTING_FLAG_DISABLE_THREAD_SUSPENSION | \
     WER_FAULT_REPORTING_FLAG_QUEUE_UPLOAD |    \
     WER_FAULT_REPORTING_ALWAYS_SHOW_UI |       \
     WER_FAULT_REPORTING_NO_UI |                \
     WER_FAULT_REPORTING_FLAG_NO_HEAP_ON_QUEUE |\
     WER_FAULT_REPORTING_DISABLE_SNAPSHOT_CRASH |\
     WER_FAULT_REPORTING_DISABLE_SNAPSHOT_HANG |\
     WER_FAULT_REPORTING_CRITICAL |             \
     WER_FAULT_REPORTING_DURABLE)

/*
 * @implemented
 */
HRESULT
WINAPI
WerSetFlags(
    _In_ DWORD dwFlags)
{
    DPRINT("WerSetFlags(0x%lx)\n", dwFlags);

    if (dwFlags & ~WER_FAULT_REPORTING_VALID_FLAGS)
        return E_INVALIDARG;

    gWerFlags = dwFlags;
    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
WerGetFlags(
    _In_ HANDLE hProcess,
    _Out_ PDWORD pdwFlags)
{
    DPRINT("WerGetFlags(%p, %p)\n", hProcess, pdwFlags);

    if (pdwFlags == NULL)
        return E_INVALIDARG;

    /*
     * Windows answers for the process named by the handle. We only keep our
     * own, so a caller asking about somebody else is told what we set for
     * ourselves rather than refused - the flags only steer report collection
     * that does not happen here either way.
     */
    UNREFERENCED_PARAMETER(hProcess);

    *pdwFlags = gWerFlags;
    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterFile(
    _In_ PCWSTR pwzFile,
    _In_ WER_REGISTER_FILE_TYPE regFileType,
    _In_ DWORD dwFlags)
{
    DPRINT("WerRegisterFile(%S, %d, 0x%lx) - accepted, not recorded\n",
           pwzFile, regFileType, dwFlags);

    if (pwzFile == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterFile(
    _In_ PCWSTR pwzFilePath)
{
    DPRINT("WerUnregisterFile(%S)\n", pwzFilePath);

    if (pwzFilePath == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterMemoryBlock(
    _In_ PVOID pvAddress,
    _In_ DWORD dwSize)
{
    DPRINT("WerRegisterMemoryBlock(%p, %lu) - accepted, not recorded\n",
           pvAddress, dwSize);

    if (pvAddress == NULL || dwSize == 0)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterMemoryBlock(
    _In_ PVOID pvAddress)
{
    DPRINT("WerUnregisterMemoryBlock(%p)\n", pvAddress);

    if (pvAddress == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterExcludedMemoryBlock(
    _In_ const void *pvAddress,
    _In_ DWORD dwSize)
{
    DPRINT("WerRegisterExcludedMemoryBlock(%p, %lu) - accepted, not recorded\n",
           pvAddress, dwSize);

    if (pvAddress == NULL || dwSize == 0)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterExcludedMemoryBlock(
    _In_ const void *pvAddress)
{
    DPRINT("WerUnregisterExcludedMemoryBlock(%p)\n", pvAddress);

    if (pvAddress == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterRuntimeExceptionModule(
    _In_ PCWSTR pwszOutOfProcessCallbackDll,
    _In_ PVOID pContext)
{
    DPRINT("WerRegisterRuntimeExceptionModule(%S, %p) - accepted, not recorded\n",
           pwszOutOfProcessCallbackDll, pContext);

    if (pwszOutOfProcessCallbackDll == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterRuntimeExceptionModule(
    _In_ PCWSTR pwszOutOfProcessCallbackDll,
    _In_ PVOID pContext)
{
    DPRINT("WerUnregisterRuntimeExceptionModule(%S, %p)\n",
           pwszOutOfProcessCallbackDll, pContext);

    if (pwszOutOfProcessCallbackDll == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterCustomMetadata(
    _In_ PCWSTR pwszKey,
    _In_ PCWSTR pwszValue)
{
    DPRINT("WerRegisterCustomMetadata(%S, %S) - accepted, not recorded\n",
           pwszKey, pwszValue);

    if (pwszKey == NULL || pwszValue == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterCustomMetadata(
    _In_ PCWSTR pwszKey)
{
    DPRINT("WerUnregisterCustomMetadata(%S)\n", pwszKey);

    if (pwszKey == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterAdditionalProcess(
    _In_ DWORD dwProcessId,
    _In_ DWORD dwThreadId)
{
    DPRINT("WerRegisterAdditionalProcess(%lu, %lu) - accepted, not recorded\n",
           dwProcessId, dwThreadId);

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterAdditionalProcess(
    _In_ DWORD dwProcessId)
{
    DPRINT("WerUnregisterAdditionalProcess(%lu)\n", dwProcessId);

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerRegisterAppLocalDump(
    _In_ PCWSTR pwzLocalAppDataRelativePath)
{
    DPRINT("WerRegisterAppLocalDump(%S) - accepted, not recorded\n",
           pwzLocalAppDataRelativePath);

    if (pwzLocalAppDataRelativePath == NULL)
        return E_INVALIDARG;

    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
WerUnregisterAppLocalDump(VOID)
{
    DPRINT("WerUnregisterAppLocalDump()\n");

    return S_OK;
}

/* EOF */
