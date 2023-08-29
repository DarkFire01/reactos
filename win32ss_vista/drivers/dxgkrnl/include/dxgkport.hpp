#pragma once

NTSTATUS
NTAPI
DxgkPortAddDevice(_In_    DRIVER_OBJECT *DriverObject,
                  _Inout_ DEVICE_OBJECT *PhysicalDeviceObject);

NTSTATUS
NTAPI
DxgkPortDriverUnload(_In_ PDRIVER_OBJECT DriverObject);

NTSTATUS
NTAPI
DxgkPortDispatchCreateDevice(_In_    PDEVICE_OBJECT DeviceObject,
                             _Inout_ PIRP Irp);

NTSTATUS
NTAPI
DxgkPortDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject,
                    _In_ PVOID Tag);

PSTR
NTAPI
DxgkPortDispatchPower(_In_ PDEVICE_OBJECT DeviceObject,
                      _In_ PSTR MutableMessage);

NTSTATUS
NTAPI
DxgkPortDispatchIoctl(_In_    PDEVICE_OBJECT DeviceObject,
                      _Inout_ IRP *Irp);

NTSTATUS
NTAPI
DxgkPortDispatchInternalIoctl(_In_ PDEVICE_OBJECT DeviceObject,
                              _Inout_ IRP *Irp);
NTSTATUS
NTAPI
DxgkPortDispatchSystemControl(_In_ PDEVICE_OBJECT DeviceObject,
                              _In_ PVOID Tag);

NTSTATUS
NTAPI
DxgkPortDispatchCloseDevice(_In_ PDEVICE_OBJECT DeviceObject);
