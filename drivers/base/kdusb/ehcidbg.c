/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     EHCI Debug Port backend
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/* EHCI capability registers */
#define EHCI_CAPLENGTH              0x00
#define EHCI_HCSPARAMS              0x04

#define EHCI_HCSPARAMS_PORTS(x)     ((x) & 0xF)
#define EHCI_HCSPARAMS_PPC          0x00000010
#define EHCI_HCSPARAMS_DEBUG(x)     (((x) >> 20) & 0xF)

/* EHCI operational registers */
#define EHCI_USBCMD                 0x00
#define EHCI_USBSTS                 0x04
#define EHCI_FRINDEX                0x0C
#define EHCI_PORTSC(Port)           (0x44 + ((Port) - 1) * 4)

#define EHCI_USBCMD_RUN             0x00000001
#define EHCI_USBCMD_HCRESET         0x00000002
#define EHCI_USBCMD_SCHEDULES       0x00000030
#define EHCI_USBSTS_HALTED          0x00001000
#define EHCI_FRINDEX_MASK           0x00003FFF

#define EHCI_PORTSC_CCS             0x00000001
#define EHCI_PORTSC_PED             0x00000004
#define EHCI_PORTSC_PR              0x00000100
#define EHCI_PORTSC_PP              0x00001000

/* PCI Debug Port capability: BAR number in bits 31:29, offset in bits 28:16 */
#define EHCI_DEBUG_CAP_BAR(x)       ((x) >> 29)
#define EHCI_DEBUG_CAP_OFFSET(x)    (((x) >> 16) & 0x1FFF)
#define EHCI_DEBUG_CAP_FIRST_BAR    1
#define EHCI_PCI_CAPABILITY_LIMIT   48

/* Debug Port registers */
#define DBGP_CONTROL                0x00
#define DBGP_PIDS                   0x04
#define DBGP_DATA_LOW               0x08
#define DBGP_DATA_HIGH              0x0C
#define DBGP_ADDRESS                0x10

#define DBGP_LENGTH_MASK            0x0000000F
#define DBGP_WRITE                  0x00000010
#define DBGP_GO                     0x00000020
#define DBGP_ERROR                  0x00000040
#define DBGP_IN_USE                 0x00000400
#define DBGP_DONE                   0x00010000
#define DBGP_ENABLED                0x10000000
#define DBGP_OWNER                  0x40000000

#define DBGP_MAKE_PIDS(Token, Send) ((ULONG)(Token) | ((ULONG)(Send) << 8))
#define DBGP_RECEIVED_PID(x)        ((UCHAR)((x) >> 16))
#define DBGP_TARGET(Address, Ep)    (((ULONG)((Address) & 0x7F) << 8) | ((Ep) & 0xF))

/* Largest packet the debug port moves */
#define EHCI_PACKET_SIZE            8

/* USB packet IDs */
#define USB_PID_OUT                 0xE1
#define USB_PID_IN                  0x69
#define USB_PID_SETUP               0x2D
#define USB_PID_DATA0               0xC3
#define USB_PID_DATA1               0x4B
#define USB_PID_ACK                 0xD2
#define USB_PID_NAK                 0x5A
#define USB_PID_NYET                0x96
#define USB_PID_STALL               0x1E

/* Standard requests used to put the debug device in debug mode */
#define USB_DIR_IN                  0x80
#define USB_REQUEST_GET_DESCRIPTOR  6
#define USB_REQUEST_SET_ADDRESS     5
#define USB_REQUEST_SET_FEATURE     3
#define USB_DEBUG_DESCRIPTOR_TYPE   10
#define USB_FEATURE_DEBUG_MODE      6
#define EHCI_DEBUG_ADDRESS          127

/* Retry budgets */
#define EHCI_ERROR_RETRIES          3
#define EHCI_CONTROL_NAKS           1000
#define EHCI_REQUEST_ATTEMPTS       3
#define EHCI_SEND_ATTEMPTS          100000
#define EHCI_RECEIVE_ATTEMPTS       100000
#define EHCI_ATTACH_ATTEMPTS        20
#define EHCI_EMPTY_PORT_ATTEMPTS    6

/* Waits, in 1 ms frames */
#define EHCI_PORT_RESET_FRAMES      20
#define EHCI_PORT_RECOVERY_FRAMES   5
#define EHCI_SETTLE_FRAMES          20
#define EHCI_ATTACH_RETRY_FRAMES    500

/* Waits, in microseconds */
#define EHCI_CONTROLLER_TIMEOUT     2000000
#define EHCI_TRANSACTION_TIMEOUT    500000

#define EHCI_GONE                   0xFFFFFFFF

/** How a transaction, or a run of retries of one, ended. */
typedef enum _EHCI_RESULT
{
    EhciSuccess,
    EhciBusy,
    EhciFailed
} EHCI_RESULT;

/** What a port reset found. */
typedef enum _EHCI_PORT_STATE
{
    EhciPortEmpty,
    EhciPortReset,
    EhciPortFailed
} EHCI_PORT_STATE;

#include <pshpack1.h>
typedef struct _USB_SETUP
{
    UCHAR RequestType;
    UCHAR Request;
    USHORT Value;
    USHORT Index;
    USHORT Length;
} USB_SETUP, *PUSB_SETUP;

typedef struct _USB_DEBUG_DESCRIPTOR
{
    UCHAR Length;
    UCHAR DescriptorType;
    UCHAR InEndpoint;
    UCHAR OutEndpoint;
} USB_DEBUG_DESCRIPTOR, *PUSB_DEBUG_DESCRIPTOR;
#include <poppack.h>

C_ASSERT(sizeof(USB_SETUP) == EHCI_PACKET_SIZE);

/** One bulk endpoint of the debug device. */
typedef struct _EHCI_PIPE
{
    UCHAR Endpoint;
    UCHAR Toggle;
} EHCI_PIPE, *PEHCI_PIPE;

typedef struct _EHCI_DEBUG
{
    PUCHAR Capabilities;
    PUCHAR Operational;
    PUCHAR Registers;
    ULONG PortStatus;
    UCHAR Address;
    EHCI_PIPE In;
    EHCI_PIPE Out;
    ULONG Received;
    BOOLEAN TransferReady;
    BOOLEAN Discarding;
    BOOLEAN NeedsAttach;
} EHCI_DEBUG, *PEHCI_DEBUG;

static EHCI_DEBUG EhciDebug;

/* REGISTER ACCESS ************************************************************/

static
ULONG
NTAPI
EhciReadOp(
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(EhciDebug.Operational + Offset));
}

static
VOID
NTAPI
EhciWriteOp(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(EhciDebug.Operational + Offset), Value);
    KeMemoryBarrier();
}

static
ULONG
NTAPI
EhciReadPort(
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(EhciDebug.Registers + Offset));
}

static
VOID
NTAPI
EhciWritePort(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(EhciDebug.Registers + Offset), Value);
    KeMemoryBarrier();
}

/**
 * @brief
 * Polls an operational register until the masked bits match.
 *
 * @param[in] Offset
 * The operational register.
 *
 * @param[in] Mask
 * The bits to compare.
 *
 * @param[in] Value
 * The value the masked bits must reach.
 *
 * @return
 * TRUE when the bits matched within EHCI_CONTROLLER_TIMEOUT.
 */
static
BOOLEAN
NTAPI
EhciPollOp(
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value)
{
    ULONG Register;
    ULONG Waited;

    for (Waited = 0; ; Waited += 5)
    {
        Register = EhciReadOp(Offset);
        if (Register == EHCI_GONE)
            return FALSE;

        if ((Register & Mask) == Value)
            return TRUE;

        if (Waited >= EHCI_CONTROLLER_TIMEOUT)
            return FALSE;

        KdUsbStall(5);
    }
}

/**
 * @brief
 * Waits for a number of USB frames to pass on the frame counter.
 *
 * @param[in] Frames
 * How many 1 ms frames to wait.
 *
 * @return
 * FALSE when the controller stopped or disappeared first.
 */
static
BOOLEAN
NTAPI
EhciWaitFrames(
    _In_ ULONG Frames)
{
    ULONG Start;
    ULONG Now;

    Start = EhciReadOp(EHCI_FRINDEX);
    if (Start == EHCI_GONE)
        return FALSE;

    /* FRINDEX counts microframes, eight to a frame */
    do
    {
        if (!(EhciReadOp(EHCI_USBCMD) & EHCI_USBCMD_RUN))
            return FALSE;

        Now = EhciReadOp(EHCI_FRINDEX);
        if (Now == EHCI_GONE)
            return FALSE;
    } while (((Now - Start) & EHCI_FRINDEX_MASK) <= Frames * 8);

    return TRUE;
}

/**
 * @brief
 * Stops the controller.
 *
 * @return
 * TRUE once the controller reports itself halted.
 */
static
BOOLEAN
NTAPI
EhciHalt(VOID)
{
    ULONG Command = EhciReadOp(EHCI_USBCMD);

    if (Command == EHCI_GONE)
        return FALSE;

    if (Command & EHCI_USBCMD_RUN)
        EhciWriteOp(EHCI_USBCMD, Command & ~EHCI_USBCMD_RUN);

    return EhciPollOp(EHCI_USBSTS, EHCI_USBSTS_HALTED, EHCI_USBSTS_HALTED);
}

/* TRANSACTIONS ***************************************************************/

/**
 * @brief
 * Runs one transaction on the debug port and waits for it to finish.
 *
 * @param[in] Pids
 * The token PID and, for a write, the data PID.
 *
 * @param[in] Write
 * TRUE when Length bytes from the data registers go out.
 *
 * @param[in] Length
 * Bytes to send.
 *
 * @param[out] Control
 * Receives the control register once the transaction is done.
 *
 * @return
 * FALSE when the controller vanished or the transaction never finished.
 */
static
BOOLEAN
NTAPI
EhciExecute(
    _In_ ULONG Pids,
    _In_ BOOLEAN Write,
    _In_ ULONG Length,
    _Out_ PULONG Control)
{
    ULONG Value;
    ULONG Waited;

    EhciWritePort(DBGP_PIDS, Pids);

    Value = EhciReadPort(DBGP_CONTROL);
    if (Value == EHCI_GONE)
        return FALSE;

    Value &= ~(DBGP_DONE | DBGP_WRITE | DBGP_LENGTH_MASK);
    if (Write)
        Value |= DBGP_WRITE | Length;

    EhciWritePort(DBGP_CONTROL, Value | DBGP_GO);

    for (Waited = 0; ; Waited += 5)
    {
        Value = EhciReadPort(DBGP_CONTROL);
        if (Value == EHCI_GONE)
            return FALSE;

        if (Value & DBGP_DONE)
            break;

        if (Waited >= EHCI_TRANSACTION_TIMEOUT)
            return FALSE;

        KdUsbStall(5);
    }

    /* DONE is cleared by writing it back */
    EhciWritePort(DBGP_CONTROL, Value);
    *Control = Value;
    return TRUE;
}

/**
 * @brief
 * Runs a transaction until the device gives a final answer.
 *
 * Bus errors and unexpected answers, such as a repeated packet that still
 * carries the old toggle, are retried EHCI_ERROR_RETRIES times.
 *
 * @param[in] Token
 * USB_PID_IN, USB_PID_OUT or USB_PID_SETUP, to the address and endpoint
 * already in DBGP_ADDRESS.
 *
 * @param[in,out] Toggle
 * The data PID to send, or for an IN the one expected. Flipped on success.
 *
 * @param[in,out] Data
 * The packet to send, or receives the packet read.
 *
 * @param[in,out] Length
 * Bytes to send; for an IN, receives the bytes read.
 *
 * @param[in] NakRetries
 * How many NAKs to ride out before reporting the device busy.
 *
 * @return
 * EhciSuccess, EhciBusy when the device kept answering NAK, or EhciFailed.
 */
static
EHCI_RESULT
NTAPI
EhciTransact(
    _In_ UCHAR Token,
    _Inout_ PUCHAR Toggle,
    _Inout_updates_bytes_(EHCI_PACKET_SIZE) PUCHAR Data,
    _Inout_ PULONG Length,
    _In_ ULONG NakRetries)
{
    BOOLEAN In = (Token == USB_PID_IN);
    ULONG Words[2] = { 0, 0 };
    ULONG Control;
    ULONG Errors = 0;
    ULONG Naks = 0;
    UCHAR Answer;

    if (!In)
        RtlCopyMemory(Words, Data, *Length);

    for (;;)
    {
        if (!In)
        {
            EhciWritePort(DBGP_DATA_LOW, Words[0]);
            EhciWritePort(DBGP_DATA_HIGH, Words[1]);
        }

        if (!EhciExecute(DBGP_MAKE_PIDS(Token, In ? 0 : *Toggle), !In, *Length, &Control))
            return EhciFailed;

        if (!(Control & DBGP_ERROR))
        {
            Answer = DBGP_RECEIVED_PID(EhciReadPort(DBGP_PIDS));
            if (Answer == USB_PID_STALL)
                return EhciFailed;

            if (Answer == USB_PID_NAK)
            {
                if (Naks++ >= NakRetries)
                    return EhciBusy;

                continue;
            }

            if (In && Answer == *Toggle)
            {
                *Length = Control & DBGP_LENGTH_MASK;
                if (*Length != 0)
                {
                    Words[0] = EhciReadPort(DBGP_DATA_LOW);
                    Words[1] = EhciReadPort(DBGP_DATA_HIGH);
                    RtlCopyMemory(Data, Words, *Length);
                }

                *Toggle ^= USB_PID_DATA0 ^ USB_PID_DATA1;
                return EhciSuccess;
            }

            if (!In && (Answer == USB_PID_ACK || Answer == USB_PID_NYET))
            {
                *Toggle ^= USB_PID_DATA0 ^ USB_PID_DATA1;
                return EhciSuccess;
            }
        }

        if (++Errors >= EHCI_ERROR_RETRIES)
            return EhciFailed;
    }
}

/**
 * @brief
 * Points the debug port at a device endpoint.
 */
static
VOID
NTAPI
EhciSelect(
    _In_ UCHAR Address,
    _In_ UCHAR Endpoint)
{
    EhciWritePort(DBGP_ADDRESS, DBGP_TARGET(Address, Endpoint));
}

/**
 * @brief
 * Runs a control transfer on endpoint 0.
 *
 * @param[in] Address
 * The device address.
 *
 * @param[in] Setup
 * The setup packet. USB_DIR_IN in RequestType makes it a read.
 *
 * @param[out] Buffer
 * Receives the data of a read. Unused for a request without data.
 *
 * @param[in,out] Length
 * For a read, the buffer size; receives how many bytes came back.
 *
 * @return
 * TRUE when the device accepted the request.
 */
static
BOOLEAN
NTAPI
EhciControl(
    _In_ UCHAR Address,
    _In_ PUSB_SETUP Setup,
    _Out_writes_bytes_opt_(*Length) PVOID Buffer,
    _Inout_opt_ PULONG Length)
{
    UCHAR Packet[EHCI_PACKET_SIZE];
    UCHAR Toggle = USB_PID_DATA0;
    UCHAR StatusToggle = USB_PID_DATA1;
    ULONG Size = sizeof(*Setup);

    EhciSelect(Address, 0);

    RtlCopyMemory(Packet, Setup, sizeof(*Setup));
    if (EhciTransact(USB_PID_SETUP, &Toggle, Packet, &Size, 0) != EhciSuccess)
        return FALSE;

    /* Without a data stage, the status stage is an empty IN */
    if (!(Setup->RequestType & USB_DIR_IN))
    {
        Size = 0;
        return EhciTransact(USB_PID_IN, &Toggle, Packet, &Size, EHCI_CONTROL_NAKS) == EhciSuccess &&
               Size == 0;
    }

    Size = 0;
    if (EhciTransact(USB_PID_IN, &Toggle, Packet, &Size, EHCI_CONTROL_NAKS) != EhciSuccess)
        return FALSE;

    *Length = min(*Length, Size);
    RtlCopyMemory(Buffer, Packet, *Length);

    /* Some debug devices never answer the status stage, so its outcome is ignored */
    Size = 0;
    EhciTransact(USB_PID_OUT, &StatusToggle, Packet, &Size, EHCI_CONTROL_NAKS);
    return TRUE;
}

/* BULK TRANSFERS *************************************************************/

/**
 * @brief
 * Pulls the next packet of a host transfer into KdUsbReceiveBuffer.
 *
 * A transfer ends on a packet shorter than EHCI_PACKET_SIZE. One that grows
 * past KDUSB_TRANSFER_SIZE cannot be a KD packet and is thrown away.
 *
 * @return
 * EhciSuccess when a packet came in, EhciBusy when the device had none.
 */
static
EHCI_RESULT
NTAPI
EhciPollReceive(VOID)
{
    UCHAR Packet[EHCI_PACKET_SIZE];
    EHCI_RESULT Result;
    ULONG Length = 0;

    EhciSelect(EhciDebug.Address, EhciDebug.In.Endpoint);
    Result = EhciTransact(USB_PID_IN, &EhciDebug.In.Toggle, Packet, &Length, 0);
    if (Result != EhciSuccess)
        return Result;

    if (EhciDebug.Received + Length > KDUSB_TRANSFER_SIZE)
    {
        EhciDebug.Discarding = TRUE;
        EhciDebug.Received = 0;
    }

    if (!EhciDebug.Discarding)
    {
        RtlCopyMemory(KdUsbReceiveBuffer + EhciDebug.Received, Packet, Length);
        EhciDebug.Received += Length;
    }

    if (Length < EHCI_PACKET_SIZE)
    {
        EhciDebug.TransferReady = !EhciDebug.Discarding;
        EhciDebug.Discarding = FALSE;
        if (!EhciDebug.TransferReady)
            EhciDebug.Received = 0;
    }

    return EhciSuccess;
}

/**
 * @brief
 * Sends one packet to the debug device.
 *
 * The device takes nothing from the target while its buffer toward the
 * target is full, so host data is pulled in while the device answers NAK.
 * The next read hands that data out.
 *
 * @param[in] Data
 * The packet.
 *
 * @param[in] Length
 * Its length, up to EHCI_PACKET_SIZE.
 *
 * @return
 * The result of the last attempt.
 */
static
EHCI_RESULT
NTAPI
EhciSendPacket(
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ ULONG Length)
{
    EHCI_RESULT Result = EhciBusy;
    ULONG Attempt;
    ULONG Size;

    for (Attempt = 0; Attempt < EHCI_SEND_ATTEMPTS && Result == EhciBusy; Attempt++)
    {
        if (!EhciDebug.TransferReady)
            EhciPollReceive();

        Size = Length;
        EhciSelect(EhciDebug.Address, EhciDebug.Out.Endpoint);
        Result = EhciTransact(USB_PID_OUT, &EhciDebug.Out.Toggle, Data, &Size, 0);
    }

    return Result;
}

/* DEVICE SETUP ***************************************************************/

/**
 * @brief
 * Finds the debug device's endpoints and switches it into debug mode.
 *
 * @return
 * TRUE once the device answers at EHCI_DEBUG_ADDRESS in debug mode.
 */
static
BOOLEAN
NTAPI
EhciSetupDevice(VOID)
{
    USB_DEBUG_DESCRIPTOR Descriptor;
    USB_SETUP Setup;
    ULONG Length;
    ULONG Attempt;

    Setup.RequestType = USB_DIR_IN;
    Setup.Request = USB_REQUEST_GET_DESCRIPTOR;
    Setup.Value = USB_DEBUG_DESCRIPTOR_TYPE << 8;
    Setup.Index = 0;
    Setup.Length = sizeof(Descriptor);

    for (Attempt = 0; ; Attempt++)
    {
        if (Attempt == EHCI_REQUEST_ATTEMPTS)
            return FALSE;

        Length = sizeof(Descriptor);
        if (EhciControl(0, &Setup, &Descriptor, &Length) && Length == sizeof(Descriptor))
            break;
    }

    Setup.RequestType = 0;
    Setup.Request = USB_REQUEST_SET_ADDRESS;
    Setup.Value = EHCI_DEBUG_ADDRESS;
    Setup.Length = 0;
    if (!EhciControl(0, &Setup, NULL, NULL))
        return FALSE;

    Setup.Request = USB_REQUEST_SET_FEATURE;
    Setup.Value = USB_FEATURE_DEBUG_MODE;
    if (!EhciControl(EHCI_DEBUG_ADDRESS, &Setup, NULL, NULL))
        return FALSE;

    EhciDebug.Address = EHCI_DEBUG_ADDRESS;
    EhciDebug.In.Endpoint = Descriptor.InEndpoint;
    EhciDebug.In.Toggle = USB_PID_DATA0;
    EhciDebug.Out.Endpoint = Descriptor.OutEndpoint;
    EhciDebug.Out.Toggle = USB_PID_DATA0;
    return TRUE;
}

/**
 * @brief
 * Resets the root port behind the debug port and moves an attached device
 * over to the debug port. The controller must be running.
 *
 * @return
 * EhciPortEmpty without a device, EhciPortReset once the reset ran, or
 * EhciPortFailed when the controller stopped underneath.
 */
static
EHCI_PORT_STATE
NTAPI
EhciResetPort(VOID)
{
    ULONG Port = EhciReadOp(EhciDebug.PortStatus);

    if (!(Port & EHCI_PORTSC_CCS))
        return EhciPortEmpty;

    EhciWriteOp(EhciDebug.PortStatus, (Port & ~EHCI_PORTSC_PED) | EHCI_PORTSC_PR);
    if (!EhciWaitFrames(EHCI_PORT_RESET_FRAMES))
        return EhciPortFailed;

    EhciWriteOp(EhciDebug.PortStatus, EhciReadOp(EhciDebug.PortStatus) & ~EHCI_PORTSC_PR);
    if (!EhciWaitFrames(EHCI_PORT_RECOVERY_FRAMES))
        return EhciPortFailed;

    /* An enabled port goes to the debug port and is disabled for the host controller */
    Port = EhciReadOp(EhciDebug.PortStatus);
    if ((Port & (EHCI_PORTSC_CCS | EHCI_PORTSC_PED)) == (EHCI_PORTSC_CCS | EHCI_PORTSC_PED))
    {
        EhciWritePort(DBGP_CONTROL, EhciReadPort(DBGP_CONTROL) | DBGP_ENABLED);
        EhciWriteOp(EhciDebug.PortStatus, EhciReadOp(EhciDebug.PortStatus) & ~EHCI_PORTSC_PED);
    }

    return EhciPortReset;
}

/**
 * @brief
 * Puts the device on an enabled debug port into debug mode.
 *
 * @return
 * TRUE when the debug device is ready for bulk transfers.
 */
static
BOOLEAN
NTAPI
EhciConfigureDevice(VOID)
{
    if (!(EhciReadPort(DBGP_CONTROL) & DBGP_ENABLED))
        return FALSE;

    if (!EhciSetupDevice())
        return FALSE;

    EhciWritePort(DBGP_CONTROL, EhciReadPort(DBGP_CONTROL) | DBGP_IN_USE);
    return TRUE;
}

/**
 * @brief
 * Brings the debug device back after the link broke.
 *
 * When the EHCI driver runs the controller, the debug port is released and
 * claimed again. When the controller is stopped, it is run without schedules
 * just long enough to reset the port, then put back the way it was found.
 *
 * @return
 * TRUE when the debug device is ready again.
 */
static
BOOLEAN
NTAPI
EhciReattach(VOID)
{
    ULONG Command;
    ULONG Control;
    BOOLEAN Ready;

    Command = EhciReadOp(EHCI_USBCMD);
    if (Command == EHCI_GONE)
        return FALSE;

    if (Command & EHCI_USBCMD_RUN)
    {
        Control = EhciReadPort(DBGP_CONTROL) & ~(DBGP_OWNER | DBGP_ENABLED);
        EhciWritePort(DBGP_CONTROL, Control);
        EhciWritePort(DBGP_CONTROL, Control | DBGP_OWNER);

        return EhciResetPort() == EhciPortReset && EhciConfigureDevice();
    }

    EhciWriteOp(EHCI_USBCMD, (Command & ~EHCI_USBCMD_SCHEDULES) | EHCI_USBCMD_RUN);
    Ready = EhciResetPort() == EhciPortReset && EhciHalt() && EhciConfigureDevice();
    EhciWriteOp(EHCI_USBCMD, Command);

    return Ready;
}

/**
 * @brief
 * Tries for a while to bring up the debug device, then halts the controller.
 *
 * A missing device is not a failure: the link is marked for setup on the
 * next transfer, so the cable can be plugged in later.
 *
 * @return
 * FALSE only when the controller misbehaved.
 */
static
BOOLEAN
NTAPI
EhciAttachAtBoot(VOID)
{
    EHCI_PORT_STATE State;
    ULONG EmptyInARow = 0;
    ULONG Attempt;
    BOOLEAN Ready = FALSE;

    for (Attempt = 0; Attempt < EHCI_ATTACH_ATTEMPTS; Attempt++)
    {
        State = EhciResetPort();
        if (State == EhciPortReset && EhciConfigureDevice())
        {
            Ready = TRUE;
            break;
        }

        EmptyInARow = (State == EhciPortEmpty) ? EmptyInARow + 1 : 0;
        if (EmptyInARow == EHCI_EMPTY_PORT_ATTEMPTS)
            break;

        if (!EhciWaitFrames(EHCI_ATTACH_RETRY_FRAMES))
            return FALSE;
    }

    KdUsbTrace("EHCI device %s\n", Ready ? "ready" : "not ready");

    if (!EhciHalt())
        return FALSE;

    if (!Ready)
    {
        KdUsbSetHostPresent(FALSE);
        EhciDebug.NeedsAttach = TRUE;
    }

    return TRUE;
}

/**
 * @brief
 * Resets the controller, claims the debug port and runs the controller.
 *
 * @return
 * TRUE when the controller is running with the debug port owned.
 */
static
BOOLEAN
NTAPI
EhciStartController(VOID)
{
    ULONG Params;

    if (!EhciHalt())
        return FALSE;

    EhciWriteOp(EHCI_USBCMD, EhciReadOp(EHCI_USBCMD) | EHCI_USBCMD_HCRESET);
    if (!EhciPollOp(EHCI_USBCMD, EHCI_USBCMD_HCRESET, 0))
        return FALSE;

    EhciWritePort(DBGP_CONTROL, EhciReadPort(DBGP_CONTROL) | DBGP_OWNER | DBGP_IN_USE);

    EhciWriteOp(EHCI_USBCMD, EhciReadOp(EHCI_USBCMD) | EHCI_USBCMD_RUN);
    if (!EhciPollOp(EHCI_USBSTS, EHCI_USBSTS_HALTED, 0))
        return FALSE;

    Params = READ_REGISTER_ULONG((PULONG)(EhciDebug.Capabilities + EHCI_HCSPARAMS));
    if (Params & EHCI_HCSPARAMS_PPC)
        EhciWriteOp(EhciDebug.PortStatus, EhciReadOp(EhciDebug.PortStatus) | EHCI_PORTSC_PP);

    return EhciWaitFrames(EHCI_SETTLE_FRAMES);
}

/**
 * @brief
 * Reads from the controller's PCI configuration space.
 *
 * @return
 * TRUE when all Length bytes were read.
 */
static
BOOLEAN
NTAPI
EhciReadConfig(
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    return KdGetPciDataByOffset(KdUsbDevice.Bus, KdUsbDevice.Slot, Buffer, Offset, Length) == Length;
}

/**
 * @brief
 * Finds the debug port registers through the PCI Debug Port capability.
 *
 * @return
 * The debug port registers, or NULL when the controller has none in its
 * first BAR.
 */
static
PUCHAR
NTAPI
EhciFindDebugRegisters(VOID)
{
    USHORT Status;
    UCHAR Offset;
    ULONG Header;
    ULONG Visited;

    if (KdGetPciDataByOffset == NULL)
        return NULL;

    if (!EhciReadConfig(FIELD_OFFSET(PCI_COMMON_HEADER, Status), &Status, sizeof(Status)) ||
        !(Status & PCI_STATUS_CAPABILITIES_LIST))
    {
        return NULL;
    }

    if (!EhciReadConfig(FIELD_OFFSET(PCI_COMMON_HEADER, u.type0.CapabilitiesPtr), &Offset, sizeof(Offset)))
        return NULL;

    /* Bounded so a looping capability list cannot hang the boot */
    for (Visited = 0; Visited < EHCI_PCI_CAPABILITY_LIMIT; Visited++)
    {
        Offset &= 0xFC;
        if (Offset == 0 || !EhciReadConfig(Offset, &Header, sizeof(Header)))
            return NULL;

        if ((Header & 0xFF) == PCI_CAPABILITY_ID_DEBUG_PORT)
        {
            if (EHCI_DEBUG_CAP_BAR(Header) != EHCI_DEBUG_CAP_FIRST_BAR)
                return NULL;

            return EhciDebug.Capabilities + EHCI_DEBUG_CAP_OFFSET(Header);
        }

        Offset = (UCHAR)(Header >> 8);
    }

    return NULL;
}

/**
 * @brief
 * Maps out the controller's registers and checks the debug port number.
 *
 * @return
 * TRUE when the controller has a usable debug port.
 */
static
BOOLEAN
NTAPI
EhciLocateController(VOID)
{
    PDEBUG_DEVICE_ADDRESS Bar = &KdUsbDevice.BaseAddress[0];
    ULONG Params;
    ULONG Port;

    if (KdUsbDevice.BaseClass != PCI_CLASS_SERIAL_BUS_CTLR ||
        KdUsbDevice.SubClass != PCI_SUBCLASS_SB_USB ||
        KdUsbDevice.ProgIf != KDUSB_PROGIF_EHCI ||
        Bar->Type != CmResourceTypeMemory ||
        !Bar->Valid ||
        Bar->TranslatedAddress == NULL)
    {
        return FALSE;
    }

    EhciDebug.Capabilities = Bar->TranslatedAddress;
    if (READ_REGISTER_ULONG((PULONG)EhciDebug.Capabilities) == EHCI_GONE)
        return FALSE;

    EhciDebug.Operational = EhciDebug.Capabilities + READ_REGISTER_UCHAR(EhciDebug.Capabilities + EHCI_CAPLENGTH);
    EhciDebug.Registers = EhciFindDebugRegisters();
    if (EhciDebug.Registers == NULL)
    {
        KdUsbTrace("no EHCI debug port registers\n");
        return FALSE;
    }

    Params = READ_REGISTER_ULONG((PULONG)(EhciDebug.Capabilities + EHCI_HCSPARAMS));
    Port = EHCI_HCSPARAMS_DEBUG(Params);
    KdUsbTrace("EHCI HCSPARAMS 0x%08lx, debug port %lu, registers at +0x%lx\n",
               Params, Port, (ULONG)(EhciDebug.Registers - EhciDebug.Capabilities));

    if (Params == EHCI_GONE || Port == 0 || Port > EHCI_HCSPARAMS_PORTS(Params))
    {
        EhciDebug.Registers = NULL;
        return FALSE;
    }

    EhciDebug.PortStatus = EHCI_PORTSC(Port);
    return TRUE;
}

/* BACKEND ROUTINES ***********************************************************/

/**
 * @brief
 * Claims the EHCI controller and brings up the debug port.
 *
 * At boot the HAL finds the controller. Later calls reuse what was found.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL when re-initializing.
 *
 * @return
 * TRUE when the debug port is running.
 */
static
BOOLEAN
NTAPI
EhciInitialize(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;

    if (LoaderBlock != NULL)
    {
        Status = KdSetupPciDeviceForDebugging(LoaderBlock, &KdUsbDevice);
        KdUsbTrace("EHCI setup 0x%lx: %lu:0x%lx %04x:%04x class %02x/%02x/%02x\n",
                   Status, KdUsbDevice.Bus, KdUsbDevice.Slot,
                   KdUsbDevice.VendorID, KdUsbDevice.DeviceID,
                   KdUsbDevice.BaseClass, KdUsbDevice.SubClass, KdUsbDevice.ProgIf);
        if (!NT_SUCCESS(Status))
            return FALSE;

        if (!EhciLocateController())
            return FALSE;
    }
    else if (EhciDebug.Registers == NULL)
    {
        return FALSE;
    }

    EhciDebug.Received = 0;
    EhciDebug.TransferReady = FALSE;
    EhciDebug.Discarding = FALSE;
    EhciDebug.NeedsAttach = FALSE;

    if (!EhciStartController())
    {
        KdUsbTrace("EHCI controller did not start\n");
        return FALSE;
    }

    return EhciAttachAtBoot();
}

/**
 * @brief
 * Brings the link back first if an earlier transfer broke it.
 *
 * @return
 * TRUE when the link is usable.
 */
static
BOOLEAN
NTAPI
EhciCheckLink(VOID)
{
    if (!EhciDebug.NeedsAttach)
        return TRUE;

    if (!EhciReattach())
        return FALSE;

    EhciDebug.NeedsAttach = FALSE;
    return TRUE;
}

/**
 * @brief
 * Sends a set of buffers to the host as one transfer.
 *
 * The data goes out in EHCI_PACKET_SIZE packets, and a transfer that fills
 * its last packet is ended with an empty one.
 *
 * @param[in] Buffers
 * The pieces of the transfer, in order.
 *
 * @param[in] Count
 * How many pieces there are.
 */
static
VOID
NTAPI
EhciSendBuffers(
    _In_reads_(Count) PKDUSB_BUFFER Buffers,
    _In_ ULONG Count)
{
    EHCI_RESULT Result = EhciSuccess;
    UCHAR Packet[EHCI_PACKET_SIZE];
    PUCHAR Data;
    ULONG Fill = 0;
    ULONG Total = 0;
    ULONG Index;
    ULONG Offset;

    if (!EhciCheckLink())
        return;

    for (Index = 0; Index < Count && Result == EhciSuccess; Index++)
    {
        Data = Buffers[Index].Data;

        for (Offset = 0; Offset < Buffers[Index].Length && Result == EhciSuccess; Offset++)
        {
            Packet[Fill++] = Data[Offset];
            Total++;

            if (Fill == sizeof(Packet))
            {
                Result = EhciSendPacket(Packet, Fill);
                Fill = 0;
            }
        }
    }

    /* The remainder, or an empty packet when the data filled the last one */
    if (Result == EhciSuccess && Total != 0)
        Result = EhciSendPacket(Packet, Fill);

    if (Result == EhciFailed)
        EhciDebug.NeedsAttach = TRUE;
}

/**
 * @brief
 * Answers the host's "NAME?" query with the TARGETNAME given at boot.
 */
static
VOID
NTAPI
EhciSendTargetName(VOID)
{
    static const CHAR Prefix[] = "NAME=";
    CHAR Reply[sizeof(Prefix) - 1 + KDUSB_TARGET_NAME_MAX];
    KDUSB_BUFFER Buffer;

    RtlCopyMemory(Reply, Prefix, sizeof(Prefix) - 1);
    RtlCopyMemory(Reply + sizeof(Prefix) - 1, KdUsbTargetName, KdUsbTargetNameLength);

    /* Unlike the xHCI path, the EHCI host reads the name without a terminator */
    Buffer.Data = Reply;
    Buffer.Length = sizeof(Prefix) - 1 + KdUsbTargetNameLength;
    EhciSendBuffers(&Buffer, 1);
}

/**
 * @brief
 * Collects one transfer from the host.
 *
 * @param[in] PacketType
 * The packet type wanted. A break-in poll gives up as soon as the device has
 * nothing and no transfer is underway.
 *
 * @param[out] Length
 * Receives the length of the transfer in KdUsbReceiveBuffer.
 *
 * @return
 * EhciSuccess with a transfer, EhciBusy when none arrived, EhciFailed when
 * the link broke.
 */
static
EHCI_RESULT
NTAPI
EhciReceiveTransfer(
    _In_ ULONG PacketType,
    _Out_ PULONG Length)
{
    EHCI_RESULT Result;
    ULONG Attempt;

    for (Attempt = 0; Attempt < EHCI_RECEIVE_ATTEMPTS; Attempt++)
    {
        /* A transfer may already have come in while sending */
        if (!EhciDebug.TransferReady)
        {
            Result = EhciPollReceive();
            if (Result == EhciFailed)
                return Result;

            if (Result == EhciBusy &&
                PacketType == PACKET_TYPE_KD_POLL_BREAKIN &&
                EhciDebug.Received == 0)
            {
                return Result;
            }
        }

        if (EhciDebug.TransferReady)
        {
            *Length = EhciDebug.Received;
            EhciDebug.Received = 0;
            EhciDebug.TransferReady = FALSE;
            return EhciSuccess;
        }
    }

    return EhciBusy;
}

/**
 * @brief
 * Reads a transfer from the host.
 *
 * @param[in] PacketType
 * The packet type wanted.
 *
 * @param[out] Packet
 * Receives the KD packet header.
 *
 * @param[in,out] MessageHeader
 * Receives the message header.
 *
 * @param[in,out] MessageData
 * Receives the message data.
 *
 * @return
 * KdPacketReceived, KdPacketTimedOut or KdPacketNeedsResend.
 */
static
KDSTATUS
NTAPI
EhciReadPacket(
    _In_ ULONG PacketType,
    _Out_ PKD_PACKET Packet,
    _Inout_opt_ PSTRING MessageHeader,
    _Inout_opt_ PSTRING MessageData)
{
    EHCI_RESULT Result;
    ULONG Length;

    if (!EhciCheckLink())
        return KdPacketTimedOut;

    for (;;)
    {
        Result = EhciReceiveTransfer(PacketType, &Length);
        if (Result == EhciFailed)
        {
            EhciDebug.NeedsAttach = TRUE;
            return KdPacketNeedsResend;
        }

        if (Result == EhciBusy)
            return KdPacketTimedOut;

        if (!KdUsbIsNameQuery(Length))
            break;

        EhciSendTargetName();
    }

    return KdUsbCompletePacket(PacketType, Length, Packet, MessageHeader, MessageData);
}

/**
 * @brief
 * Stops the controller.
 */
static
VOID
NTAPI
EhciShutdown(VOID)
{
    EhciHalt();
}

const KDUSB_CONTROLLER KdUsbEhciController =
{
    EhciInitialize,
    EhciReadPacket,
    EhciSendBuffers,
    EhciShutdown
};
