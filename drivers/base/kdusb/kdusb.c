/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KD transport entry points and the KD packet protocol
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/* GLOBALS ********************************************************************/

/* Tag handed to PoSetHiberRange for the transport image */
#define KDUSB_HIBER_TAG     'BsuK'

DEBUG_DEVICE_DESCRIPTOR KdUsbDevice;
DECLSPEC_ALIGN(16) UCHAR KdUsbReceiveBuffer[KDUSB_TRANSFER_SIZE];
CHAR KdUsbTargetName[KDUSB_TARGET_NAME_MAX + 3];
ULONG KdUsbTargetNameLength;

PKDUSB_BOOT_PRINT KdUsbBootPrint;

static const KDUSB_CONTROLLER *KdUsbController;
static BOOLEAN KdUsbControllerReady;

static ULONG KdUsbNextPacketId = INITIAL_PACKET_ID | SYNC_PACKET_ID;
static ULONG KdUsbRemotePacketId = INITIAL_PACKET_ID;
static ULONG KdUsbRetryLimit = 2;
static ULONG KdUsbRetriesLeft = 2;

/* HELPERS ********************************************************************/

/**
 * @brief
 * Puts the packet IDs back where a fresh host session expects them.
 */
static
VOID
NTAPI
KdUsbResetPacketIds(VOID)
{
    KdUsbNextPacketId = INITIAL_PACKET_ID | SYNC_PACKET_ID;
    KdUsbRemotePacketId = INITIAL_PACKET_ID;
}

/**
 * @brief
 * Records whether a debugger host is answering on the link.
 *
 * @param[in] Present
 * TRUE when the host just answered, FALSE when it stopped answering.
 */
VOID
NTAPI
KdUsbSetHostPresent(
    _In_ BOOLEAN Present)
{
    KD_DEBUGGER_NOT_PRESENT = !Present;

    if (Present)
        SharedUserData->KdDebuggerEnabled |= KDUSB_SHARED_HOST_CONNECTED;
    else
        SharedUserData->KdDebuggerEnabled &= ~KDUSB_SHARED_HOST_CONNECTED;
}

/**
 * @brief
 * Checks whether the host just asked for the target's name.
 *
 * @param[in] Length
 * How many bytes of KdUsbReceiveBuffer the last transfer filled.
 *
 * @return
 * TRUE when the receive buffer holds exactly "NAME?".
 */
BOOLEAN
NTAPI
KdUsbIsNameQuery(
    _In_ ULONG Length)
{
    static const CHAR Query[] = "NAME?";

    if (Length != sizeof(Query) - 1)
        return FALSE;

    return RtlCompareMemory(KdUsbReceiveBuffer, Query, sizeof(Query) - 1) == sizeof(Query) - 1;
}

/**
 * @brief
 * Hands a received transfer to the KD packet layer.
 *
 * @param[in] PacketType
 * The packet type the caller is waiting for.
 *
 * @param[in] Length
 * How many bytes of KdUsbReceiveBuffer the transfer filled.
 *
 * @param[out] Packet
 * Receives the KD packet header.
 *
 * @param[in,out] MessageHeader
 * Receives the message header bytes, sized by its MaximumLength.
 *
 * @param[in,out] MessageData
 * Receives whatever follows the message header.
 *
 * @return
 * KdPacketReceived when the buffer held a packet, otherwise the status the
 * packet layer should act on.
 */
KDSTATUS
NTAPI
KdUsbCompletePacket(
    _In_ ULONG PacketType,
    _In_ ULONG Length,
    _Out_ PKD_PACKET Packet,
    _Inout_opt_ PSTRING MessageHeader,
    _Inout_opt_ PSTRING MessageData)
{
    PUCHAR Payload;
    ULONG HeaderLength;
    ULONG DataLength;

    if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN)
    {
        if (Length != 0 && KdUsbReceiveBuffer[0] == BREAKIN_PACKET_BYTE)
            return KdPacketReceived;

        return KdPacketTimedOut;
    }

    if (Length < sizeof(*Packet))
        return KdPacketNeedsResend;

    RtlCopyMemory(Packet, KdUsbReceiveBuffer, sizeof(*Packet));
    if (MessageHeader == NULL)
        return KdPacketReceived;

    Payload = KdUsbReceiveBuffer + sizeof(*Packet);
    HeaderLength = min(MessageHeader->MaximumLength,
                       (ULONG)(sizeof(KdUsbReceiveBuffer) - sizeof(*Packet)));
    RtlCopyMemory(MessageHeader->Buffer, Payload, HeaderLength);

    if (MessageData != NULL && Length > sizeof(*Packet) + MessageHeader->MaximumLength)
    {
        DataLength = Length - sizeof(*Packet) - MessageHeader->MaximumLength;
        DataLength = min(DataLength, MessageData->MaximumLength);
        RtlCopyMemory(MessageData->Buffer, Payload + MessageHeader->MaximumLength, DataLength);
    }

    return KdPacketReceived;
}

/**
 * @brief
 * Adds up the bytes of a buffer the way the KD checksum does.
 *
 * @param[in] Buffer
 * The bytes to add.
 *
 * @param[in] Length
 * How many bytes to add.
 *
 * @return
 * The sum of every byte.
 */
static
ULONG
NTAPI
KdUsbChecksum(
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    PUCHAR Byte = Buffer;
    ULONG Sum = 0;

    while (Length--)
        Sum += *Byte++;

    return Sum;
}

/**
 * @brief
 * Sends a KD control packet with no payload.
 *
 * @param[in] PacketType
 * The control packet type.
 *
 * @param[in] PacketId
 * The packet ID the control packet refers to, or zero.
 */
static
VOID
NTAPI
KdUsbSendControlPacket(
    _In_ USHORT PacketType,
    _In_ ULONG PacketId)
{
    KD_PACKET Packet;
    KDUSB_BUFFER Buffer;

    Packet.PacketLeader = CONTROL_PACKET_LEADER;
    Packet.PacketType = PacketType;
    Packet.ByteCount = 0;
    Packet.PacketId = PacketId;
    Packet.Checksum = 0;

    Buffer.Data = &Packet;
    Buffer.Length = sizeof(Packet);
    KdUsbController->SendBuffers(&Buffer, 1);
}

/**
 * @brief
 * Checks whether a packet that ran out of retries may be dropped.
 *
 * Prints, symbol loads, file opens and trace output are the traffic the target
 * produces on its own. When nobody acknowledges them the host is gone, so the
 * target stops waiting instead of hanging on output nobody will read.
 *
 * @param[in] PacketType
 * The packet type being sent.
 *
 * @param[in] MessageHeader
 * The message header; its first ULONG is the API number or new state.
 *
 * @return
 * TRUE when the packet may be dropped.
 */
static
BOOLEAN
NTAPI
KdUsbIsDroppable(
    _In_ ULONG PacketType,
    _In_ PSTRING MessageHeader)
{
    ULONG Selector = *(PULONG)MessageHeader->Buffer;

    switch (PacketType)
    {
        case PACKET_TYPE_KD_DEBUG_IO:
            return Selector == DbgKdPrintStringApi;

        case PACKET_TYPE_KD_STATE_CHANGE64:
            return Selector == DbgKdLoadSymbolsStateChange;

        case PACKET_TYPE_KD_FILE_IO:
            return Selector == DbgKdCreateFileApi;

        case PACKET_TYPE_KD_TRACE_IO:
            return Selector == DbgKdPrintTraceApi;

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Finds a loader option, ignoring case.
 *
 * @param[in] Options
 * The loader option string.
 *
 * @param[in] Name
 * The upper case option name, including its '=' when it takes a value.
 *
 * @return
 * The character after the name, or NULL when the option is absent.
 */
static
PCSTR
NTAPI
KdUsbFindOption(
    _In_ PCSTR Options,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);

    for (; *Options != ANSI_NULL; Options++)
    {
        if (_strnicmp(Options, Name, Length) == 0)
            return Options + Length;
    }

    return NULL;
}

/**
 * @brief
 * Copies TARGETNAME out of the loader options.
 *
 * Only characters the host accepts in a name are taken: digits, letters,
 * '-' and '_'. Letters are stored in upper case.
 *
 * @param[in] Options
 * The loader option string.
 */
static
VOID
NTAPI
KdUsbParseTargetName(
    _In_ PCSTR Options)
{
    PCSTR Value;
    CHAR Character;
    ULONG Length;

    Value = KdUsbFindOption(Options, "TARGETNAME=");
    if (Value == NULL)
        return;

    for (Length = 0; Length < KDUSB_TARGET_NAME_MAX; Length++)
    {
        Character = RtlUpperChar(Value[Length]);
        if (!(Character >= '0' && Character <= '9') &&
            !(Character >= 'A' && Character <= 'Z') &&
            Character != '-' &&
            Character != '_')
        {
            break;
        }

        KdUsbTargetName[Length] = Character;
    }

    KdUsbTargetNameLength = Length;
}

/**
 * @brief
 * Reads BUSPARAMS=bus.device.function out of the loader options.
 *
 * @param[in] Options
 * The loader option string.
 *
 * @param[out] Bus
 * Receives the bus number.
 *
 * @param[out] Slot
 * Receives the device and function packed the way PCI_SLOT_NUMBER does.
 *
 * @return
 * TRUE when all three fields were present.
 */
static
BOOLEAN
NTAPI
KdUsbParseBusParams(
    _In_ PCSTR Options,
    _Out_ PULONG Bus,
    _Out_ PULONG Slot)
{
    ULONG Field[3];
    PCSTR Value;
    ULONG Index;
    BOOLEAN Digits;

    Value = KdUsbFindOption(Options, "BUSPARAMS=");
    if (Value == NULL)
        return FALSE;

    for (Index = 0; Index < RTL_NUMBER_OF(Field); Index++)
    {
        Field[Index] = 0;
        Digits = FALSE;

        while (*Value >= '0' && *Value <= '9')
        {
            Field[Index] = Field[Index] * 10 + (*Value - '0');
            Digits = TRUE;
            Value++;
        }

        if (!Digits)
            return FALSE;

        /* The last field ends the value; the others end at a dot */
        if (Index == RTL_NUMBER_OF(Field) - 1)
            break;

        if (*Value != '.')
            return FALSE;

        Value++;
    }

    *Bus = Field[0];
    *Slot = (Field[1] & 0x1F) | ((Field[2] & 0x7) << 5);
    return TRUE;
}

/**
 * @brief
 * Points the debug device descriptor at a USB host controller.
 *
 * @param[in] ProgIf
 * The programming interface of the controller to match.
 *
 * @param[in] Bus
 * The bus to match, or 0xFFFFFFFF to match by class.
 *
 * @param[in] Slot
 * The slot to match, or 0xFFFFFFFF to match by class.
 */
static
VOID
NTAPI
KdUsbDescribeController(
    _In_ UCHAR ProgIf,
    _In_ ULONG Bus,
    _In_ ULONG Slot)
{
    RtlZeroMemory(&KdUsbDevice, sizeof(KdUsbDevice));

    KdUsbDevice.Bus = Bus;
    KdUsbDevice.Slot = Slot;
    KdUsbDevice.Segment = 0xFFFF;
    KdUsbDevice.VendorID = 0xFFFF;
    KdUsbDevice.DeviceID = 0xFFFF;
    KdUsbDevice.BaseClass = PCI_CLASS_SERIAL_BUS_CTLR;
    KdUsbDevice.SubClass = PCI_SUBCLASS_SB_USB;
    KdUsbDevice.ProgIf = ProgIf;
    KdUsbDevice.PortType = KDUSB_DBG2_PORT_TYPE_USB;
    KdUsbDevice.NameSpace = KdNameSpacePCI;
}

/**
 * @brief
 * Finds a USB debug controller at boot and brings up the link on it.
 *
 * An xHCI debug capability is tried first and an EHCI debug port second. The
 * EHCI path counts as up even with no debug cable attached, so the host can
 * plug in later.
 *
 * @param[in] LoaderBlock
 * The loader block.
 *
 * @return
 * STATUS_SUCCESS when a controller is driving the link.
 */
static
NTSTATUS
NTAPI
KdUsbBringUpLink(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PCSTR Options = LoaderBlock->LoadOptions;
    ULONG Bus = 0xFFFFFFFF;
    ULONG Slot = 0xFFFFFFFF;

    RtlZeroMemory(KdUsbTargetName, sizeof(KdUsbTargetName));
    KdUsbTargetNameLength = 0;

    if (Options == NULL)
        return STATUS_UNSUCCESSFUL;

    KdUsbParseTargetName(Options);
    if (KdUsbParseBusParams(Options, &Bus, &Slot))
        KdUsbTrace("BUSPARAMS bus %lu slot 0x%lx\n", Bus, Slot);

    KdUsbTrace("target name '%s'\n", KdUsbTargetName);

    KdUsbDescribeController(KDUSB_PROGIF_XHCI, Bus, Slot);
    KdUsbController = &KdUsbXhciController;
    KdUsbTrace("trying xHCI\n");

    if (KdUsbController->Initialize(LoaderBlock))
    {
        KdUsbTrace("xHCI link up\n");
        return STATUS_SUCCESS;
    }

    KdUsbDescribeController(KDUSB_PROGIF_EHCI, Bus, Slot);
    KdUsbController = &KdUsbEhciController;
    KdUsbTrace("xHCI failed, trying EHCI\n");

    if (KdUsbController->Initialize(LoaderBlock))
    {
        KdUsbTrace("EHCI link up\n");
        return STATUS_SUCCESS;
    }

    KdUsbTrace("EHCI failed, no USB debug link\n");
    KD_DEBUGGER_NOT_PRESENT = TRUE;
    return STATUS_UNSUCCESSFUL;
}

/* KD TRANSPORT EXPORTS *******************************************************/

/**
 * @brief
 * Brings up the USB debug link.
 *
 * With /FRLDRSCREENLOG the boot path is traced on the loader's text screen.
 * That only works until memory management takes the loader's mappings away,
 * so the trace stops before this routine returns.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL to bring back a link that was set up
 * before.
 *
 * @return
 * STATUS_SUCCESS when a controller is driving the link.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize0(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    if (LoaderBlock == NULL)
    {
        KdUsbXhciRequestReset();

        if (KdUsbController != NULL && KdUsbController->Initialize(NULL))
        {
            KdUsbControllerReady = TRUE;
            Status = STATUS_SUCCESS;
        }

        KdUsbResetPacketIds();
        return Status;
    }

#if defined(_M_IX86) || defined(_M_AMD64)
    if (LoaderBlock->LoadOptions != NULL &&
        KdUsbFindOption(LoaderBlock->LoadOptions, "FRLDRSCREENLOG") != NULL)
    {
        KdUsbBootPrint = (PKDUSB_BOOT_PRINT)LoaderBlock->u.I386.CommonDataArea;
    }
#endif

    KdUsbTrace("phase 0, options '%s'\n",
               LoaderBlock->LoadOptions ? LoaderBlock->LoadOptions : "");

    Status = KdUsbBringUpLink(LoaderBlock);
    if (NT_SUCCESS(Status))
        KdUsbControllerReady = TRUE;

    KdUsbTrace("phase 0 done, status 0x%lx\n", Status);
    KdUsbBootPrint = NULL;

    KdUsbResetPacketIds();
    return Status;
}

/**
 * @brief
 * Phase 1 of the debug transport. Nothing is left to do by then.
 *
 * @param[in] LoaderBlock
 * Unused.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize1(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Brings the link back after the system returns to D0.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdD0Transition(VOID)
{
    if (!KdUsbControllerReady &&
        KdUsbController != NULL &&
        KdUsbController->Initialize(NULL))
    {
        KdUsbControllerReady = TRUE;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops the link before the system leaves D0.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdD3Transition(VOID)
{
    if (KdUsbControllerReady)
    {
        KdUsbControllerReady = FALSE;
        KdUsbController->Shutdown();
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Saves transport state before a sleep transition. The USB link keeps none.
 *
 * @param[in] SleepTransition
 * Unused.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdSave(
    _In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Restores transport state after a sleep transition. The USB link keeps none.
 *
 * @param[in] SleepTransition
 * Unused.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
KdRestore(
    _In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Keeps the transport image across hibernation.
 */
VOID
NTAPI
KdSetHiberRange(VOID)
{
    PoSetHiberRange(NULL, PO_MEM_BOOT_PHASE, (PVOID)KdSetHiberRange, 0, KDUSB_HIBER_TAG);
}

/**
 * @brief
 * Receives a KD packet from the host.
 *
 * @param[in] PacketType
 * The packet type wanted. PACKET_TYPE_KD_POLL_BREAKIN only checks for a
 * break-in byte, and PACKET_TYPE_KD_ACKNOWLEDGE waits for the host to accept
 * the packet just sent.
 *
 * @param[out] MessageHeader
 * Receives the message header.
 *
 * @param[out] MessageData
 * Receives the message data.
 *
 * @param[out] DataLength
 * Receives the length of the message data.
 *
 * @param[in,out] KdContext
 * The kernel's KD context; a break-in sets KdpControlCPending.
 *
 * @return
 * KdPacketReceived, KdPacketTimedOut or KdPacketNeedsResend.
 */
KDSTATUS
NTAPI
KdReceivePacket(
    _In_ ULONG PacketType,
    _Out_ PSTRING MessageHeader,
    _Out_ PSTRING MessageData,
    _Out_ PULONG DataLength,
    _Inout_ PKD_CONTEXT KdContext)
{
    KD_PACKET Packet;
    KDSTATUS Status;
    ULONG Checksum;

    if (!KdUsbControllerReady)
        return KdPacketTimedOut;

    for (;;)
    {
        RtlZeroMemory(&Packet, sizeof(Packet));
        Status = KdUsbController->ReadPacket(PacketType, &Packet, MessageHeader, MessageData);
        if (Status == KdPacketReceived)
            KdUsbSetHostPresent(TRUE);

        if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN)
            return (Status == KdPacketReceived) ? KdPacketReceived : KdPacketTimedOut;

        if (Status == KdPacketTimedOut)
            break;

        KdUsbRetriesLeft = KdUsbRetryLimit;
        if (Status != KdPacketReceived)
            break;

        if (Packet.PacketLeader == CONTROL_PACKET_LEADER)
        {
            switch (Packet.PacketType)
            {
                case PACKET_TYPE_KD_RESEND:
                    return KdPacketNeedsResend;

                case PACKET_TYPE_KD_ACKNOWLEDGE:
                    if (PacketType == PACKET_TYPE_KD_ACKNOWLEDGE &&
                        Packet.PacketId == (KdUsbNextPacketId & ~SYNC_PACKET_ID))
                    {
                        KdUsbNextPacketId ^= 1;
                        return KdPacketReceived;
                    }
                    break;

                case PACKET_TYPE_KD_RESET:
                    KdUsbResetPacketIds();
                    KdUsbSendControlPacket(PACKET_TYPE_KD_RESET, 0);
                    return KdPacketNeedsResend;
            }

            continue;
        }

        if (PacketType == PACKET_TYPE_KD_ACKNOWLEDGE)
        {
            /* A new data packet acknowledges ours; ask for it again later */
            if (Packet.PacketId == KdUsbRemotePacketId)
            {
                KdUsbSendControlPacket(PACKET_TYPE_KD_RESEND, 0);
                KdUsbNextPacketId ^= 1;
                return KdPacketReceived;
            }

            KdUsbSendControlPacket(PACKET_TYPE_KD_ACKNOWLEDGE, Packet.PacketId);
            continue;
        }

        if (Packet.ByteCount <= PACKET_MAX_SIZE &&
            Packet.ByteCount >= MessageHeader->MaximumLength)
        {
            *DataLength = Packet.ByteCount - MessageHeader->MaximumLength;
            MessageHeader->Length = MessageHeader->MaximumLength;
            MessageData->Length = (USHORT)*DataLength;

            /* Unwanted or repeated packets are acknowledged and dropped */
            if (Packet.PacketType != PacketType)
            {
                KdUsbSendControlPacket(PACKET_TYPE_KD_ACKNOWLEDGE, Packet.PacketId);
                continue;
            }

            if ((Packet.PacketId & ~1) == INITIAL_PACKET_ID)
            {
                if (Packet.PacketId != KdUsbRemotePacketId)
                {
                    KdUsbSendControlPacket(PACKET_TYPE_KD_ACKNOWLEDGE, Packet.PacketId);
                    continue;
                }

                Checksum = KdUsbChecksum(MessageHeader->Buffer, MessageHeader->Length) +
                           KdUsbChecksum(MessageData->Buffer, MessageData->Length);
                if (Checksum == Packet.Checksum)
                {
                    KdUsbSendControlPacket(PACKET_TYPE_KD_ACKNOWLEDGE, Packet.PacketId);
                    KdUsbRemotePacketId ^= 1;
                    return KdPacketReceived;
                }
            }
        }

        KdUsbSendControlPacket(PACKET_TYPE_KD_RESEND, 0);
    }

    if (*(PUCHAR)&Packet == BREAKIN_PACKET_BYTE)
    {
        KdContext->KdpControlCPending = TRUE;
        return KdPacketNeedsResend;
    }

    return Status;
}

/**
 * @brief
 * Sends a KD packet to the host and waits until it is acknowledged.
 *
 * @param[in] PacketType
 * The packet type.
 *
 * @param[in] MessageHeader
 * The message header.
 *
 * @param[in] MessageData
 * The message data, if the packet carries any.
 *
 * @param[in,out] KdContext
 * The kernel's KD context; its retry count applies to the next packet.
 */
VOID
NTAPI
KdSendPacket(
    _In_ ULONG PacketType,
    _In_ PSTRING MessageHeader,
    _In_opt_ PSTRING MessageData,
    _Inout_ PKD_CONTEXT KdContext)
{
    KDUSB_BUFFER Buffers[3];
    KD_PACKET Packet;
    KDSTATUS Status;
    ULONG DataLength;
    ULONG Count;

    if (!KdUsbControllerReady)
        return;

    DataLength = (MessageData != NULL) ? MessageData->Length : 0;

    Packet.PacketLeader = PACKET_LEADER;
    Packet.PacketType = (USHORT)PacketType;
    Packet.ByteCount = (USHORT)(MessageHeader->Length + DataLength);
    Packet.Checksum = KdUsbChecksum(MessageHeader->Buffer, MessageHeader->Length);
    if (DataLength != 0)
        Packet.Checksum += KdUsbChecksum(MessageData->Buffer, DataLength);

    KdUsbRetriesLeft = KdUsbRetryLimit;

    for (;;)
    {
        if (KdUsbRetriesLeft == 0 && KdUsbIsDroppable(PacketType, MessageHeader))
        {
            KdUsbSetHostPresent(FALSE);
            KdUsbResetPacketIds();
            return;
        }

        Packet.PacketId = KdUsbNextPacketId;

        Buffers[0].Data = &Packet;
        Buffers[0].Length = sizeof(Packet);
        Buffers[1].Data = MessageHeader->Buffer;
        Buffers[1].Length = MessageHeader->Length;
        Count = 2;

        if (DataLength != 0)
        {
            Buffers[2].Data = MessageData->Buffer;
            Buffers[2].Length = DataLength;
            Count = 3;
        }

        KdUsbController->SendBuffers(Buffers, Count);

        Status = KdReceivePacket(PACKET_TYPE_KD_ACKNOWLEDGE, NULL, NULL, NULL, KdContext);
        if (Status == KdPacketTimedOut)
        {
            KdUsbRetriesLeft--;
        }
        else if (Status == KdPacketReceived)
        {
            KdUsbNextPacketId &= ~SYNC_PACKET_ID;
            KdUsbRetryLimit = KdContext->KdpDefaultRetries;
            return;
        }
    }
}
