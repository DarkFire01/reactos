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

//
// WPP extended format specifiers, written %!NAME!, are not printf conversions.
// The trace decoder expands them from the driver's .tmf file using the value
// WPP recorded next to them - %!STATUS! becomes the symbolic name of an
// NTSTATUS, %!IRQL! the name of an IRQL, and so on.
//
// Handing one to RtlStringCbVPrintfA instead does two things, and the second
// one is the expensive one. The name is left sitting in the text, which is
// where every "!STATUS!" in the log comes from. And no argument is consumed
// for it - so in a format like "%!STATUS!, info 0x%x" the %x reads the status
// and the value that was meant to be printed is never seen at all. There are
// 23 such formats in this tree, and better than a thousand traces whose only
// interesting content is a status that never reaches the log.
//
// So rewrite them into ordinary conversions before formatting. Each %!NAME!
// carries exactly one argument, promoted to int by the varargs call, except
// %!FUNC!, which carries none - WPP substitutes the calling function's name
// there, and that is not recoverable from here.
//
#define FX_WPP_MAX_SPECIFIER 64

static
BOOLEAN
FxWppNameIs(
    __in PCSTR Name,
    __in PCSTR Match
    )
{
    while ((*Name != '\0') && (*Match != '\0'))
    {
        CHAR a = *Name++;
        CHAR b = *Match++;

        if ((a >= 'a') && (a <= 'z')) a = (CHAR)(a - 'a' + 'A');
        if ((b >= 'a') && (b <= 'z')) b = (CHAR)(b - 'a' + 'A');

        if (a != b)
        {
            return FALSE;
        }
    }

    return (*Name == '\0') && (*Match == '\0');
}

static
PCSTR
FxExpandWppSpecifiers(
    __in PCSTR DebugMessage,
    __in PSTR Buffer,
    __in size_t BufferCb
    )
{
    PCSTR  src = DebugMessage;
    size_t out = 0;

    if (BufferCb == 0)
    {
        return DebugMessage;
    }

    while (*src != '\0')
    {
        CHAR   name[FX_WPP_MAX_SPECIFIER];
        PCSTR  end;
        PCSTR  replacement;
        size_t nameLength;
        size_t replacementLength;

        //
        // Anything that is not the start of a specifier is copied through.
        // "%%" is an escaped per cent and must be stepped over as a pair, or
        // the "!" of a following "%%!" would be mistaken for one.
        //
        if (src[0] != '%')
        {
            if (out + 1 >= BufferCb) break;
            Buffer[out++] = *src++;
            continue;
        }

        if (src[1] == '%')
        {
            if (out + 2 >= BufferCb) break;
            Buffer[out++] = *src++;
            Buffer[out++] = *src++;
            continue;
        }

        if (src[1] != '!')
        {
            if (out + 1 >= BufferCb) break;
            Buffer[out++] = *src++;
            continue;
        }

        //
        // "%!NAME!" - find the closing delimiter. An unterminated or absurdly
        // long run is not a specifier; copy the per cent and carry on.
        //
        end = src + 2;
        while ((*end != '\0') && (*end != '!'))
        {
            end++;
        }

        nameLength = (size_t)(end - (src + 2));
        if ((*end != '!') || (nameLength == 0) || (nameLength >= sizeof(name)))
        {
            if (out + 1 >= BufferCb) break;
            Buffer[out++] = *src++;
            continue;
        }

        RtlCopyMemory(name, src + 2, nameLength);
        name[nameLength] = '\0';

        if (FxWppNameIs(name, "FUNC"))
        {
            /* The one specifier that takes no argument */
            replacement = "<function>";
        }
        else if (FxWppNameIs(name, "STATUS") ||
                 FxWppNameIs(name, "HRESULT") ||
                 FxWppNameIs(name, "WINERR") ||
                 FxWppNameIs(name, "WINERROR"))
        {
            replacement = "0x%08X";
        }
        else if (FxWppNameIs(name, "wZ"))
        {
            replacement = "%wZ";
        }
        else if (FxWppNameIs(name, "GUID"))
        {
            replacement = "%p";
        }
        else
        {
            //
            // Everything else names an enumeration or a small integer - IRQL,
            // BOOLEAN, IRPMJ, the Fx* state and event enums. Their names are
            // in the .tmf and not here, so print the value; one argument
            // either way, which is what keeps the rest of the line aligned.
            //
            replacement = "0x%x";
        }

        replacementLength = 0;
        while (replacement[replacementLength] != '\0')
        {
            replacementLength++;
        }

        if (out + replacementLength >= BufferCb) break;

        RtlCopyMemory(Buffer + out, replacement, replacementLength);
        out += replacementLength;
        src = end + 1;
    }

    Buffer[out] = '\0';
    return Buffer;
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
    PCSTR      format;
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) {

        //
        // Turn the WPP specifiers into printf conversions first, so that the
        // values behind them are printed and the arguments after them line up
        //
        format = FxExpandWppSpecifiers(DebugMessage,
                                       formatBuffer,
                                       sizeof(formatBuffer));

        //
        // Using new safe string functions instead of _vsnprintf.
        // This function takes care of NULL terminating if the message
        // is longer than the buffer.
        //
#if FX_CORE_MODE==FX_CORE_KERNEL_MODE
        status = RtlStringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      format,
                                      list );
#else
        HRESULT hr;
        hr = StringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      format,
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
