/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Imagis IST3038C touchscreen on the Galaxy J5 2016
 * COPYRIGHT:   Copyright 2026 jimkoutso2008 <lomg04492@gmail.com>
 */

/*
 * j5touch.sys - Imagis IST3038C touchscreen for the Galaxy J5 2016 (MSM8916)
 * under Windows, as a HID absolute mouse.
 *
 * Windows has no I2C (BLSP QUP) or GPIO (TLMM) controller drivers for this
 * SoC, so this driver talks to the chip itself: it maps the TLMM register
 * pages of the four pins it needs (ACPI device J5TS0001, see the firmware's
 * DSDT) and bit-bangs I2C, exactly like the firmware's GpioI2cLib. A system
 * thread polls the controller and completes HID read reports.
 *
 * Register protocol: Linux drivers/input/touchscreen/imagis.c.
 */

#include <ntddk.h>
#include <wdm.h>

/* --- HID minidriver interface (hidport.h is not in the mingw DDK) --- */

#define HID_REVISION 0x00000001
#define HID_CTL_CODE(id) CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_HID_GET_DEVICE_DESCRIPTOR   HID_CTL_CODE(0)
#define IOCTL_HID_GET_REPORT_DESCRIPTOR   HID_CTL_CODE(1)
#define IOCTL_HID_READ_REPORT             HID_CTL_CODE(2)
#define IOCTL_HID_ACTIVATE_DEVICE         HID_CTL_CODE(7)
#define IOCTL_HID_DEACTIVATE_DEVICE       HID_CTL_CODE(8)
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES   HID_CTL_CODE(9)

typedef struct {
  ULONG          Revision;
  PDRIVER_OBJECT DriverObject;
  PUNICODE_STRING RegistryPath;
  ULONG          DeviceExtensionSize;
  BOOLEAN        DevicesArePolled;
  UCHAR          Reserved[3];
} HID_MINIDRIVER_REGISTRATION;

typedef struct {
  PDEVICE_OBJECT PhysicalDeviceObject;
  PDEVICE_OBJECT NextDeviceObject;
  PVOID          MiniDeviceExtension;
} HID_DEVICE_EXTENSION;

#pragma pack(push, 1)
typedef struct {
  UCHAR  bLength;
  UCHAR  bDescriptorType;
  USHORT bcdHID;
  UCHAR  bCountry;
  UCHAR  bNumDescriptors;
  UCHAR  bReportType;
  USHORT wReportLength;
} HID_DESCRIPTOR;

typedef struct {
  UCHAR  Buttons;
  USHORT X;
  USHORT Y;
} TOUCH_REPORT;
#pragma pack(pop)

typedef struct {
  ULONG  Size;
  USHORT VendorID;
  USHORT ProductID;
  USHORT VersionNumber;
  USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES;

__declspec(dllimport) NTSTATUS NTAPI HidRegisterMinidriver(HID_MINIDRIVER_REGISTRATION *Registration);

/* --- Absolute mouse report: button 1 = finger down, X/Y 0..32767 --- */

static const UCHAR mReportDescriptor[] = {
  0x05, 0x01,       /* Usage Page (Generic Desktop) */
  0x09, 0x02,       /* Usage (Mouse) */
  0xA1, 0x01,       /* Collection (Application) */
  0x09, 0x01,       /*   Usage (Pointer) */
  0xA1, 0x00,       /*   Collection (Physical) */
  0x05, 0x09,       /*     Usage Page (Button) */
  0x19, 0x01,       /*     Usage Minimum (1) */
  0x29, 0x01,       /*     Usage Maximum (1) */
  0x15, 0x00,       /*     Logical Minimum (0) */
  0x25, 0x01,       /*     Logical Maximum (1) */
  0x95, 0x01,       /*     Report Count (1) */
  0x75, 0x01,       /*     Report Size (1) */
  0x81, 0x02,       /*     Input (Data, Var, Abs) */
  0x95, 0x01,       /*     Report Count (1) */
  0x75, 0x07,       /*     Report Size (7) */
  0x81, 0x03,       /*     Input (Const) */
  0x05, 0x01,       /*     Usage Page (Generic Desktop) */
  0x09, 0x30,       /*     Usage (X) */
  0x09, 0x31,       /*     Usage (Y) */
  0x15, 0x00,       /*     Logical Minimum (0) */
  0x26, 0xFF, 0x7F, /*     Logical Maximum (32767) */
  0x75, 0x10,       /*     Report Size (16) */
  0x95, 0x02,       /*     Report Count (2) */
  0x81, 0x02,       /*     Input (Data, Var, Abs) */
  0xC0,             /*   End Collection */
  0xC0,             /* End Collection */
};

/* Absolute pointer: keeps a .reloc section in the image, like Microsoft's drivers */
static const UCHAR *volatile mReportDescriptorPtr = mReportDescriptor;

static const HID_DESCRIPTOR mHidDescriptor = {
  sizeof(HID_DESCRIPTOR), 0x21, 0x0101, 0, 1, 0x22, sizeof(mReportDescriptor)
};

/* --- Board wiring (must match the DSDT resource order) --- */

enum { PIN_INT, PIN_SDA, PIN_SCL, PIN_PWR, PIN_COUNT };

#define TLMM_CFG          0x0
#define   TLMM_CFG_OE     (1u << 9)
#define TLMM_IN_OUT       0x4
#define   TLMM_IN         (1u << 0)
#define   TLMM_OUT        (1u << 1)

#define IST_ADDR          0x50
#define IST_MAX_X         720
#define IST_MAX_Y         1280
#define IST_HIB_ACCESS    (0x800Bu << 16)
#define IST_REG_CHIPID    (0x40001000u | 0x80000000u)
#define IST_REG_HIB_BASE  0x30000100u
#define IST_REG_INTR_MSG  (IST_REG_HIB_BASE | IST_HIB_ACCESS | 0x4)
#define IST_REG_COORD     (IST_REG_HIB_BASE | IST_HIB_ACCESS | 0x8)

#define I2C_HALF_US       5
#define POLL_MS           10

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT Next;
  volatile ULONG *Pin[PIN_COUNT];
  PVOID          Mapped[PIN_COUNT];
  KSPIN_LOCK     Lock;
  LIST_ENTRY     PendingReads;
  PKTHREAD       Thread;
  KEVENT         StopEvent;
  TOUCH_REPORT   Last;
  BOOLEAN        HaveReport;
} DEV_EXT;

/* WinPE has no debugger attached, so report progress through the registry */
static void Stat(PCWSTR Name, ULONG Value)
{
  RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"j5touch", Name, REG_DWORD, &Value, sizeof(Value));
}

static DEV_EXT *GetExt(PDEVICE_OBJECT Fdo)
{
  return (DEV_EXT *)((HID_DEVICE_EXTENSION *)Fdo->DeviceExtension)->MiniDeviceExtension;
}

/* --- Open-drain bit-banged I2C on the TLMM pins --- */

static void LineLow(DEV_EXT *E, int Pin)
{
  E->Pin[Pin][TLMM_IN_OUT / 4] &= ~TLMM_OUT;
  E->Pin[Pin][TLMM_CFG / 4] |= TLMM_CFG_OE;
}

static void LineRelease(DEV_EXT *E, int Pin)
{
  E->Pin[Pin][TLMM_CFG / 4] &= ~TLMM_CFG_OE;
}

static int LineGet(DEV_EXT *E, int Pin)
{
  return (E->Pin[Pin][TLMM_IN_OUT / 4] & TLMM_IN) != 0;
}

static void Half(void)
{
  KeStallExecutionProcessor(I2C_HALF_US);
}

static BOOLEAN SclRelease(DEV_EXT *E)
{
  int Timeout = 1000;

  LineRelease(E, PIN_SCL);
  while (!LineGet(E, PIN_SCL)) {
    if (Timeout-- == 0) {
      return FALSE;
    }
    KeStallExecutionProcessor(1);
  }
  return TRUE;
}

static BOOLEAN I2cStart(DEV_EXT *E)
{
  LineRelease(E, PIN_SDA);
  Half();
  if (!SclRelease(E)) {
    return FALSE;
  }
  Half();
  LineLow(E, PIN_SDA);
  Half();
  LineLow(E, PIN_SCL);
  Half();
  return TRUE;
}

static void I2cStop(DEV_EXT *E)
{
  LineLow(E, PIN_SDA);
  Half();
  SclRelease(E);
  Half();
  LineRelease(E, PIN_SDA);
  Half();
}

static BOOLEAN I2cWriteByte(DEV_EXT *E, UCHAR Byte)
{
  int Bit;
  int Nack;

  for (Bit = 7; Bit >= 0; Bit--) {
    if (Byte & (1 << Bit)) {
      LineRelease(E, PIN_SDA);
    } else {
      LineLow(E, PIN_SDA);
    }
    Half();
    if (!SclRelease(E)) {
      return FALSE;
    }
    Half();
    LineLow(E, PIN_SCL);
  }

  LineRelease(E, PIN_SDA);
  Half();
  if (!SclRelease(E)) {
    return FALSE;
  }
  Half();
  Nack = LineGet(E, PIN_SDA);
  LineLow(E, PIN_SCL);
  return !Nack;
}

static BOOLEAN I2cReadByte(DEV_EXT *E, UCHAR *Byte, BOOLEAN Ack)
{
  int   Bit;
  UCHAR Value = 0;

  LineRelease(E, PIN_SDA);
  for (Bit = 0; Bit < 8; Bit++) {
    Half();
    if (!SclRelease(E)) {
      return FALSE;
    }
    Half();
    Value = (UCHAR)((Value << 1) | LineGet(E, PIN_SDA));
    LineLow(E, PIN_SCL);
  }

  if (Ack) {
    LineLow(E, PIN_SDA);
  } else {
    LineRelease(E, PIN_SDA);
  }
  Half();
  if (!SclRelease(E)) {
    return FALSE;
  }
  Half();
  LineLow(E, PIN_SCL);
  LineRelease(E, PIN_SDA);

  *Byte = Value;
  return TRUE;
}

/* 4-byte big-endian register address in, 4-byte big-endian value out */
static BOOLEAN IstReadReg(DEV_EXT *E, ULONG Reg, ULONG *Value)
{
  UCHAR   Buf[4];
  int     Retry;
  int     I;
  BOOLEAN Ok;

  for (Retry = 0; Retry < 3; Retry++) {
    Ok = I2cStart(E) && I2cWriteByte(E, IST_ADDR << 1);
    for (I = 0; Ok && I < 4; I++) {
      Ok = I2cWriteByte(E, (UCHAR)(Reg >> (24 - 8 * I)));
    }
    Ok = Ok && I2cStart(E) && I2cWriteByte(E, (IST_ADDR << 1) | 1);
    for (I = 0; Ok && I < 4; I++) {
      Ok = I2cReadByte(E, &Buf[I], I < 3);
    }
    I2cStop(E);
    if (Ok) {
      *Value = ((ULONG)Buf[0] << 24) | ((ULONG)Buf[1] << 16) | ((ULONG)Buf[2] << 8) | Buf[3];
      return TRUE;
    }
  }
  return FALSE;
}

/* --- Touch polling and HID read completion --- */

static VOID ReadCancel(PDEVICE_OBJECT Fdo, PIRP Irp)
{
  DEV_EXT *E = GetExt(Fdo);
  KIRQL   Irql;

  IoReleaseCancelSpinLock(Irp->CancelIrql);
  KeAcquireSpinLock(&E->Lock, &Irql);
  RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
  KeReleaseSpinLock(&E->Lock, Irql);

  Irp->IoStatus.Status = STATUS_CANCELLED;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static BOOLEAN CompleteOneRead(DEV_EXT *E, const TOUCH_REPORT *Report)
{
  KIRQL       Irql;
  PIRP        Irp = NULL;
  PLIST_ENTRY Entry;

  KeAcquireSpinLock(&E->Lock, &Irql);
  while (!IsListEmpty(&E->PendingReads)) {
    Entry = RemoveHeadList(&E->PendingReads);
    Irp   = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
    if (IoSetCancelRoutine(Irp, NULL) != NULL) {
      break;
    }
    /* Cancel routine is running for this one and will complete it */
    InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    Irp = NULL;
  }
  KeReleaseSpinLock(&E->Lock, Irql);

  if (Irp == NULL) {
    return FALSE;
  }

  RtlCopyMemory(Irp->UserBuffer, Report, sizeof(*Report));
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = sizeof(*Report);
  IoCompleteRequest(Irp, IO_MOUSE_INCREMENT);
  return TRUE;
}

static void PollOnce(DEV_EXT *E, ULONG Tick)
{
  ULONG        Msg;
  ULONG        Coord;
  ULONG        Count;
  ULONG        Status;
  ULONG        I;
  TOUCH_REPORT R = E->Last;

  /* INT (active low) says there is news; also poll while touching and every 100ms */
  if (LineGet(E, PIN_INT) && !(E->Last.Buttons & 1) && (Tick % 10) != 0) {
    return;
  }

  if (!IstReadReg(E, IST_REG_INTR_MSG, &Msg)) {
    return;
  }

  Count  = (Msg >> 12) & 0xF;
  Status = Msg & 0x3FF;
  if (Count > 10) {
    return;
  }

  R.Buttons = 0;
  for (I = 0; I < Count; I++) {
    if (!(Status & (1u << I))) {
      continue;
    }
    if (!IstReadReg(E, IST_REG_COORD + I * 4, &Coord)) {
      return;
    }
    ULONG X = (Coord >> 12) & 0xFFF;
    ULONG Y = Coord & 0xFFF;
    if (X >= IST_MAX_X) X = IST_MAX_X - 1;
    if (Y >= IST_MAX_Y) Y = IST_MAX_Y - 1;
    R.X = (USHORT)(X * 32767 / (IST_MAX_X - 1));
    R.Y = (USHORT)(Y * 32767 / (IST_MAX_Y - 1));
    R.Buttons = 1;
    break;
  }

  if (!E->HaveReport || R.Buttons != E->Last.Buttons || R.X != E->Last.X || R.Y != E->Last.Y) {
    E->Last = R;
    E->HaveReport = TRUE;
    CompleteOneRead(E, &R);
  }
}

static VOID PollThread(PVOID Context)
{
  DEV_EXT       *E = Context;
  LARGE_INTEGER Period;
  ULONG         Tick = 0;
  ULONG         ChipId = 0;

  Stat(L"ThreadRunning", 1);
  if (IstReadReg(E, IST_REG_CHIPID, &ChipId)) {
    Stat(L"ChipId", ChipId);
  } else {
    Stat(L"ChipId", 0xFFFFFFFF); /* no answer from the touch controller */
  }

  Period.QuadPart = -10000LL * POLL_MS;
  while (KeWaitForSingleObject(&E->StopEvent, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT) {
    PollOnce(E, Tick++);
  }
  Stat(L"ThreadRunning", 0);
  PsTerminateSystemThread(STATUS_SUCCESS);
}

/* --- PnP --- */

static NTSTATUS SyncCompletion(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Context)
{
  (void)Dev;
  (void)Irp;
  KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardAndWait(DEV_EXT *E, PIRP Irp)
{
  KEVENT Event;

  KeInitializeEvent(&Event, NotificationEvent, FALSE);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, SyncCompletion, &Event, TRUE, TRUE, TRUE);
  if (IoCallDriver(E->Next, Irp) == STATUS_PENDING) {
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
  }
  return Irp->IoStatus.Status;
}

static NTSTATUS StartDevice(DEV_EXT *E, PCM_RESOURCE_LIST Res)
{
  PCM_PARTIAL_RESOURCE_LIST List;
  ULONG                     I;
  ULONG                     Found = 0;
  HANDLE                    Thread;
  NTSTATUS                  Status;

  if (Res == NULL || Res->Count == 0) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  List = &Res->List[0].PartialResourceList;
  for (I = 0; I < List->Count && Found < PIN_COUNT; I++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR D = &List->PartialDescriptors[I];
    if (D->Type != CmResourceTypeMemory) {
      continue;
    }
    E->Mapped[Found] = MmMapIoSpace(D->u.Memory.Start, D->u.Memory.Length, MmNonCached);
    if (E->Mapped[Found] == NULL) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    E->Pin[Found] = (volatile ULONG *)E->Mapped[Found];
    Found++;
  }
  if (Found != PIN_COUNT) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  /* Keep the touch VDD LDO enabled (the firmware switched it on) */
  E->Pin[PIN_PWR][TLMM_IN_OUT / 4] |= TLMM_OUT;
  E->Pin[PIN_PWR][TLMM_CFG / 4] |= TLMM_CFG_OE;
  LineRelease(E, PIN_SDA);
  LineRelease(E, PIN_SCL);
  LineRelease(E, PIN_INT);

  KeInitializeEvent(&E->StopEvent, NotificationEvent, FALSE);
  Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, E);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&E->Thread, NULL);
  ZwClose(Thread);
  return STATUS_SUCCESS;
}

static void StopDevice(DEV_EXT *E)
{
  int I;

  if (E->Thread != NULL) {
    KeSetEvent(&E->StopEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(E->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(E->Thread);
    E->Thread = NULL;
  }
  for (I = 0; I < PIN_COUNT; I++) {
    if (E->Mapped[I] != NULL) {
      MmUnmapIoSpace(E->Mapped[I], PAGE_SIZE);
      E->Mapped[I] = NULL;
      E->Pin[I] = NULL;
    }
  }
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Fdo, PIRP Irp)
{
  DEV_EXT            *E  = GetExt(Fdo);
  PIO_STACK_LOCATION Sp  = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS           Status;

  switch (Sp->MinorFunction) {
  case IRP_MN_START_DEVICE:
    Status = ForwardAndWait(E, Irp);
    if (NT_SUCCESS(Status)) {
      Status = StartDevice(E, Sp->Parameters.StartDevice.AllocatedResourcesTranslated);
      if (!NT_SUCCESS(Status)) {
        StopDevice(E);
      }
    }
    Stat(L"StartDevice", (ULONG)Status);
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;

  case IRP_MN_STOP_DEVICE:
  case IRP_MN_SURPRISE_REMOVAL:
  case IRP_MN_REMOVE_DEVICE:
    StopDevice(E);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    break;

  default:
    break;
  }

  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(E->Next, Irp);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(GetExt(Fdo)->Next, Irp);
}

/* --- HID IOCTLs from hidclass --- */

static NTSTATUS CopyOut(PIRP Irp, const void *Data, ULONG Length)
{
  PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);

  if (Sp->Parameters.DeviceIoControl.OutputBufferLength < Length) {
    return STATUS_BUFFER_TOO_SMALL;
  }
  RtlCopyMemory(Irp->UserBuffer, Data, Length);
  Irp->IoStatus.Information = Length;
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchInternalIoctl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
  DEV_EXT              *E  = GetExt(Fdo);
  PIO_STACK_LOCATION   Sp  = IoGetCurrentIrpStackLocation(Irp);
  HID_DEVICE_ATTRIBUTES Attr;
  NTSTATUS             Status;
  KIRQL                Irql;

  Irp->IoStatus.Information = 0;

  switch (Sp->Parameters.DeviceIoControl.IoControlCode) {
  case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
    Status = CopyOut(Irp, &mHidDescriptor, sizeof(mHidDescriptor));
    break;

  case IOCTL_HID_GET_REPORT_DESCRIPTOR:
    Status = CopyOut(Irp, mReportDescriptorPtr, sizeof(mReportDescriptor));
    break;

  case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
    RtlZeroMemory(&Attr, sizeof(Attr));
    Attr.Size          = sizeof(Attr);
    Attr.VendorID      = 0x4A35; /* "J5" */
    Attr.ProductID     = 0x3038;
    Attr.VersionNumber = 0x0100;
    Status = CopyOut(Irp, &Attr, sizeof(Attr));
    break;

  case IOCTL_HID_READ_REPORT:
    if (Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(TOUCH_REPORT)) {
      Status = STATUS_BUFFER_TOO_SMALL;
      break;
    }
    KeAcquireSpinLock(&E->Lock, &Irql);
    IoSetCancelRoutine(Irp, ReadCancel);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL) {
      KeReleaseSpinLock(&E->Lock, Irql);
      Status = STATUS_CANCELLED;
      break;
    }
    IoMarkIrpPending(Irp);
    InsertTailList(&E->PendingReads, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&E->Lock, Irql);
    return STATUS_PENDING;

  case IOCTL_HID_ACTIVATE_DEVICE:
  case IOCTL_HID_DEACTIVATE_DEVICE:
    Status = STATUS_SUCCESS;
    break;

  default:
    Status = STATUS_NOT_SUPPORTED;
    break;
  }

  Irp->IoStatus.Status = Status;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Fdo)
{
  DEV_EXT *E = GetExt(Fdo);

  (void)Driver;
  RtlZeroMemory(E, sizeof(*E));
  E->Self = Fdo;
  E->Next = ((HID_DEVICE_EXTENSION *)Fdo->DeviceExtension)->NextDeviceObject;
  KeInitializeSpinLock(&E->Lock);
  InitializeListHead(&E->PendingReads);
  Fdo->Flags &= ~DO_DEVICE_INITIALIZING;
  Stat(L"AddDevice", 1);
  return STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT Driver)
{
  (void)Driver;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
  HID_MINIDRIVER_REGISTRATION Reg;
  NTSTATUS                    Status;

  Driver->MajorFunction[IRP_MJ_PNP]                     = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER]                   = DispatchPower;
  Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternalIoctl;
  Driver->DriverExtension->AddDevice                    = AddDevice;
  Driver->DriverUnload                                  = DriverUnload;

  RtlZeroMemory(&Reg, sizeof(Reg));
  Reg.Revision            = HID_REVISION;
  Reg.DriverObject        = Driver;
  Reg.RegistryPath        = RegistryPath;
  Reg.DeviceExtensionSize = sizeof(DEV_EXT);
  Reg.DevicesArePolled    = FALSE;
  Status = HidRegisterMinidriver(&Reg);
  Stat(L"DriverEntry", (ULONG)Status);
  return Status;
}
