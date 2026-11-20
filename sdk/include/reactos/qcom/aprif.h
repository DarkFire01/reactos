/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Asynchronous Packet Router transport to the audio DSP
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* {7F3A9C21-6E48-4B5D-8C12-2A9E5D7B4C30} */
DEFINE_GUID(GUID_QCOM_APR_INTERFACE,
            0x7f3a9c21, 0x6e48, 0x4b5d, 0x8c, 0x12, 0x2a, 0x9e, 0x5d, 0x7b, 0x4c, 0x30);

/*
 * The APR packet is a 20 byte header followed by the service payload. The
 * layout matches the one the DSP services decode, the header carries the
 * source and destination service and the opcode. All little endian.
 */
#include <pshpack1.h>
typedef struct _APR_HEADER
{
    USHORT HdrField;    /* version, header size in words, message type */
    USHORT PktSize;     /* total packet size in bytes, header included */
    UCHAR SrcSvc;
    UCHAR SrcDomain;
    USHORT SrcPort;
    UCHAR DestSvc;
    UCHAR DestDomain;
    USHORT DestPort;
    ULONG Token;
    ULONG Opcode;
} APR_HEADER, *PAPR_HEADER;
#include <poppack.h>

C_ASSERT(sizeof(APR_HEADER) == 20);

/* HdrField encodes version (low nibble), header words, and message type */
#define APR_PKT_VERSION             0
#define APR_HDR_SIZE_WORDS          (sizeof(APR_HEADER) / 4)
#define APR_MSG_TYPE_EVENT          0
#define APR_MSG_TYPE_CMD_RSP        1
#define APR_MSG_TYPE_SEQ_CMD        2
#define APR_MSG_TYPE_NSEQ_CMD       3

#define APR_HDR_FIELD(MsgType, Words, Ver) \
    (USHORT)(((MsgType) & 0x3) << 8 | ((Words) & 0xF) << 4 | ((Ver) & 0xF))

/* Processor domains */
#define APR_DOMAIN_PC               1
#define APR_DOMAIN_MODEM            2
#define APR_DOMAIN_ADSP             4
#define APR_DOMAIN_APPS             5

/* Core service ids on the DSP */
#define APR_SVC_ADSP_CORE           3
#define APR_SVC_AFE                 4
#define APR_SVC_ASM                 7
#define APR_SVC_ADM                 8

typedef PVOID APR_HANDLE;

/* A registered service receives the inbound packets addressed to it */
typedef VOID
(NTAPI *PQCOM_APR_RECEIVE)(
    _In_ PVOID Context,
    _In_reads_bytes_(Length) const VOID *Packet,
    _In_ ULONG Length);

/*
 * A client registers for one service id on a domain, gets back a handle, and
 * sends APR packets through it. The transport fills the source fields and the
 * header, the client supplies the destination service, opcode and payload.
 */
typedef APR_HANDLE
(NTAPI *PQCOM_APR_REGISTER)(
    _In_ ULONG Domain,
    _In_ ULONG Service,
    _In_ PQCOM_APR_RECEIVE Receive,
    _In_opt_ PVOID Context);

typedef VOID
(NTAPI *PQCOM_APR_UNREGISTER)(
    _In_ APR_HANDLE Handle);

typedef NTSTATUS
(NTAPI *PQCOM_APR_SEND)(
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length);

typedef struct _QCOM_APR_INTERFACE
{
    INTERFACE Header;
    PQCOM_APR_REGISTER Register;
    PQCOM_APR_UNREGISTER Unregister;
    PQCOM_APR_SEND Send;
} QCOM_APR_INTERFACE, *PQCOM_APR_INTERFACE;

/* The transport also exports these directly for in-kernel audio clients */
APR_HANDLE
NTAPI
QcAprRegister(
    _In_ ULONG Domain,
    _In_ ULONG Service,
    _In_ PQCOM_APR_RECEIVE Receive,
    _In_opt_ PVOID Context);

VOID
NTAPI
QcAprUnregister(
    _In_ APR_HANDLE Handle);

/* TRUE once the apr_audio_svc channel to the ADSP is connected */
BOOLEAN
NTAPI
QcAprIsConnected(VOID);

NTSTATUS
NTAPI
QcAprSend(
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length);

/* Same as QcAprSend but the caller sets the source and destination ports,
   which the ASM service uses to carry the session and stream id. */
NTSTATUS
NTAPI
QcAprSendTo(
    _In_ APR_HANDLE Handle,
    _In_ ULONG DestService,
    _In_ ULONG DestPort,
    _In_ ULONG SrcPort,
    _In_ ULONG Opcode,
    _In_ ULONG Token,
    _In_reads_bytes_opt_(Length) const VOID *Payload,
    _In_ ULONG Length);
