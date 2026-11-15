/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     CoreMessaging style message session (endpoints, registrar, send)
 */

#pragma once

#include <windows.h>

#ifdef COREMSG_EXPORTS
#define COREMSG_API __declspec(dllexport)
#else
#define COREMSG_API __declspec(dllimport)
#endif

typedef ULONG MSG_ENDPOINT, *PMSG_ENDPOINT;

#define MSG_ENDPOINT_NONE 0

typedef enum _MSG_PRIORITY
{
    MsgPriorityNormal = 0,
    MsgPriorityHigh = 1
} MSG_PRIORITY;

/* Delivered on the thread that calls Dispatch, or inline for SendSync */
typedef HRESULT (CALLBACK *PMSG_CALLBACK)(
    _In_opt_ PVOID Context,
    _In_reads_bytes_(Size) const VOID *Payload,
    _In_ ULONG Size);

/* Called from Send when the queue goes from empty to non empty */
typedef VOID (CALLBACK *PMSG_WAKE)(
    _In_opt_ PVOID Context);

class IMessageSession
{
public:
    virtual ULONG STDMETHODCALLTYPE AddRef(VOID) = 0;
    virtual ULONG STDMETHODCALLTYPE Release(VOID) = 0;

    virtual HRESULT STDMETHODCALLTYPE CreateEndpoint(
        _In_ PMSG_CALLBACK Callback,
        _In_opt_ PVOID Context,
        _Out_ PMSG_ENDPOINT Endpoint) = 0;

    virtual HRESULT STDMETHODCALLTYPE CloseEndpoint(
        _In_ MSG_ENDPOINT Endpoint) = 0;

    virtual HRESULT STDMETHODCALLTYPE PublishEndpoint(
        _In_z_ PCWSTR Name,
        _In_ MSG_ENDPOINT Endpoint) = 0;

    virtual HRESULT STDMETHODCALLTYPE OpenEndpoint(
        _In_z_ PCWSTR Name,
        _Out_ PMSG_ENDPOINT Endpoint) = 0;

    /* Copies the payload and queues it */
    virtual HRESULT STDMETHODCALLTYPE Send(
        _In_ MSG_ENDPOINT Target,
        _In_ MSG_PRIORITY Priority,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size) = 0;

    /* Delivers inline, for requests that carry an output pointer */
    virtual HRESULT STDMETHODCALLTYPE SendSync(
        _In_ MSG_ENDPOINT Target,
        _In_reads_bytes_(Size) const VOID *Payload,
        _In_ ULONG Size) = 0;

    virtual VOID STDMETHODCALLTYPE SetWake(
        _In_opt_ PMSG_WAKE Wake,
        _In_opt_ PVOID Context) = 0;

    /* Drains the queue, returns the number of messages delivered */
    virtual ULONG STDMETHODCALLTYPE Dispatch(VOID) = 0;
};

EXTERN_C
COREMSG_API
HRESULT
WINAPI
CoreMsgCreateSession(
    _Outptr_ IMessageSession **Session);
