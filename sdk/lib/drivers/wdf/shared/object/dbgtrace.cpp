/*++

Copyright (c) Microsoft Corporation

ModuleName:

    DbgTrace.cpp

Abstract:

    Temporary file to be used until ETW can be used
    for UM

Author:



Revision History:



--*/

#include "fxobjectpch.hpp"

#if FX_CORE_MODE==FX_CORE_USER_MODE
#include "strsafe.h"
#endif

#if !defined(EVENT_TRACING)

/**
 * @brief
 * Rewrites WPP %!NAME! specifiers into printf conversions, so the format and
 * the argument list stay in step. Each replacement is shorter than the original.
 *
 * @param[in] DebugMessage
 * The message as the caller wrote it.
 *
 * @param[out] Buffer
 * Receives the rewritten message.
 *
 * @param[in] BufferLength
 * Size of Buffer in bytes.
 */
static
VOID
FxTraceSanitizeFormat(
    _In_z_ PCSTR DebugMessage,
    _Out_writes_z_(BufferLength) PSTR Buffer,
    _In_ SIZE_T BufferLength
    )
{
    SIZE_T In = 0;
    SIZE_T Out = 0;

    while ((DebugMessage[In] != '\0') && (Out + 1 < BufferLength)) {

        if ((DebugMessage[In] == '%') && (DebugMessage[In + 1] == '%')) {
            //
            // Step over an escaped percent, so a ! behind it is left alone
            //
            Buffer[Out++] = DebugMessage[In++];
            if (Out + 1 >= BufferLength) {
                break;
            }
            Buffer[Out++] = DebugMessage[In++];
            continue;
        }

        if ((DebugMessage[In] == '%') && (DebugMessage[In + 1] == '!')) {
            SIZE_T Name = In + 2;
            SIZE_T End = Name;
            SIZE_T NameLength;
            PCSTR Replacement;

            while ((DebugMessage[End] != '\0') && (DebugMessage[End] != '!')) {
                End++;
            }

            if (DebugMessage[End] != '!') {
                //
                // Unterminated, so it was never a specifier
                //
                Buffer[Out++] = DebugMessage[In++];
                continue;
            }

            NameLength = End - Name;

            if ((NameLength == 4) &&
                (RtlCompareMemory(&DebugMessage[Name], "FUNC", 4) == 4)) {
                //
                // The preprocessor fills this one in and takes no argument
                //
                Replacement = "";
            }
            else if ((NameLength == 4) &&
                     (RtlCompareMemory(&DebugMessage[Name], "GUID", 4) == 4)) {
                Replacement = "%p";
            }
            else {
                //
                // The rest are status codes and enumerations, all ULONG wide
                //
                Replacement = "%x";
            }

            while ((*Replacement != '\0') && (Out + 1 < BufferLength)) {
                Buffer[Out++] = *Replacement++;
            }

            In = End + 1;
            continue;
        }

        Buffer[Out++] = DebugMessage[In++];
    }

    Buffer[Out] = '\0';
}

VOID
__cdecl
DoTraceLevelMessage(
    __in PVOID FxDriverGlobals,
    __in ULONG   DebugPrintLevel,
    __in ULONG   DebugPrintFlag,
    __drv_formatString(FormatMessage)
    __in PCSTR   DebugMessage,
    ...
    )

/*++

Routine Description:

    Print the trace message to debugger.

Arguments:

    TraceEventsLevel - print level between 0 and 3, with 3 the most verbose

Return Value:

    None.

 --*/
 {
#if DBG
    UNREFERENCED_PARAMETER(FxDriverGlobals);

#define     TEMP_BUFFER_SIZE        1024
    va_list    list;
    CHAR       debugMessageBuffer[TEMP_BUFFER_SIZE];
    CHAR       formatBuffer[TEMP_BUFFER_SIZE];
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) {

        FxTraceSanitizeFormat(DebugMessage, formatBuffer, sizeof(formatBuffer));

        //
        // Using new safe string functions instead of _vsnprintf.
        // This function takes care of NULL terminating if the message
        // is longer than the buffer.
        //
#if FX_CORE_MODE==FX_CORE_KERNEL_MODE
        status = RtlStringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      formatBuffer,
                                      list );
#else
        HRESULT hr;
        hr = StringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      formatBuffer,
                                      list );


        if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        {
            status = WinErrorToNtStatus(HRESULT_CODE(hr));
        }
        else
        {
            status = SUCCEEDED(hr) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        }
#endif
        if(!NT_SUCCESS(status)) {

#if FX_CORE_MODE==FX_CORE_KERNEL_MODE
            DbgPrint ("WDFTrace: RtlStringCbVPrintfA failed 0x%x\n", status);
#else
            OutputDebugString("WDFTrace: Unable to expand: ");
            OutputDebugString(DebugMessage);
#endif
            return;
        }
        if (DebugPrintLevel <= TRACE_LEVEL_ERROR ||
            (DebugPrintLevel <= DebugLevel &&
             ((DebugPrintFlag & DebugFlag) == DebugPrintFlag))) {
#if FX_CORE_MODE==FX_CORE_KERNEL_MODE
            DbgPrint("WDFTrace: %s\n", debugMessageBuffer);
#else
            OutputDebugString("WDFTrace: ");
            OutputDebugString(DebugMessage);
#endif
        }
    }
    va_end(list);

    return;
#else
    UNREFERENCED_PARAMETER(FxDriverGlobals);
    UNREFERENCED_PARAMETER(DebugPrintLevel);
    UNREFERENCED_PARAMETER(DebugPrintFlag);
    UNREFERENCED_PARAMETER(DebugMessage);
#endif
}

#endif
