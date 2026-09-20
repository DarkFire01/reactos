// Copyright (c) 2004, Antony C. Roberts

// Use of this file is subject to the terms
// described in the LICENSE.TXT file that
// accompanies this file.
//
// Your use of this file indicates your
// acceptance of the terms described in
// LICENSE.TXT.
//
// http://www.freebt.net

#ifndef _FREEBT_RWR_H
#define _FREEBT_RWR_H

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS
NTAPI
FreeBT_TransferCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context);

NTSTATUS
NTAPI
FreeBT_SubmitTransfer(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PUSBD_PIPE_INFORMATION Pipe,
    _In_ ULONG TransferFlags,
    _In_ ULONG TransferLength);

NTSTATUS
NTAPI
FreeBT_DispatchRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp);

NTSTATUS
NTAPI
FreeBT_DispatchWrite(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp);

#ifdef __cplusplus
};
#endif

#endif
