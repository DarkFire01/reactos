#include "ros_wow64_private.h"

#include <csr/csr.h>

typedef struct _WOW64_CSR_CAPTURE_BUFFER
{
    PCSR_CAPTURE_BUFFER pNativeBuffer;
    LONG_PTR BufferStartOffset;
    SIZE_T BufferSize;
    BYTE Data[ANYSIZE_ARRAY];
} WOW64_CSR_CAPTURE_BUFFER, *PWOW64_CSR_CAPTURE_BUFFER;

ULONG 
NTAPI
wow64_NtWow64CsrAllocateCaptureBuffer(UINT* pArgs)
{
    PWOW64_CSR_CAPTURE_BUFFER result;

    ULONG ArgumentCount = get_ulong(&pArgs);
    ULONG BufferSize = get_ulong(&pArgs);

    result = RtlAllocateHeap(RtlGetProcessHeap(), 0, offsetof(WOW64_CSR_CAPTURE_BUFFER, Data[BufferSize]));
    if (result == NULL)
    {
        return 0;
    }

    RtlZeroMemory(&result->Data, BufferSize);

    result->pNativeBuffer = CsrAllocateCaptureBuffer(ArgumentCount, BufferSize);
    if (result->pNativeBuffer == NULL) 
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, result);
        return 0;
    }

    result->BufferStartOffset = (ULONG_PTR)result->pNativeBuffer->BufferEnd - (ULONG_PTR)result->pNativeBuffer;
    result->BufferSize = BufferSize;

    return PtrToUlong(result);
}

static
VOID
Wow64CsrCapturePtrHelper(PWOW64_CSR_CAPTURE_BUFFER pBuffer, ULONG* pData32, PVOID Data, ULONG Size)
{
    ASSERT(Data != NULL && pData32 != NULL);
    *pData32 = (ULONG_PTR)pBuffer->Data + (ULONG_PTR)Data - pBuffer->BufferStartOffset;

    RtlCopyMemory(UlongToPtr(*pData32), Data, Size);
}

ULONG 
NTAPI
wow64_NtWow64CsrAllocateMessagePointer(UINT* pArgs)
{
    PWOW64_CSR_CAPTURE_BUFFER pBuffer = get_ptr(&pArgs);
    ULONG MessageSize = get_ulong(&pArgs);
    ULONG* pData32 = get_ptr(&pArgs);
    
    ULONG result;
    PVOID Data;

    result = CsrAllocateMessagePointer(pBuffer->pNativeBuffer, MessageSize, &Data);
    Wow64CsrCapturePtrHelper(pBuffer, pData32, Data, MessageSize);
    return result;
}

VOID 
NTAPI
wow64_NtWow64CsrCaptureMessageBuffer(UINT* pArgs)
{
    PWOW64_CSR_CAPTURE_BUFFER pBuffer = get_ptr(&pArgs);
    PVOID Message = get_ptr(&pArgs);
    ULONG MessageSize = get_ulong(&pArgs);
    ULONG* pData32 = get_ptr(&pArgs);

    PVOID Data;

    CsrCaptureMessageBuffer(pBuffer->pNativeBuffer, Message, MessageSize, &Data);
    Wow64CsrCapturePtrHelper(pBuffer, pData32, Data, MessageSize);
}

VOID 
NTAPI
wow64_NtWow64CsrCaptureMessageString(UINT* pArgs)
{
    PWOW64_CSR_CAPTURE_BUFFER pBuffer = get_ptr(&pArgs);
    PCSTR String = get_ptr(&pArgs);
    ULONG StringLength = get_ulong(&pArgs);
    ULONG MaximumLength = get_ulong(&pArgs);
    PSTRING32 CapturedString = get_ptr(&pArgs);

    STRING DataStr;
    
    CsrCaptureMessageString(pBuffer->pNativeBuffer, String, StringLength, MaximumLength, &DataStr);
    Wow64CsrCapturePtrHelper(pBuffer, &CapturedString->Buffer, DataStr.Buffer, StringLength);
    CapturedString->MaximumLength = DataStr.MaximumLength;
    CapturedString->Length = DataStr.Length;
}

NTSTATUS 
NTAPI
wow64_NtWow64CsrClientCallServer(UINT* pArgs)
{
    PCSR_API_MESSAGE ApiMessage = get_ptr(&pArgs);
    PWOW64_CSR_CAPTURE_BUFFER pBuffer = get_ptr(&pArgs);
    CSR_API_NUMBER ApiNumber = get_ulong(&pArgs);
    ULONG DataLength = get_ulong(&pArgs);

    NTSTATUS status;
    PVOID NativeBufferData = NULL; 
    
    if (pBuffer != NULL) 
    {
        NativeBufferData = (PVOID)((ULONG_PTR)pBuffer->pNativeBuffer + pBuffer->BufferStartOffset);
        RtlCopyMemory(NativeBufferData, &pBuffer->Data, pBuffer->BufferSize);
    }

    status = CsrClientCallServer(ApiMessage, pBuffer ? pBuffer->pNativeBuffer : NULL, ApiNumber, DataLength);
    
    if (pBuffer != NULL)
    {
        RtlCopyMemory(&pBuffer->Data, NativeBufferData, pBuffer->BufferSize);
    }

    return status;
}

NTSTATUS 
NTAPI
wow64_NtWow64CsrClientConnectToServer(UINT* pArgs)
{
    PCWSTR ObjectDirectory = get_ptr(&pArgs);
    ULONG ServerId = get_ulong(&pArgs);
    PVOID ConnectionInfo = get_ptr(&pArgs);
    PULONG ConnectionInfoSize = get_ptr(&pArgs);
    PBOOLEAN ServerToServerCall = get_ptr(&pArgs);

    return CsrClientConnectToServer(ObjectDirectory, ServerId, ConnectionInfo, ConnectionInfoSize, ServerToServerCall);
}

VOID 
NTAPI
wow64_NtWow64CsrFreeCaptureBuffer(UINT* pArgs)
{
    PWOW64_CSR_CAPTURE_BUFFER pBuffer = get_ptr(&pArgs);

    if (pBuffer == NULL)
    {
        return;
    }

    CsrFreeCaptureBuffer(pBuffer->pNativeBuffer);
    RtlFreeHeap(RtlGetProcessHeap(), 0, pBuffer);
}

VOID 
NTAPI
wow64_NtWow64CsrProbeForRead(UINT* pArgs)
{
    PVOID Address = get_ptr(&pArgs);
    ULONG Length = get_ulong(&pArgs);
    ULONG Alignment = get_ulong(&pArgs);

    CsrProbeForRead(Address, Length, Alignment);
}


VOID 
NTAPI
wow64_NtWow64CsrProbeForWrite(UINT* pArgs)
{
    PVOID Address = get_ptr(&pArgs);
    ULONG Length = get_ulong(&pArgs);
    ULONG Alignment = get_ulong(&pArgs);

    CsrProbeForWrite(Address, Length, Alignment);
}


HANDLE 
NTAPI
wow64_NtWow64CsrGetProcessId(UINT* pArgs)
{
    return CsrGetProcessId();
}

NTSTATUS
NTAPI
wow64_NtWow64CsrNewThread(UINT* pArgs)
{
    return CsrNewThread();
}

NTSTATUS
NTAPI
wow64_NtWow64CsrIdentifyAlertableThread(UINT* pArgs)
{
    return CsrIdentifyAlertableThread();
}

NTSTATUS
NTAPI
wow64_NtWow64CsrSetPriorityClass(UINT* pArgs)
{
    HANDLE Process = get_handle(&pArgs);
    PULONG PriorityClass = get_ptr(&pArgs);

    return CsrSetPriorityClass(Process, PriorityClass);
}

NTSTATUS
NTAPI
wow64_NtWow64CsrCaptureMessageMultiUnicodeStringsInPlace(UINT* pArgs)
{
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
wow64_NtWow64CsrCaptureTimeout(UINT* pArgs)
{
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}