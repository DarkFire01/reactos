/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DISPLIB managment APIs
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#include "pdisplib.h"
//#define NDEBUG
#include <debug.h>

/*
 * Load DXGKrnl and obtain the interface list associated
 */
NTSTATUS
NTAPI
DisplibLoadDxgkrnl(_In_  ULONG IoControlCode,
                   _Out_ PDXGKPORT_INITIALIZE* DxgkInitPfn)
{
    IRP *Irp;
    KEVENT Event;
    NTSTATUS Status;
    UNICODE_STRING DeviceName;
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_OBJECT DxgkrnlFileObject;
    UNICODE_STRING DxgkrnlServiceName;
    PDEVICE_OBJECT DxgkrnlDeviceObject;

    /* First load DXGKrnl itself */
    RtlInitUnicodeString(&DxgkrnlServiceName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\DXGKrnl");
    Status = ZwLoadDriver(&DxgkrnlServiceName);

    if (Status != STATUS_SUCCESS)
    {
        DPRINT("DisplibLoadDxgkrnl: ZwLoadDriver has failed with status: %X", Status);
        return Status;
    }

    /* Okay we suceeded, Go ahead and grab the DxgkrnlDeviceObject */
    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    Status = IoGetDeviceObjectPointer(&DeviceName,
                                      FILE_ALL_ACCESS,
                                      &DxgkrnlFileObject,
                                      &DxgkrnlDeviceObject);
    if (Status != STATUS_SUCCESS)
        goto Failure;

    /* Grab a function pointer to DxgkPortInitialize via IOCTRL */
    KeInitializeEvent(&Event, NotificationEvent, 0);
    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DxgkrnlDeviceObject,
                                        NULL,
                                        0,
                                        DxgkInitPfn,
                                        sizeof(DxgkInitPfn),
                                        TRUE,
                                        &Event,
                                        &IoStatusBlock);

    /* Can't continue without being able to call this routine */
    if (!Irp)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        ZwUnloadDriver(&DxgkrnlServiceName);
        return Status;
    }
    Status = IofCallDriver(DxgkrnlDeviceObject, Irp);
    if (Status != STATUS_SUCCESS)
        goto Failure;


    return Status;

Failure:
    ZwUnloadDriver(&DxgkrnlServiceName);
    return Status;
}
