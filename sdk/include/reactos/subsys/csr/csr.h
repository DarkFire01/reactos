/*
 * PROJECT:     ReactOS Client/Server Runtime SubSystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Public definitions for CSR Clients
 * COPYRIGHT:   Copyright 2005 Alex Ionescu <alex@relsoft.net>
 *              Copyright 2012-2022 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#ifndef _CSR_H
#define _CSR_H

#include "csrmsg.h"

NTSTATUS
NTAPI
CsrClientConnectToServer(
    _In_ PCWSTR ObjectDirectory,
    _In_ ULONG ServerId,
    _In_ PVOID ConnectionInfo,
    _Inout_ PULONG ConnectionInfoSize,
    _Out_ PBOOLEAN ServerToServerCall);

NTSTATUS
NTAPI
CsrClientCallServer(
    _Inout_ PCSR_API_MESSAGE ApiMessage,
    _Inout_opt_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_ CSR_API_NUMBER ApiNumber,
    _In_ ULONG DataLength);

PCSR_CAPTURE_BUFFER
NTAPI
CsrAllocateCaptureBuffer(
    _In_ ULONG ArgumentCount,
    _In_ ULONG BufferSize);

ULONG
NTAPI
CsrAllocateMessagePointer(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_ ULONG MessageLength,
    _Out_ LPC_PVOID* CapturedData);

VOID
NTAPI
CsrCaptureMessageBuffer(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_opt_ PVOID MessageBuffer,
    _In_ ULONG MessageLength,
    _Out_ LPC_PVOID* CapturedData);
    
#ifdef USE_LPC6432 

static
ULONG
CsrAllocateMessagePointerNative(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_ ULONG MessageLength,
    _Out_ LPC_PVOID* CapturedData)
{
    return CsrAllocateMessagePointer(CaptureBuffer, MessageLength, CapturedData);
}

static
VOID
CsrCaptureMessageBufferNative(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_opt_ PVOID MessageBuffer,
    _In_ ULONG MessageLength,
    _Out_ LPC_PVOID* CapturedData)
{
    CsrCaptureMessageBuffer(CaptureBuffer, MessageBuffer, MessageLength, CapturedData);
}

static
ULONG
CsrAllocateMessagePointer32(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_ ULONG MessageLength,
    _Out_ PVOID* CapturedData)
{
    LPC_PVOID Temp;
    ULONG Result;
    
    Result = CsrAllocateMessagePointer(CaptureBuffer, MessageLength, &Temp);
    ASSERT((((ULONGLONG)Temp) & 0xFFFFFFFF00000000ULL) == 0);
    
    *CapturedData = (PVOID)Temp;
    return Result;
}

static
VOID
CsrCaptureMessageBuffer32(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_opt_ PVOID MessageBuffer,
    _In_ ULONG MessageLength,
    _Out_ PVOID* CapturedData)
{
    LPC_PVOID Temp;
    CsrCaptureMessageBuffer(CaptureBuffer, MessageBuffer, MessageLength, &Temp);
    ASSERT((((ULONGLONG)Temp) & 0xFFFFFFFF00000000ULL) == 0);
    *CapturedData = (PVOID)Temp;
}

#define CsrAllocateMessagePointer CsrAllocateMessagePointer32
#define CsrCaptureMessageBuffer CsrCaptureMessageBuffer32

#else
    
#define CsrAllocateMessagePointerNative CsrAllocateMessagePointer
#define CsrCaptureMessageBufferNative CsrCaptureMessageBuffer

#endif

VOID
NTAPI
CsrFreeCaptureBuffer(
    _In_ _Frees_ptr_ PCSR_CAPTURE_BUFFER CaptureBuffer);

VOID
NTAPI
CsrCaptureMessageString(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _In_opt_ PCSTR String,
    _In_ ULONG StringLength,
    _In_ ULONG MaximumLength,
    _Out_ PSTRING CapturedString);

VOID
NTAPI
CsrCaptureMessageUnicodeStringInPlace(
    _Inout_ PCSR_CAPTURE_BUFFER CaptureBuffer,
    _Inout_ PUNICODE_STRING String);

NTSTATUS
NTAPI
CsrCaptureMessageMultiUnicodeStringsInPlace(
    _Inout_ PCSR_CAPTURE_BUFFER* CaptureBuffer,
    _In_ ULONG StringsCount,
    _In_ PUNICODE_STRING* MessageStrings);

PLARGE_INTEGER
NTAPI
CsrCaptureTimeout(
    _In_ ULONG Milliseconds,
    _Out_ PLARGE_INTEGER Timeout);

VOID
NTAPI
CsrProbeForRead(
    _In_ PVOID Address,
    _In_ ULONG Length,
    _In_ ULONG Alignment);

VOID
NTAPI
CsrProbeForWrite(
    _In_ PVOID Address,
    _In_ ULONG Length,
    _In_ ULONG Alignment);

HANDLE
NTAPI
CsrGetProcessId(VOID);

NTSTATUS
NTAPI
CsrNewThread(VOID);

NTSTATUS
NTAPI
CsrIdentifyAlertableThread(VOID);

NTSTATUS
NTAPI
CsrSetPriorityClass(
    _In_ HANDLE Process,
    _Inout_ PULONG PriorityClass);

#endif // _CSR_H

/* EOF */
