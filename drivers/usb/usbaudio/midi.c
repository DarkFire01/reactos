/*
 * PROJECT:     ReactOS Universal Audio Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB MIDI streaming pins
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "usbaudio.h"

/* How long stopping waits for the device to give its transfers back */
#define USBMIDI_STOP_TIMEOUT_MS         2000

/* Code index numbers of USB MIDI event packets */
#define USBMIDI_CIN_COMMON_2            (0x2)
#define USBMIDI_CIN_COMMON_3            (0x3)
#define USBMIDI_CIN_SYSEX               (0x4)
#define USBMIDI_CIN_SYSEX_END_1         (0x5)
#define USBMIDI_CIN_SINGLE_BYTE         (0xF)

/* Bytes of an event packet that carry MIDI data, by its code index number */
static const UCHAR UsbMidiPacketBytes[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

static KSDATARANGE_MUSIC UsbMidiDataRange =
{
    {
        sizeof(KSDATARANGE_MUSIC),
        0,
        0,
        0,
        {STATIC_KSDATAFORMAT_TYPE_MUSIC},
        {STATIC_KSDATAFORMAT_SUBTYPE_MIDI},
        {STATIC_KSDATAFORMAT_SPECIFIER_NONE}
    },
    {STATIC_KSMUSIC_TECHNOLOGY_PORT},
    0,
    0,
    0xFFFF
};

static const PKSDATARANGE UsbMidiDataRanges[] =
{
    (PKSDATARANGE)&UsbMidiDataRange
};

NTSTATUS NTAPI UsbMidiPinCreate(_In_ PKSPIN Pin, _In_ PIRP Irp);
NTSTATUS NTAPI UsbMidiPinClose(_In_ PKSPIN Pin, _In_ PIRP Irp);
NTSTATUS NTAPI UsbMidiPinProcess(_In_ PKSPIN Pin);
NTSTATUS NTAPI UsbMidiPinSetDeviceState(_In_ PKSPIN Pin, _In_ KSSTATE ToState, _In_ KSSTATE FromState);

static const KSPIN_DISPATCH UsbMidiPinDispatch =
{
    UsbMidiPinCreate,
    UsbMidiPinClose,
    UsbMidiPinProcess,
    NULL,
    NULL,
    UsbMidiPinSetDeviceState,
    NULL,
    NULL,
    NULL,
    NULL
};

/* Data bytes that follow a status byte; system exclusive data is counted apart */
static
UCHAR
UsbMidiDataBytes(
    _In_ UCHAR Status)
{
    if (Status < 0xF0)
        return ((Status & 0xE0) == 0xC0) ? 1 : 2;
    if (Status == 0xF1 || Status == 0xF3)
        return 1;
    if (Status == 0xF2)
        return 2;
    return 0;
}

/**
 * @brief
 * Notes the bulk pipes of the MIDI streaming interface and how many cables each carries,
 * as its class endpoint descriptors list them.
 */
VOID
UsbMidiSetupInterface(
    _Inout_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PUSBD_INTERFACE_INFORMATION Interface)
{
    PUSB_CONFIGURATION_DESCRIPTOR Configuration = DeviceExtension->ConfigurationDescriptor;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PUSB_ENDPOINT_DESCRIPTOR Endpoint = NULL;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PUSBD_PIPE_INFORMATION Pipe;
    ULONG Index;
    UCHAR Cables;

    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(Configuration, Configuration, Interface->InterfaceNumber, 0, -1, -1, -1);
    if (!InterfaceDescriptor)
        return;

    for (Descriptor = UsbAudioNextDescriptor(Configuration, InterfaceDescriptor);
         Descriptor && Descriptor->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE;
         Descriptor = UsbAudioNextDescriptor(Configuration, Descriptor))
    {
        if (Descriptor->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && Descriptor->bLength >= sizeof(USB_ENDPOINT_DESCRIPTOR))
        {
            Endpoint = (PUSB_ENDPOINT_DESCRIPTOR)Descriptor;
            continue;
        }

        /* the class endpoint descriptor follows its endpoint and counts the embedded jacks behind it */
        if (!Endpoint || Descriptor->bDescriptorType != USB_AUDIO_CS_ENDPOINT_DESCRIPTOR_TYPE ||
            Descriptor->bLength < 4 || ((PUCHAR)Descriptor)[2] != USB_MIDI_GENERAL)
        {
            continue;
        }

        Cables = (UCHAR)min(((PUCHAR)Descriptor)[3], USBMIDI_MAX_CABLES);
        for (Index = 0; Index < Interface->NumberOfPipes; Index++)
        {
            Pipe = &Interface->Pipes[Index];
            if (Pipe->EndpointAddress != Endpoint->bEndpointAddress ||
                (Pipe->PipeType != UsbdPipeTypeBulk && Pipe->PipeType != UsbdPipeTypeInterrupt))
            {
                continue;
            }

            if (USB_ENDPOINT_DIRECTION_IN(Endpoint->bEndpointAddress))
            {
                DeviceExtension->MidiInPipe = Pipe->PipeHandle;
                DeviceExtension->MidiInPacketSize = Pipe->MaximumPacketSize;
                DeviceExtension->MidiInCables = Cables;
            }
            else
            {
                DeviceExtension->MidiOutPipe = Pipe->PipeHandle;
                DeviceExtension->MidiOutPacketSize = Pipe->MaximumPacketSize;
                DeviceExtension->MidiOutCables = Cables;
            }
        }
        Endpoint = NULL;
    }

    DPRINT1("UsbMidiSetupInterface interface %u, %u cables out, %u cables in\n",
            Interface->InterfaceNumber, DeviceExtension->MidiOutCables, DeviceExtension->MidiInCables);
}

static
VOID
UsbMidiFreeTransfer(
    _Inout_ PMIDI_TRANSFER Transfer)
{
    if (Transfer->Irp)
        IoFreeIrp(Transfer->Irp);
    if (Transfer->Urb)
        FreeFunction(Transfer->Urb);
    if (Transfer->Buffer)
        FreeFunction(Transfer->Buffer);
    Transfer->Irp = NULL;
    Transfer->Urb = NULL;
    Transfer->Buffer = NULL;
}

static
NTSTATUS
UsbMidiAllocateTransfer(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _Inout_ PMIDI_TRANSFER Transfer,
    _In_ PVOID Context)
{
    Transfer->Context = Context;
    Transfer->Irp = IoAllocateIrp(DeviceExtension->LowerDevice->StackSize, FALSE);
    Transfer->Urb = AllocFunction(sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));
    Transfer->Buffer = AllocFunction(USBMIDI_TRANSFER_BYTES);
    if (!Transfer->Irp || !Transfer->Urb || !Transfer->Buffer)
    {
        UsbMidiFreeTransfer(Transfer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

static
VOID
UsbMidiCallDevice(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _Inout_ PMIDI_TRANSFER Transfer,
    _In_ USBD_PIPE_HANDLE Pipe,
    _In_ BOOLEAN In,
    _In_ ULONG Length,
    _In_ PIO_COMPLETION_ROUTINE CompletionRoutine)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *Urb = &Transfer->Urb->UrbBulkOrInterruptTransfer;
    PIO_STACK_LOCATION IoStack;

    RtlZeroMemory(Urb, sizeof(*Urb));
    Urb->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb->Hdr.Length = sizeof(*Urb);
    Urb->PipeHandle = Pipe;
    Urb->TransferFlags = In ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT;
    Urb->TransferBuffer = Transfer->Buffer;
    Urb->TransferBufferLength = Length;

    IoReuseIrp(Transfer->Irp, STATUS_SUCCESS);
    IoStack = IoGetNextIrpStackLocation(Transfer->Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoStack->Parameters.Others.Argument1 = Transfer->Urb;
    IoSetCompletionRoutine(Transfer->Irp, CompletionRoutine, Transfer, TRUE, TRUE, TRUE);
    IoCallDriver(DeviceExtension->LowerDevice, Transfer->Irp);
}

static
VOID
UsbMidiAbortPipe(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ USBD_PIPE_HANDLE Pipe)
{
    struct _URB_PIPE_REQUEST *Urb;

    Urb = AllocFunction(sizeof(*Urb));
    if (!Urb)
        return;

    Urb->Hdr.Length = sizeof(*Urb);
    Urb->Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    Urb->PipeHandle = Pipe;
    SubmitUrbSync(DeviceExtension->LowerDevice, (PURB)Urb);
    FreeFunction(Urb);
}

/* A transfer that ended this way will not be sent again */
static
BOOLEAN
UsbMidiIsFinal(
    _In_ NTSTATUS Status)
{
    return Status == STATUS_CANCELLED || Status == STATUS_NO_SUCH_DEVICE ||
           Status == STATUS_DEVICE_NOT_CONNECTED || Status == STATUS_DEVICE_REMOVED;
}

/* Output */

static
VOID
UsbMidiPutPacket(
    _In_ PMIDI_PIN_CONTEXT Context,
    _Inout_ PMIDI_TRANSFER Transfer,
    _In_ UCHAR CodeIndex,
    _In_ UCHAR Byte0,
    _In_ UCHAR Byte1,
    _In_ UCHAR Byte2)
{
    PUCHAR Packet = Transfer->Buffer + Transfer->Filled;

    Packet[0] = (UCHAR)((Context->Cable << 4) | CodeIndex);
    Packet[1] = Byte0;
    Packet[2] = Byte1;
    Packet[3] = Byte2;
    Transfer->Filled += 4;
}

/*
 * Turns one MIDI byte into event packets. Each byte ends at most one packet, so four bytes of
 * room in the transfer always do.
 */
static
VOID
UsbMidiOutByte(
    _Inout_ PMIDI_PIN_CONTEXT Context,
    _Inout_ PMIDI_TRANSFER Transfer,
    _In_ UCHAR Byte)
{
    UCHAR CodeIndex;

    /* Real time messages go out at once, even in the middle of another message */
    if (Byte >= 0xF8)
    {
        UsbMidiPutPacket(Context, Transfer, USBMIDI_CIN_SINGLE_BYTE, Byte, 0, 0);
        return;
    }

    if (Byte == 0xF0)
    {
        Context->SysEx = TRUE;
        Context->Status = 0;
        Context->Message[0] = Byte;
        Context->Fill = 1;
        return;
    }

    if (Context->SysEx)
    {
        if (Byte < 0x80 || Byte == 0xF7)
        {
            Context->Message[Context->Fill++] = Byte;
            if (Byte == 0xF7)
            {
                /* the end packet says how many of its bytes are the last of the message */
                UsbMidiPutPacket(Context, Transfer, (UCHAR)(USBMIDI_CIN_SYSEX_END_1 + Context->Fill - 1), Context->Message[0],
                                 (Context->Fill > 1) ? Context->Message[1] : 0, (Context->Fill > 2) ? Context->Message[2] : 0);
                Context->SysEx = FALSE;
                Context->Fill = 0;
            }
            else if (Context->Fill == 3)
            {
                UsbMidiPutPacket(Context, Transfer, USBMIDI_CIN_SYSEX, Context->Message[0], Context->Message[1], Context->Message[2]);
                Context->Fill = 0;
            }
            return;
        }

        /* another status cuts the system exclusive message short */
        Context->SysEx = FALSE;
        Context->Fill = 0;
    }

    if (Byte >= 0x80)
    {
        Context->Status = Byte;
        Context->Need = UsbMidiDataBytes(Byte);
        Context->Fill = 0;
        if (!Context->Need)
        {
            UsbMidiPutPacket(Context, Transfer, USBMIDI_CIN_SYSEX_END_1, Byte, 0, 0);
            Context->Status = 0;
        }
        return;
    }

    /* data with no status to belong to is dropped */
    if (!Context->Status)
        return;

    Context->Message[Context->Fill++] = Byte;
    if (Context->Fill < Context->Need)
        return;

    if (Context->Status < 0xF0)
        CodeIndex = Context->Status >> 4;
    else
        CodeIndex = (Context->Need == 1) ? USBMIDI_CIN_COMMON_2 : USBMIDI_CIN_COMMON_3;
    UsbMidiPutPacket(Context, Transfer, CodeIndex, Context->Status, Context->Message[0], (Context->Need > 1) ? Context->Message[1] : 0);

    /* channel messages keep their status for running status, system common ones do not */
    Context->Fill = 0;
    if (Context->Status >= 0xF0)
        Context->Status = 0;
}

/* Reads KS music records off the client's frame; returns the bytes used */
static
ULONG
UsbMidiConsume(
    _Inout_ PMIDI_PIN_CONTEXT Context,
    _Inout_ PMIDI_TRANSFER Transfer,
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ ULONG Length)
{
    ULONG Used;

    for (Used = 0; Used < Length && USBMIDI_TRANSFER_BYTES - Transfer->Filled >= 4; Used++)
    {
        if (Context->HeaderFill < sizeof(KSMUSICFORMAT))
        {
            Context->Header[Context->HeaderFill++] = Data[Used];
            if (Context->HeaderFill == sizeof(KSMUSICFORMAT))
            {
                Context->DataLeft = ((PKSMUSICFORMAT)Context->Header)->ByteCount;
                Context->PadLeft = (ULONG)(-(LONG)Context->DataLeft) & (sizeof(ULONG) - 1);
                if (!Context->DataLeft && !Context->PadLeft)
                    Context->HeaderFill = 0;
            }
            continue;
        }

        if (Context->DataLeft)
        {
            UsbMidiOutByte(Context, Transfer, Data[Used]);
            Context->DataLeft--;
        }
        else if (Context->PadLeft)
        {
            Context->PadLeft--;
        }

        if (!Context->DataLeft && !Context->PadLeft)
            Context->HeaderFill = 0;
    }
    return Used;
}

static
PMIDI_TRANSFER
UsbMidiTakeTransfer(
    _In_ PMIDI_PIN_CONTEXT Context)
{
    PMIDI_TRANSFER Transfer = NULL;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    if (!IsListEmpty(&Context->FreeTransfers))
        Transfer = CONTAINING_RECORD(RemoveHeadList(&Context->FreeTransfers), MIDI_TRANSFER, Entry);
    KeReleaseSpinLock(&Context->Lock, OldIrql);
    return Transfer;
}

static
VOID
UsbMidiReturnTransfer(
    _In_ PMIDI_PIN_CONTEXT Context,
    _In_ PMIDI_TRANSFER Transfer)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    InsertTailList(&Context->FreeTransfers, &Transfer->Entry);
    KeReleaseSpinLock(&Context->Lock, OldIrql);
}

static
NTSTATUS
NTAPI
UsbMidiOutputComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Ctx)
{
    PMIDI_TRANSFER Transfer = Ctx;
    PMIDI_PIN_CONTEXT Context = Transfer->Context;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!NT_SUCCESS(Irp->IoStatus.Status))
        DPRINT1("UsbMidiOutputComplete cable %u failed with %x\n", Context->Cable, Irp->IoStatus.Status);

    /* the pin is asked to go on before the transfer stops counting, as the pin may go after that */
    UsbMidiReturnTransfer(Context, Transfer);
    KsPinAttemptProcessing(Context->Pin, TRUE);

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    if (!--Context->Pending)
        KeSetEvent(&Context->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Context->Lock, OldIrql);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
UsbMidiSendOutput(
    _In_ PMIDI_PIN_CONTEXT Context,
    _In_ PMIDI_TRANSFER Transfer)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    if (!Context->Streaming)
    {
        InsertTailList(&Context->FreeTransfers, &Transfer->Entry);
        KeReleaseSpinLock(&Context->Lock, OldIrql);
        return;
    }
    if (!Context->Pending++)
        KeClearEvent(&Context->Idle);
    KeReleaseSpinLock(&Context->Lock, OldIrql);

    UsbMidiCallDevice(Context->DeviceExtension, Transfer, Context->DeviceExtension->MidiOutPipe, FALSE,
                      Transfer->Filled, UsbMidiOutputComplete);
}

/* Packs what the client sent into transfers, and sends each as soon as it is made */
static
NTSTATUS
UsbMidiRenderProcess(
    _In_ PKSPIN Pin)
{
    PMIDI_PIN_CONTEXT Context = Pin->Context;
    PKSSTREAM_POINTER Leading;
    PMIDI_TRANSFER Transfer = NULL;
    ULONG Used;

    if (!Context->Streaming)
        return STATUS_PENDING;

    Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    while (Leading)
    {
        if (!Transfer)
        {
            Transfer = UsbMidiTakeTransfer(Context);
            if (!Transfer)
                break;
            Transfer->Filled = 0;
        }

        if (USBMIDI_TRANSFER_BYTES - Transfer->Filled < 4)
        {
            UsbMidiSendOutput(Context, Transfer);
            Transfer = NULL;
            continue;
        }

        Used = UsbMidiConsume(Context, Transfer, Leading->OffsetIn.Data, Leading->OffsetIn.Remaining);

        /* moving past the end of a frame hands it back and goes on to the next one */
        if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, Used, 0, FALSE)))
            Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    }

    if (Leading)
        KsStreamPointerUnlock(Leading, FALSE);

    if (Transfer && Transfer->Filled)
        UsbMidiSendOutput(Context, Transfer);
    else if (Transfer)
        UsbMidiReturnTransfer(Context, Transfer);
    return STATUS_PENDING;
}

/* Input */

static
NTSTATUS
NTAPI
UsbMidiInputComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Ctx);

/* Keeps a read at the device while some input pin runs; Counted is set for one just given back */
static
VOID
UsbMidiSendInput(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ PMIDI_TRANSFER Transfer,
    _In_ BOOLEAN Counted)
{
    ULONG Length;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->MidiInLock, &OldIrql);
    if (!DeviceExtension->MidiInUsers)
    {
        if (Counted && !--DeviceExtension->MidiInPending)
            KeSetEvent(&DeviceExtension->MidiInIdle, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);
        return;
    }
    if (!Counted && !DeviceExtension->MidiInPending++)
        KeClearEvent(&DeviceExtension->MidiInIdle);
    KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);

    /* whole packets of the endpoint, so the device never has to split one */
    Length = USBMIDI_TRANSFER_BYTES;
    if (DeviceExtension->MidiInPacketSize && DeviceExtension->MidiInPacketSize <= USBMIDI_TRANSFER_BYTES)
        Length -= Length % DeviceExtension->MidiInPacketSize;
    UsbMidiCallDevice(DeviceExtension, Transfer, DeviceExtension->MidiInPipe, TRUE, Length, UsbMidiInputComplete);
}

/* Adds bytes for one cable, dropping the oldest when its pin falls behind */
static
VOID
UsbMidiRingWrite(
    _Inout_ PMIDI_IN_CABLE Cable,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
    {
        if (Cable->Count == USBMIDI_IN_RING_BYTES)
        {
            Cable->Read = (Cable->Read + 1) % USBMIDI_IN_RING_BYTES;
            Cable->Count--;
        }
        Cable->Ring[(Cable->Read + Cable->Count) % USBMIDI_IN_RING_BYTES] = Data[Index];
        Cable->Count++;
    }
}

static
NTSTATUS
NTAPI
UsbMidiInputComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Ctx)
{
    PMIDI_TRANSFER Transfer = Ctx;
    PDEVICE_EXTENSION DeviceExtension = Transfer->Context;
    ULONG Length, Offset, Touched = 0;
    UCHAR Cable;
    BOOLEAN Again;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&DeviceExtension->MidiInLock, &OldIrql);
    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        Length = min(Transfer->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength, (ULONG)USBMIDI_TRANSFER_BYTES);
        for (Offset = 0; Offset + 4 <= Length; Offset += 4)
        {
            /* empty packets pad a transfer out and carry nothing */
            Cable = Transfer->Buffer[Offset] >> 4;
            if (!UsbMidiPacketBytes[Transfer->Buffer[Offset] & 0xF] || Cable >= DeviceExtension->MidiInCables)
                continue;
            UsbMidiRingWrite(&DeviceExtension->MidiIn[Cable], &Transfer->Buffer[Offset + 1],
                             UsbMidiPacketBytes[Transfer->Buffer[Offset] & 0xF]);
            Touched |= 1 << Cable;
        }

        for (Cable = 0; Cable < DeviceExtension->MidiInCables; Cable++)
        {
            if ((Touched & (1 << Cable)) && DeviceExtension->MidiIn[Cable].Pin)
                KsPinAttemptProcessing(DeviceExtension->MidiIn[Cable].Pin, TRUE);
        }
    }

    Again = DeviceExtension->MidiInUsers && !UsbMidiIsFinal(Irp->IoStatus.Status);
    if (!Again && !--DeviceExtension->MidiInPending)
        KeSetEvent(&DeviceExtension->MidiInIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);

    if (Again)
        UsbMidiSendInput(DeviceExtension, Transfer, TRUE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Hands what came in for the pin's cable to the client, one record per frame */
static
NTSTATUS
UsbMidiCaptureProcess(
    _In_ PKSPIN Pin)
{
    PMIDI_PIN_CONTEXT Context = Pin->Context;
    PDEVICE_EXTENSION DeviceExtension = Context->DeviceExtension;
    PMIDI_IN_CABLE Cable = &DeviceExtension->MidiIn[Context->Cable];
    PKSSTREAM_POINTER Leading;
    PKSMUSICFORMAT Record;
    ULONG Space, Count, Chunk, Used;
    KIRQL OldIrql;

    Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    while (Leading)
    {
        KeAcquireSpinLock(&DeviceExtension->MidiInLock, &OldIrql);
        if (!Cable->Count)
        {
            KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);
            break;
        }

        /* a record and its padding have to fit; a frame without room for any goes back empty */
        Space = Leading->OffsetOut.Remaining;
        Space = (Space > sizeof(*Record)) ? ((Space - sizeof(*Record)) & ~(sizeof(ULONG) - 1)) : 0;
        if (!Space)
        {
            KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);
            if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, 0, 0, TRUE)))
                Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
            continue;
        }

        Count = min(Cable->Count, Space);
        Record = (PKSMUSICFORMAT)Leading->OffsetOut.Data;
        Record->TimeDeltaMs = 0;
        Record->ByteCount = Count;
        Chunk = min(Count, USBMIDI_IN_RING_BYTES - Cable->Read);
        RtlCopyMemory(Record + 1, Cable->Ring + Cable->Read, Chunk);
        RtlCopyMemory((PUCHAR)(Record + 1) + Chunk, Cable->Ring, Count - Chunk);
        Cable->Read = (Cable->Read + Count) % USBMIDI_IN_RING_BYTES;
        Cable->Count -= Count;
        KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);

        Used = sizeof(*Record) + ((Count + sizeof(ULONG) - 1) & ~(sizeof(ULONG) - 1));
        RtlZeroMemory((PUCHAR)(Record + 1) + Count, Used - sizeof(*Record) - Count);

        /* input goes to the client as it comes rather than once the frame is full */
        if (!NT_SUCCESS(KsStreamPointerAdvanceOffsets(Leading, 0, Used, TRUE)))
            Leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    }

    if (Leading)
        KsStreamPointerUnlock(Leading, FALSE);
    return STATUS_SUCCESS;
}

static
VOID
UsbMidiStartInput(
    _In_ PMIDI_PIN_CONTEXT Context)
{
    PDEVICE_EXTENSION DeviceExtension = Context->DeviceExtension;
    BOOLEAN First;
    ULONG Index;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->MidiInLock, &OldIrql);
    DeviceExtension->MidiIn[Context->Cable].Pin = Context->Pin;
    DeviceExtension->MidiIn[Context->Cable].Read = 0;
    DeviceExtension->MidiIn[Context->Cable].Count = 0;
    First = !DeviceExtension->MidiInUsers++;
    KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);

    for (Index = 0; First && Index < USBMIDI_IN_TRANSFERS; Index++)
        UsbMidiSendInput(DeviceExtension, &DeviceExtension->MidiInTransfers[Index], FALSE);
}

static
VOID
UsbMidiStopInput(
    _In_ PMIDI_PIN_CONTEXT Context)
{
    PDEVICE_EXTENSION DeviceExtension = Context->DeviceExtension;
    LARGE_INTEGER Timeout;
    BOOLEAN Last;
    ULONG Attempt;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->MidiInLock, &OldIrql);
    DeviceExtension->MidiIn[Context->Cable].Pin = NULL;
    Last = !--DeviceExtension->MidiInUsers;
    KeReleaseSpinLock(&DeviceExtension->MidiInLock, OldIrql);

    /* a read sent just as the last pin stopped is taken back by a second abort */
    Timeout.QuadPart = -10000LL * USBMIDI_STOP_TIMEOUT_MS / 2;
    for (Attempt = 0; Last && Attempt < 2; Attempt++)
    {
        UsbMidiAbortPipe(DeviceExtension, DeviceExtension->MidiInPipe);
        if (KeWaitForSingleObject(&DeviceExtension->MidiInIdle, Executive, KernelMode, FALSE, &Timeout) != STATUS_TIMEOUT)
            return;
    }
    if (Last)
        DPRINT1("UsbMidiStopInput %ld reads did not come back\n", DeviceExtension->MidiInPending);
}

/* Waits for an output pin's transfers; the pipe is shared, so it is aborted only as a last resort */
static
BOOLEAN
UsbMidiStopOutput(
    _In_ PKSPIN Pin,
    _In_ PMIDI_PIN_CONTEXT Context)
{
    LARGE_INTEGER Timeout;
    BOOLEAN AllBack = TRUE;
    KIRQL OldIrql;

    KsPinAcquireProcessingMutex(Pin);

    KeAcquireSpinLock(&Context->Lock, &OldIrql);
    Context->Streaming = FALSE;
    KeReleaseSpinLock(&Context->Lock, OldIrql);

    Timeout.QuadPart = -10000LL * USBMIDI_STOP_TIMEOUT_MS;
    if (KeWaitForSingleObject(&Context->Idle, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        UsbMidiAbortPipe(Context->DeviceExtension, Context->DeviceExtension->MidiOutPipe);
        AllBack = KeWaitForSingleObject(&Context->Idle, Executive, KernelMode, FALSE, &Timeout) != STATUS_TIMEOUT;
    }

    /* a message cut off by the stop is not finished by the next run */
    Context->HeaderFill = 0;
    Context->DataLeft = 0;
    Context->PadLeft = 0;
    Context->Status = 0;
    Context->Fill = 0;
    Context->SysEx = FALSE;

    KsPinReleaseProcessingMutex(Pin);
    return AllBack;
}

NTSTATUS
NTAPI
UsbMidiPinProcess(
    _In_ PKSPIN Pin)
{
    if (!Pin->Context)
        return STATUS_PENDING;
    if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
        return UsbMidiCaptureProcess(Pin);
    return UsbMidiRenderProcess(Pin);
}

NTSTATUS
NTAPI
UsbMidiPinCreate(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PKSFILTER Filter = KsPinGetParentFilter(Pin);
    PDEVICE_EXTENSION DeviceExtension;
    PMIDI_PIN_CONTEXT Context;
    ULONG Index;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Irp);

    if (!Filter)
        return STATUS_INVALID_PARAMETER;
    DeviceExtension = ((PFILTER_CONTEXT)Filter->Context)->DeviceExtension;

    if (!IsEqualGUIDAligned(&Pin->ConnectionFormat->MajorFormat, &KSDATAFORMAT_TYPE_MUSIC) ||
        !IsEqualGUIDAligned(&Pin->ConnectionFormat->SubFormat, &KSDATAFORMAT_SUBTYPE_MIDI))
    {
        return STATUS_NO_MATCH;
    }

    Context = AllocFunction(sizeof(*Context));
    if (!Context)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* output cables come first among the MIDI pins */
    Index = Pin->Id - DeviceExtension->MidiPinBase;
    Context->DeviceExtension = DeviceExtension;
    Context->Pin = Pin;
    Context->Cable = (UCHAR)((Pin->DataFlow == KSPIN_DATAFLOW_IN) ? Index : Index - DeviceExtension->MidiOutCables);
    KeInitializeSpinLock(&Context->Lock);
    KeInitializeEvent(&Context->Idle, NotificationEvent, TRUE);
    InitializeListHead(&Context->FreeTransfers);

    for (Index = 0; Pin->DataFlow == KSPIN_DATAFLOW_IN && Index < USBMIDI_OUT_TRANSFERS; Index++)
    {
        Status = UsbMidiAllocateTransfer(DeviceExtension, &Context->Transfers[Index], Context);
        if (!NT_SUCCESS(Status))
        {
            while (Index--)
                UsbMidiFreeTransfer(&Context->Transfers[Index]);
            FreeFunction(Context);
            return Status;
        }
        InsertTailList(&Context->FreeTransfers, &Context->Transfers[Index].Entry);
    }

    Pin->Context = Context;
    DPRINT1("UsbMidiPinCreate pin %lu cable %u %s\n", Pin->Id, Context->Cable, (Pin->DataFlow == KSPIN_DATAFLOW_IN) ? "out" : "in");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
UsbMidiPinClose(
    _In_ PKSPIN Pin,
    _In_ PIRP Irp)
{
    PMIDI_PIN_CONTEXT Context = Pin->Context;
    BOOLEAN AllBack = TRUE;
    ULONG Index;

    UNREFERENCED_PARAMETER(Irp);

    if (!Context)
        return STATUS_SUCCESS;

    if (Context->Streaming)
    {
        if (Pin->DataFlow == KSPIN_DATAFLOW_IN)
        {
            AllBack = UsbMidiStopOutput(Pin, Context);
        }
        else
        {
            Context->Streaming = FALSE;
            UsbMidiStopInput(Context);
        }
    }

    KsPinAcquireProcessingMutex(Pin);
    Pin->Context = NULL;
    KsPinReleaseProcessingMutex(Pin);

    /* Transfers the device never gave back still belong to it, and so does their context */
    if (!AllBack)
    {
        DPRINT1("UsbMidiPinClose leaking pin context %p, the device kept transfers\n", Context);
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < USBMIDI_OUT_TRANSFERS; Index++)
        UsbMidiFreeTransfer(&Context->Transfers[Index]);
    FreeFunction(Context);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
UsbMidiPinSetDeviceState(
    _In_ PKSPIN Pin,
    _In_ KSSTATE ToState,
    _In_ KSSTATE FromState)
{
    PMIDI_PIN_CONTEXT Context = Pin->Context;
    KIRQL OldIrql;

    if (!Context || ToState == FromState)
        return STATUS_SUCCESS;

    if (FromState == KSSTATE_RUN && Context->Streaming)
    {
        if (Pin->DataFlow == KSPIN_DATAFLOW_IN)
        {
            UsbMidiStopOutput(Pin, Context);
        }
        else
        {
            Context->Streaming = FALSE;
            UsbMidiStopInput(Context);
        }
    }

    if (ToState == KSSTATE_RUN)
    {
        if (Pin->DataFlow == KSPIN_DATAFLOW_OUT && !Context->DeviceExtension->MidiInTransfers)
            return STATUS_INSUFFICIENT_RESOURCES;

        KeAcquireSpinLock(&Context->Lock, &OldIrql);
        Context->Streaming = TRUE;
        KeReleaseSpinLock(&Context->Lock, OldIrql);

        if (Pin->DataFlow == KSPIN_DATAFLOW_OUT)
            UsbMidiStartInput(Context);
    }
    return STATUS_SUCCESS;
}

ULONG
UsbMidiGetPinCount(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    return (DeviceExtension->MidiOutPipe ? DeviceExtension->MidiOutCables : 0) +
           (DeviceExtension->MidiInPipe ? DeviceExtension->MidiInCables : 0);
}

/**
 * @brief
 * Describes one pin per cable, output cables first, and sets up the reads shared by the
 * input ones.
 */
VOID
UsbMidiInitPinDescriptors(
    _Inout_ PDEVICE_EXTENSION DeviceExtension,
    _Out_writes_(Count) PKSPIN_DESCRIPTOR_EX Pins,
    _In_ ULONG Base,
    _In_ ULONG Count)
{
    ULONG Index, Outputs = DeviceExtension->MidiOutPipe ? DeviceExtension->MidiOutCables : 0;

    DeviceExtension->MidiPinBase = Base;
    if (!DeviceExtension->MidiOutPipe)
        DeviceExtension->MidiOutCables = 0;

    for (Index = 0; Index < Count; Index++)
    {
        Pins[Index].Dispatch = &UsbMidiPinDispatch;
        Pins[Index].PinDescriptor.DataRanges = UsbMidiDataRanges;
        Pins[Index].PinDescriptor.DataRangesCount = ARRAYSIZE(UsbMidiDataRanges);
        Pins[Index].PinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
        Pins[Index].PinDescriptor.DataFlow = (Index < Outputs) ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
        Pins[Index].Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY;
        Pins[Index].InstancesPossible = 1;
    }

    if (Count > Outputs && !DeviceExtension->MidiInTransfers)
    {
        DeviceExtension->MidiIn = AllocFunction(DeviceExtension->MidiInCables * sizeof(MIDI_IN_CABLE));
        DeviceExtension->MidiInTransfers = AllocFunction(USBMIDI_IN_TRANSFERS * sizeof(MIDI_TRANSFER));
        for (Index = 0; DeviceExtension->MidiIn && DeviceExtension->MidiInTransfers && Index < USBMIDI_IN_TRANSFERS; Index++)
        {
            if (!NT_SUCCESS(UsbMidiAllocateTransfer(DeviceExtension, &DeviceExtension->MidiInTransfers[Index], DeviceExtension)))
                break;
        }

        /* without its reads the input has nothing to give, and its pins refuse to run */
        if (!DeviceExtension->MidiIn || !DeviceExtension->MidiInTransfers || Index < USBMIDI_IN_TRANSFERS)
            UsbMidiFreeDevice(DeviceExtension);
    }
}

VOID
UsbMidiFreeDevice(
    _Inout_ PDEVICE_EXTENSION DeviceExtension)
{
    ULONG Index;

    /* reads the device still holds keep what they point at */
    if (DeviceExtension->MidiInPending)
        return;

    for (Index = 0; DeviceExtension->MidiInTransfers && Index < USBMIDI_IN_TRANSFERS; Index++)
        UsbMidiFreeTransfer(&DeviceExtension->MidiInTransfers[Index]);
    if (DeviceExtension->MidiInTransfers)
        FreeFunction(DeviceExtension->MidiInTransfers);
    if (DeviceExtension->MidiIn)
        FreeFunction(DeviceExtension->MidiIn);
    DeviceExtension->MidiInTransfers = NULL;
    DeviceExtension->MidiIn = NULL;
}
