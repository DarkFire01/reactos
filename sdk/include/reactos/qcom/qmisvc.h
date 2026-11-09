/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI service and client framework over a pluggable transport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <qcom/qmi.h>

/* A port on the router, the pair every message is addressed by */
typedef struct _QMI_ADDR
{
    ULONG Node;
    ULONG Port;
} QMI_ADDR, *PQMI_ADDR;

typedef struct _QMI_TRANSPORT QMI_TRANSPORT, *PQMI_TRANSPORT;

/*
 * The transport is whatever carries QMI messages between ports, normally the
 * IPC router. The binding fills Ops, and calls QmiTransportDeliver for each
 * inbound message. Everything above here is transport independent.
 */
typedef NTSTATUS
(*PQMI_TP_SEND)(
    _In_ PQMI_TRANSPORT Transport,
    _In_ const QMI_ADDR *Destination,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

typedef NTSTATUS
(*PQMI_TP_SERVICE)(
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG Service,
    _In_ ULONG Version,
    _In_ ULONG Instance);

typedef NTSTATUS
(*PQMI_TP_LOOKUP)(
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG Service,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _Out_ PQMI_ADDR Address);

typedef struct _QMI_TRANSPORT_OPS
{
    PQMI_TP_SEND Send;
    PQMI_TP_SERVICE RegisterService;
    PQMI_TP_SERVICE UnregisterService;
    PQMI_TP_LOOKUP LookupService;
} QMI_TRANSPORT_OPS, *PQMI_TRANSPORT_OPS;

typedef VOID
(*PQMI_DELIVER)(
    _In_ PVOID Sink,
    _In_ const QMI_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length);

struct _QMI_TRANSPORT
{
    const QMI_TRANSPORT_OPS *Ops;
    PVOID Context;
    PVOID Sink;
    PQMI_DELIVER Deliver;
};

/* Called by the binding at PASSIVE_LEVEL for every inbound message */
FORCEINLINE
VOID
QmiTransportDeliver(
    _In_ PQMI_TRANSPORT Transport,
    _In_ const QMI_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    if (Transport->Deliver)
        Transport->Deliver(Transport->Sink, Source, Message, Length);
}

/* ---- Service (answers requests from the modem) ---- */

typedef struct _QMI_SERVICE QMI_SERVICE, *PQMI_SERVICE;

/*
 * Handler for one request. The response encoder is already started with the
 * matching transaction and message id, so the handler only adds the result
 * record and any payload. A failure return makes the framework send a
 * failure result instead.
 */
typedef NTSTATUS
(*PQMI_REQUEST_HANDLER)(
    _In_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Inout_ PQMI_ENCODER Response,
    _In_opt_ PVOID Context);

struct _QMI_SERVICE
{
    PQMI_TRANSPORT Transport;
    ULONG ServiceId;
    ULONG Version;
    ULONG Instance;
    PQMI_REQUEST_HANDLER Handler;
    PVOID HandlerContext;
    KSPIN_LOCK Lock;
    LIST_ENTRY Clients;
};

NTSTATUS
QmiServiceCreate(
    _Out_ PQMI_SERVICE Service,
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG ServiceId,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _In_ PQMI_REQUEST_HANDLER Handler,
    _In_opt_ PVOID Context);

VOID
QmiServiceDestroy(
    _Inout_ PQMI_SERVICE Service);

NTSTATUS
QmiServiceSendIndication(
    _In_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(BodyLength) const VOID *Body,
    _In_ ULONG BodyLength);

/* ---- Client (calls a service the modem exposes) ---- */

typedef struct _QMI_CLIENT QMI_CLIENT, *PQMI_CLIENT;

typedef VOID
(*PQMI_INDICATION_HANDLER)(
    _In_ PQMI_CLIENT Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_opt_ PVOID Context);

struct _QMI_CLIENT
{
    PQMI_TRANSPORT Transport;
    QMI_ADDR Server;
    USHORT NextTransaction;
    KSPIN_LOCK Lock;
    LIST_ENTRY Pending;
    PQMI_INDICATION_HANDLER IndicationHandler;
    PVOID IndicationContext;
};

NTSTATUS
QmiClientConnect(
    _Out_ PQMI_CLIENT Client,
    _In_ PQMI_TRANSPORT Transport,
    _In_ ULONG ServiceId,
    _In_ ULONG Version,
    _In_ ULONG Instance,
    _In_opt_ PQMI_INDICATION_HANDLER IndicationHandler,
    _In_opt_ PVOID Context);

VOID
QmiClientDisconnect(
    _Inout_ PQMI_CLIENT Client);

/*
 * Request carries a full message the caller built with QmiEncodeBegin
 * (type and transaction are overwritten here). The response message, header
 * included, is copied into Response and parsed with the codec find helpers.
 */
NTSTATUS
QmiClientRequest(
    _In_ PQMI_CLIENT Client,
    _Inout_updates_bytes_(RequestLength) PVOID Request,
    _In_ ULONG RequestLength,
    _Out_writes_bytes_to_(ResponseCapacity, *ResponseLength) PVOID Response,
    _In_ ULONG ResponseCapacity,
    _Out_ PULONG ResponseLength,
    _In_ ULONG TimeoutMs);
