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

#define DBGP_MAKE_PIDS(Token, Send) ((Token) | ((ULONG)(Send) << 8))
#define DBGP_RECEIVED_PID(x)        (((x) >> 16) & 0xFF)
#define DBGP_TARGET(Address, Ep)    (((ULONG)((Address) & 0x7F) << 8) | ((Ep) & 0xF))

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
#define EHCI_REQUEST_GET_DESCRIPTOR 6
#define EHCI_REQUEST_SET_ADDRESS    5
#define EHCI_REQUEST_SET_FEATURE    3
#define EHCI_DEBUG_DESCRIPTOR       10
#define EHCI_FEATURE_DEBUG_MODE     6
#define EHCI_DEBUG_ADDRESS          127

#define EHCI_DEBUG_PORT_BAR0        1

/* Microseconds before a polled register wait gives up */
#define EHCI_CONTROLLER_TIMEOUT     2000000
#define EHCI_TRANSACTION_TIMEOUT    500000

#define EHCI_GONE                   0xFFFFFFFF

/** Result of one debug port transaction. */
typedef enum _EHCI_RESULT
{
    EhciDone = 0,
    EhciNak = 1,
    EhciFailed = 2
} EHCI_RESULT;

#include <pshpack1.h>
typedef struct _EHCI_SETUP
{
    UCHAR RequestType;
    UCHAR Request;
    USHORT Value;
    USHORT Index;
    USHORT Length;
} EHCI_SETUP, *PEHCI_SETUP;
#include <poppack.h>

/** Debug port location for controllers without the PCI capability. */
typedef struct _EHCI_KNOWN_PORT
{
    USHORT VendorId;
    USHORT DeviceId;
    USHORT Offset;
} EHCI_KNOWN_PORT;

static const EHCI_KNOWN_PORT EhciKnownPorts[] =
{
    { 0x8086, 0x24CD, 0x80 },
    { 0x8086, 0x24DD, 0xA0 },
    { 0x8086, 0x265C, 0xA0 },
    { 0x10DE, 0x00D8, 0x80 },
};

typedef struct _EHCI_DEBUG
{
    PUCHAR Capabilities;
    PUCHAR Operational;
    PUCHAR Debug;
    ULONG Port;
    UCHAR Address;
    UCHAR InEndpoint;
    UCHAR OutEndpoint;
    UCHAR InPid;
    UCHAR OutPid;
    ULONG ReceiveOffset;
    BOOLEAN ReceiveComplete;
    BOOLEAN Reinitialize;
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
    return READ_REGISTER_ULONG((PULONG)(EhciDebug.Debug + Offset));
}

static
VOID
NTAPI
EhciWritePort(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(EhciDebug.Debug + Offset), Value);
    KeMemoryBarrier();
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
    ULONG Command;

    Start = EhciReadOp(EHCI_FRINDEX);
    if (Start == EHCI_GONE)
        return FALSE;

    Start &= EHCI_FRINDEX_MASK;

    for (;;)
    {
        Command = EhciReadOp(EHCI_USBCMD);
        if (Command == EHCI_GONE || !(Command & EHCI_USBCMD_RUN))
            return FALSE;

        Now = EhciReadOp(EHCI_FRINDEX);
        if (Now == EHCI_GONE)
            return FALSE;

        /* FRINDEX counts microframes, eight to a frame */
        if (((Now - Start) & EHCI_FRINDEX_MASK) > Frames * 8)
            return TRUE;
    }
}

/**
 * @brief
 * Polls an operational register until a bit reaches the wanted state.
 *
 * @param[in] Offset
 * The operational register.
 *
 * @param[in] Mask
 * The bit to watch.
 *
 * @param[in] Set
 * TRUE to wait for the bit to be set, FALSE for it to clear.
 *
 * @param[out] Value
 * Receives the last value read.
 *
 * @return
 * TRUE when the bit got there within two seconds.
 */
static
BOOLEAN
NTAPI
EhciWaitForBit(
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ BOOLEAN Set,
    _Out_ PULONG Value)
{
    ULONG Elapsed;

    for (Elapsed = 0; ; Elapsed += 5)
    {
        *Value = EhciReadOp(Offset);
        if (*Value == EHCI_GONE)
            return FALSE;

        if (!!(*Value & Mask) == Set)
            return TRUE;

        if (Elapsed > EHCI_CONTROLLER_TIMEOUT)
            return FALSE;

        KdUsbStall(5);
    }
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
    ULONG Status;
    ULONG Command;

    Status = EhciReadOp(EHCI_USBSTS);
    if (Status == EHCI_GONE)
        return FALSE;

    if (Status & EHCI_USBSTS_HALTED)
        return TRUE;

    Command = EhciReadOp(EHCI_USBCMD);
    if (Command == EHCI_GONE)
        return FALSE;

    EhciWriteOp(EHCI_USBCMD, Command & ~EHCI_USBCMD_RUN);
    return EhciWaitForBit(EHCI_USBSTS, EHCI_USBSTS_HALTED, TRUE, &Status);
}

/**
 * @brief
 * Starts a debug port transaction and waits for it to finish.
 *
 * @param[in] Control
 * The control register value, with GO already set.
 *
 * @param[in] Bounded
 * TRUE to give up after EHCI_TRANSACTION_TIMEOUT.
 *
 * @param[out] Result
 * Receives the control register once the transaction is done.
 *
 * @param[out] TimedOut
 * Receives TRUE when the wait gave up.
 *
 * @return
 * TRUE when the transaction finished.
 */
static
BOOLEAN
NTAPI
EhciRunTransaction(
    _In_ ULONG Control,
    _In_ BOOLEAN Bounded,
    _Out_ PULONG Result,
    _Out_ PBOOLEAN TimedOut)
{
    ULONG Elapsed;

    *TimedOut = FALSE;
    EhciWritePort(DBGP_CONTROL, Control);

    for (Elapsed = 0; ; Elapsed += 5)
    {
        *Result = EhciReadPort(DBGP_CONTROL);
        if (*Result == EHCI_GONE)
            return FALSE;

        if (*Result & DBGP_DONE)
            break;

        if (Bounded)
        {
            if (Elapsed > EHCI_TRANSACTION_TIMEOUT)
            {
                *TimedOut = TRUE;
                return FALSE;
            }

            KdUsbStall(5);
        }
    }

    /* Writing DONE back clears it */
    EhciWritePort(DBGP_CONTROL, *Result);
    return TRUE;
}

/* CONTROL TRANSFERS **********************************************************/

/**
 * @brief
 * Sends the SETUP stage of a control transfer.
 *
 * @param[in] Address
 * The device address.
 *
 * @param[in] Setup
 * The setup packet.
 *
 * @param[out] Control
 * Receives the control register after the stage.
 *
 * @return
 * TRUE when the stage ran.
 */
static
BOOLEAN
NTAPI
EhciSetupStage(
    _In_ UCHAR Address,
    _In_ PEHCI_SETUP Setup,
    _Out_ PULONG Control)
{
    ULONG Data[2];
    BOOLEAN TimedOut;

    RtlCopyMemory(Data, Setup, sizeof(Data));
    EhciWritePort(DBGP_DATA_LOW, Data[0]);
    EhciWritePort(DBGP_DATA_HIGH, Data[1]);
    EhciWritePort(DBGP_PIDS, DBGP_MAKE_PIDS(USB_PID_SETUP, USB_PID_DATA0));
    EhciWritePort(DBGP_ADDRESS, DBGP_TARGET(Address, 0));

    *Control = EhciReadPort(DBGP_CONTROL);
    if (*Control == EHCI_GONE)
        return FALSE;

    *Control = (*Control & ~(DBGP_DONE | DBGP_LENGTH_MASK)) | sizeof(*Setup) | DBGP_WRITE | DBGP_GO;
    return EhciRunTransaction(*Control, FALSE, Control, &TimedOut);
}

/**
 * @brief
 * Runs an IN stage until the device answers with DATA1.
 *
 * @param[in,out] Control
 * The control register; receives its value after the stage.
 *
 * @return
 * TRUE when DATA1 came back, FALSE on a stall, three errors or a thousand
 * other answers.
 */
static
BOOLEAN
NTAPI
EhciInStage(
    _Inout_ PULONG Control)
{
    ULONG Errors = 0;
    ULONG Tries = 0;
    ULONG Pids;
    UCHAR Answer;
    BOOLEAN TimedOut;

    while (Errors < 3)
    {
        EhciWritePort(DBGP_PIDS, DBGP_MAKE_PIDS(USB_PID_IN, 0));
        if (!EhciRunTransaction((*Control & ~(DBGP_DONE | DBGP_WRITE)) | DBGP_GO,
                                FALSE,
                                Control,
                                &TimedOut))
        {
            return FALSE;
        }

        if (*Control & DBGP_ERROR)
        {
            Errors++;
        }
        else
        {
            Pids = EhciReadPort(DBGP_PIDS);
            if (Pids == EHCI_GONE)
                return FALSE;

            Answer = (UCHAR)DBGP_RECEIVED_PID(Pids);
            if (Answer == USB_PID_DATA1)
                return TRUE;

            if (Answer == USB_PID_STALL)
                return FALSE;
        }

        if (++Tries >= 1000)
            return FALSE;
    }

    return FALSE;
}

/**
 * @brief
 * Runs a control transfer with no data stage.
 *
 * @param[in] Address
 * The device address.
 *
 * @param[in] Setup
 * The setup packet.
 *
 * @return
 * TRUE when the device accepted the request.
 */
static
BOOLEAN
NTAPI
EhciControlWrite(
    _In_ UCHAR Address,
    _In_ PEHCI_SETUP Setup)
{
    ULONG Control;

    if (!EhciSetupStage(Address, Setup, &Control))
        return FALSE;

    if (!EhciInStage(&Control))
        return FALSE;

    Control = EhciReadPort(DBGP_CONTROL);
    if (Control == EHCI_GONE)
        return FALSE;

    return (Control & DBGP_LENGTH_MASK) == 0;
}

/**
 * @brief
 * Runs a control transfer that reads up to eight bytes.
 *
 * @param[in] Address
 * The device address.
 *
 * @param[in] Setup
 * The setup packet.
 *
 * @param[out] Buffer
 * Receives the data.
 *
 * @param[in,out] Length
 * The buffer size; receives how many bytes came back.
 *
 * @return
 * TRUE when the data stage completed.
 */
static
BOOLEAN
NTAPI
EhciControlRead(
    _In_ UCHAR Address,
    _In_ PEHCI_SETUP Setup,
    _Out_writes_bytes_(*Length) PUCHAR Buffer,
    _Inout_ PULONG Length)
{
    ULONG Data[2];
    ULONG Control;
    ULONG Pids;
    ULONG Errors = 0;
    ULONG Tries = 0;
    UCHAR Answer;
    BOOLEAN TimedOut;

    if (!EhciSetupStage(Address, Setup, &Control))
        return FALSE;

    if (!EhciInStage(&Control))
        return FALSE;

    Control = EhciReadPort(DBGP_CONTROL);
    if (Control == EHCI_GONE)
        return FALSE;

    *Length = min(*Length, Control & DBGP_LENGTH_MASK);
    Data[0] = EhciReadPort(DBGP_DATA_LOW);
    Data[1] = EhciReadPort(DBGP_DATA_HIGH);
    RtlCopyMemory(Buffer, Data, *Length);

    /* Status stage: a zero length OUT, which the device may never answer */
    while (Errors < 3)
    {
        EhciWritePort(DBGP_PIDS, DBGP_MAKE_PIDS(USB_PID_OUT, USB_PID_DATA1));
        if (!EhciRunTransaction((Control & ~(DBGP_DONE | DBGP_LENGTH_MASK)) | DBGP_WRITE | DBGP_GO,
                                FALSE,
                                &Control,
                                &TimedOut))
        {
            return FALSE;
        }

        if (Control & DBGP_ERROR)
        {
            Errors++;
        }
        else
        {
            Pids = EhciReadPort(DBGP_PIDS);
            if (Pids == EHCI_GONE)
                return FALSE;

            Answer = (UCHAR)DBGP_RECEIVED_PID(Pids);
            if (Answer == USB_PID_ACK || Answer == USB_PID_NYET)
                return TRUE;
        }

        if (++Tries >= 1000)
            break;
    }

    return TRUE;
}

/* BULK TRANSFERS *************************************************************/

/**
 * @brief
 * Flips a data toggle between DATA0 and DATA1.
 *
 * @param[in,out] Pid
 * The toggle to flip.
 */
static
VOID
NTAPI
EhciFlipToggle(
    _Inout_ PUCHAR Pid)
{
    *Pid = (*Pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;
}

static
BOOLEAN
NTAPI
EhciReinitializeDevice(VOID);

/**
 * @brief
 * Reads one packet of up to eight bytes from the debug device.
 *
 * @param[out] Buffer
 * Receives the data.
 *
 * @param[out] Length
 * Receives how many bytes came back.
 *
 * @return
 * EhciDone with data, EhciNak when the device had none, EhciFailed when the
 * link is broken.
 */
static
EHCI_RESULT
NTAPI
EhciReadChunk(
    _Out_writes_bytes_(8) PUCHAR Buffer,
    _Out_ PULONG Length)
{
    ULONG Data[2];
    ULONG Control;
    ULONG Pids;
    ULONG Errors;
    UCHAR Answer;
    BOOLEAN TimedOut;

    *Length = 0;
    EhciWritePort(DBGP_ADDRESS, DBGP_TARGET(EhciDebug.Address, EhciDebug.InEndpoint));

    for (Errors = 0; Errors < 3; Errors++)
    {
        EhciWritePort(DBGP_PIDS, DBGP_MAKE_PIDS(USB_PID_IN, 0));

        Control = EhciReadPort(DBGP_CONTROL);
        if (Control == EHCI_GONE)
            return EhciFailed;

        if (!EhciRunTransaction((Control & ~(DBGP_DONE | DBGP_WRITE)) | DBGP_GO,
                                TRUE,
                                &Control,
                                &TimedOut))
        {
            if (TimedOut)
                EhciReinitializeDevice();

            return EhciFailed;
        }

        if (Control & DBGP_ERROR)
            continue;

        Pids = EhciReadPort(DBGP_PIDS);
        if (Pids == EHCI_GONE)
            return EhciFailed;

        Answer = (UCHAR)DBGP_RECEIVED_PID(Pids);
        if (Answer == USB_PID_NAK)
            return EhciNak;

        if (Answer == USB_PID_STALL)
            return EhciFailed;

        /* A repeated packet carries the old toggle; read it again */
        if (Answer != EhciDebug.InPid)
            continue;

        EhciFlipToggle(&EhciDebug.InPid);

        Control = EhciReadPort(DBGP_CONTROL);
        if (Control == EHCI_GONE)
            return EhciFailed;

        *Length = Control & DBGP_LENGTH_MASK;
        if (*Length != 0)
        {
            Data[0] = EhciReadPort(DBGP_DATA_LOW);
            Data[1] = EhciReadPort(DBGP_DATA_HIGH);
            RtlCopyMemory(Buffer, Data, *Length);
        }

        return EhciDone;
    }

    return EhciFailed;
}

/**
 * @brief
 * Sends one packet of up to eight bytes to the debug device.
 *
 * @param[in] Buffer
 * The data.
 *
 * @param[in] Length
 * How many bytes to send.
 *
 * @return
 * EhciDone when acknowledged, EhciNak when the device was busy, EhciFailed
 * when the link is broken.
 */
static
EHCI_RESULT
NTAPI
EhciSendChunk(
    _In_reads_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    ULONG Data[2] = { 0, 0 };
    ULONG Control;
    ULONG Pids;
    ULONG Errors;
    UCHAR Answer;
    BOOLEAN TimedOut;

    RtlCopyMemory(Data, Buffer, Length);
    EhciWritePort(DBGP_ADDRESS, DBGP_TARGET(EhciDebug.Address, EhciDebug.OutEndpoint));

    for (Errors = 0; Errors < 3; Errors++)
    {
        EhciWritePort(DBGP_DATA_LOW, Data[0]);
        EhciWritePort(DBGP_DATA_HIGH, Data[1]);
        EhciWritePort(DBGP_PIDS, DBGP_MAKE_PIDS(USB_PID_OUT, EhciDebug.OutPid));

        Control = EhciReadPort(DBGP_CONTROL);
        if (Control == EHCI_GONE)
            return EhciFailed;

        Control = (Control & ~(DBGP_DONE | DBGP_LENGTH_MASK)) | Length | DBGP_WRITE | DBGP_GO;
        if (!EhciRunTransaction(Control, TRUE, &Control, &TimedOut))
            return EhciFailed;

        if (Control & DBGP_ERROR)
            continue;

        Pids = EhciReadPort(DBGP_PIDS);
        if (Pids == EHCI_GONE)
            return EhciFailed;

        Answer = (UCHAR)DBGP_RECEIVED_PID(Pids);
        if (Answer == USB_PID_NAK)
            return EhciNak;

        if (Answer == USB_PID_STALL)
            return EhciFailed;

        if (Answer == USB_PID_ACK || Answer == USB_PID_NYET)
        {
            EhciFlipToggle(&EhciDebug.OutPid);
            return EhciDone;
        }
    }

    return EhciFailed;
}

/**
 * @brief
 * Sends one packet, pulling in host data while the device is busy.
 *
 * The device cannot take more from the target while its own buffer to the
 * target is full, so whatever the host sent is read into the receive buffer
 * between attempts and handed out by the next read.
 *
 * @param[in] Buffer
 * The data.
 *
 * @param[in] Length
 * How many bytes to send.
 *
 * @return
 * The result of the last send attempt.
 */
static
EHCI_RESULT
NTAPI
EhciSendChunkDraining(
    _In_reads_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    EHCI_RESULT Result = EhciFailed;
    ULONG Received;
    ULONG Try;

    for (Try = 0; Try < 100000; Try++)
    {
        if (!EhciDebug.ReceiveComplete &&
            EhciDebug.ReceiveOffset + 8 <= KDUSB_TRANSFER_SIZE &&
            EhciReadChunk(KdUsbReceiveBuffer + EhciDebug.ReceiveOffset, &Received) == EhciDone)
        {
            EhciDebug.ReceiveOffset += Received;
            if (Received < 8)
                EhciDebug.ReceiveComplete = TRUE;
        }

        Result = EhciSendChunk(Buffer, Length);
        if (Result != EhciNak)
            break;
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
    EHCI_SETUP Setup;
    UCHAR Descriptor[4];
    ULONG Length;
    ULONG Try;

    EhciDebug.InPid = USB_PID_DATA0;
    EhciDebug.OutPid = USB_PID_DATA0;

    Setup.RequestType = 0x80;
    Setup.Request = EHCI_REQUEST_GET_DESCRIPTOR;
    Setup.Value = EHCI_DEBUG_DESCRIPTOR << 8;
    Setup.Index = 0;
    Setup.Length = sizeof(Descriptor);

    for (Try = 0; ; )
    {
        Length = sizeof(Descriptor);
        if (EhciControlRead(0, &Setup, Descriptor, &Length) && Length == sizeof(Descriptor))
            break;

        if (++Try >= 3)
            return FALSE;
    }

    EhciDebug.InEndpoint = Descriptor[2];
    EhciDebug.OutEndpoint = Descriptor[3];
    EhciDebug.Address = EHCI_DEBUG_ADDRESS;

    Setup.RequestType = 0;
    Setup.Request = EHCI_REQUEST_SET_ADDRESS;
    Setup.Value = EHCI_DEBUG_ADDRESS;
    Setup.Length = 0;
    if (!EhciControlWrite(0, &Setup))
        return FALSE;

    Setup.Request = EHCI_REQUEST_SET_FEATURE;
    Setup.Value = EHCI_FEATURE_DEBUG_MODE;
    return EhciControlWrite(EhciDebug.Address, &Setup);
}

/**
 * @brief
 * Resets the debug port and brings up the device behind it.
 *
 * @param[in] HaltAfter
 * TRUE to stop the controller once the port is reset, for when the
 * controller was started only for this.
 *
 * @param[out] Connected
 * Receives whether a device was attached.
 *
 * @return
 * TRUE when the debug device is ready.
 */
static
BOOLEAN
NTAPI
EhciTryInitializeDevice(
    _In_ BOOLEAN HaltAfter,
    _Out_ PBOOLEAN Connected)
{
    ULONG PortStatus = EHCI_PORTSC(EhciDebug.Port);
    ULONG Port;
    ULONG Control;

    Port = EhciReadOp(PortStatus);
    if (Port == EHCI_GONE)
        return FALSE;

    if (!(Port & EHCI_PORTSC_CCS))
    {
        *Connected = FALSE;
        if (HaltAfter)
            EhciHalt();

        return FALSE;
    }

    *Connected = TRUE;

    EhciWriteOp(PortStatus, (Port & ~EHCI_PORTSC_PED) | EHCI_PORTSC_PR);
    if (!EhciWaitFrames(20))
        return FALSE;

    Port = EhciReadOp(PortStatus);
    if (Port == EHCI_GONE)
        return FALSE;

    EhciWriteOp(PortStatus, Port & ~EHCI_PORTSC_PR);
    if (!EhciWaitFrames(5))
        return FALSE;

    Port = EhciReadOp(PortStatus);
    if (Port == EHCI_GONE)
        return FALSE;

    /* Hand the enabled port from the host controller to the debug port */
    if ((Port & EHCI_PORTSC_CCS) && (Port & EHCI_PORTSC_PED))
    {
        Control = EhciReadPort(DBGP_CONTROL);
        if (Control == EHCI_GONE)
            return FALSE;

        EhciWritePort(DBGP_CONTROL, Control | DBGP_ENABLED);

        Port = EhciReadOp(PortStatus);
        if (Port == EHCI_GONE)
            return FALSE;

        EhciWriteOp(PortStatus, Port & ~EHCI_PORTSC_PED);
    }

    if (HaltAfter && !EhciHalt())
        return FALSE;

    Control = EhciReadPort(DBGP_CONTROL);
    if (Control == EHCI_GONE || !(Control & DBGP_ENABLED))
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
 * When the EHCI driver has stopped the controller, it is run just long enough
 * to reset the port and then put back the way it was found.
 *
 * @return
 * TRUE when the debug device is ready again.
 */
static
BOOLEAN
NTAPI
EhciReinitializeDevice(VOID)
{
    ULONG Command;
    ULONG Control;
    BOOLEAN Connected;
    BOOLEAN Ready;

    Command = EhciReadOp(EHCI_USBCMD);
    if (Command == EHCI_GONE)
        return FALSE;

    if (!(Command & EHCI_USBCMD_RUN))
    {
        EhciWriteOp(EHCI_USBCMD, (Command & ~EHCI_USBCMD_SCHEDULES) | EHCI_USBCMD_RUN);
        Ready = EhciTryInitializeDevice(TRUE, &Connected);
        EhciWriteOp(EHCI_USBCMD, Command);
        return Ready;
    }

    Control = EhciReadPort(DBGP_CONTROL);
    if (Control == EHCI_GONE)
        return FALSE;

    Control &= ~(DBGP_OWNER | DBGP_ENABLED);
    EhciWritePort(DBGP_CONTROL, Control);
    EhciWritePort(DBGP_CONTROL, Control | DBGP_OWNER);

    if (!(EhciReadOp(EHCI_PORTSC(EhciDebug.Port)) & EHCI_PORTSC_CCS))
        return FALSE;

    return EhciTryInitializeDevice(FALSE, &Connected);
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
EhciInitializeDevice(VOID)
{
    BOOLEAN Connected = FALSE;
    BOOLEAN Ready = FALSE;
    ULONG Try;

    for (Try = 0; Try < 20; Try++)
    {
        Ready = EhciTryInitializeDevice(FALSE, &Connected);
        if (Ready)
            break;

        if (!EhciWaitFrames(500))
            return FALSE;

        if (!Connected && Try >= 5)
            break;
    }

    KdUsbTrace("EHCI device %s after %lu tries, cable %s\n",
               Ready ? "ready" : "not ready", Try + 1, Connected ? "seen" : "not seen");

    if (!EhciHalt())
        return FALSE;

    if (!Ready)
    {
        KdUsbSetHostPresent(FALSE);
        EhciDebug.Reinitialize = TRUE;
    }

    return TRUE;
}

/**
 * @brief
 * Resets the controller, runs it, and claims the debug port.
 *
 * @return
 * TRUE when the controller is running with the debug port owned.
 */
static
BOOLEAN
NTAPI
EhciStartController(VOID)
{
    ULONG Command;
    ULONG Status;
    ULONG Control;
    ULONG Params;

    if (!EhciHalt())
        return FALSE;

    Command = EhciReadOp(EHCI_USBCMD);
    if (Command == EHCI_GONE)
        return FALSE;

    EhciWriteOp(EHCI_USBCMD, Command | EHCI_USBCMD_HCRESET);
    if (!EhciWaitForBit(EHCI_USBCMD, EHCI_USBCMD_HCRESET, FALSE, &Command))
        return FALSE;

    if (Command & EHCI_USBCMD_SCHEDULES)
        return FALSE;

    EhciWriteOp(EHCI_USBCMD, Command | EHCI_USBCMD_RUN);
    if (!EhciWaitForBit(EHCI_USBSTS, EHCI_USBSTS_HALTED, FALSE, &Status))
        return FALSE;

    Control = EhciReadPort(DBGP_CONTROL);
    if (Control == EHCI_GONE)
        return FALSE;

    EhciWritePort(DBGP_CONTROL, Control | DBGP_OWNER | DBGP_IN_USE);
    if (!EhciWaitFrames(20))
        return FALSE;

    Params = READ_REGISTER_ULONG((PULONG)(EhciDebug.Capabilities + EHCI_HCSPARAMS));
    if (Params == EHCI_GONE)
        return FALSE;

    if (!(Params & EHCI_HCSPARAMS_PPC))
        return TRUE;

    EhciWriteOp(EHCI_PORTSC(EhciDebug.Port),
                EhciReadOp(EHCI_PORTSC(EhciDebug.Port)) | EHCI_PORTSC_PP);
    return EhciWaitFrames(20);
}

/**
 * @brief
 * Finds the debug port registers inside the controller's register BAR.
 *
 * The PCI Debug Port capability says where they are. Without the HAL's PCI
 * config routines, a table of known controllers is used instead.
 *
 * @return
 * The debug port registers, or NULL when they cannot be found.
 */
static
PUCHAR
NTAPI
EhciLocateDebugPort(VOID)
{
    PCI_COMMON_CONFIG Config;
    PUCHAR Bytes = (PUCHAR)&Config;
    ULONG Capability;
    ULONG Index;
    UCHAR Next;

    if (KdGetPciDataByOffset == NULL)
    {
        for (Index = 0; Index < RTL_NUMBER_OF(EhciKnownPorts); Index++)
        {
            if (EhciKnownPorts[Index].VendorId == KdUsbDevice.VendorID &&
                EhciKnownPorts[Index].DeviceId == KdUsbDevice.DeviceID)
            {
                return EhciDebug.Capabilities + EhciKnownPorts[Index].Offset;
            }
        }

        return NULL;
    }

    if (KdGetPciDataByOffset(KdUsbDevice.Bus, KdUsbDevice.Slot, &Config, 0, sizeof(Config)) !=
        sizeof(Config))
    {
        return NULL;
    }

    if (!(Config.Status & PCI_STATUS_CAPABILITIES_LIST))
        return NULL;

    /* Bounded so a looping capability list cannot hang the boot */
    Next = Config.u.type0.CapabilitiesPtr & 0xFC;
    for (Index = 0; Next != 0 && Index < 48; Index++)
    {
        if (Bytes[Next] == PCI_CAPABILITY_ID_DEBUG_PORT)
        {
            RtlCopyMemory(&Capability, &Bytes[Next], sizeof(Capability));
            if ((Capability >> 29) != EHCI_DEBUG_PORT_BAR0)
                return NULL;

            return EhciDebug.Capabilities + ((Capability >> 16) & 0xFFF);
        }

        Next = Bytes[Next + 1] & 0xFC;
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
    ULONG Capability;
    ULONG Params;
    ULONG Port;

    if (KdUsbDevice.BaseClass != PCI_CLASS_SERIAL_BUS_CTLR ||
        KdUsbDevice.SubClass != PCI_SUBCLASS_SB_USB ||
        KdUsbDevice.ProgIf != KDUSB_PROGIF_EHCI ||
        Bar->Type != CmResourceTypeMemory ||
        !Bar->Valid)
    {
        return FALSE;
    }

    EhciDebug.Capabilities = Bar->TranslatedAddress;

    Capability = READ_REGISTER_ULONG((PULONG)EhciDebug.Capabilities);
    if (Capability == EHCI_GONE)
        return FALSE;

    EhciDebug.Operational = EhciDebug.Capabilities + (Capability & 0xFF);
    EhciDebug.Debug = EhciLocateDebugPort();
    if (EhciDebug.Debug == NULL)
    {
        KdUsbTrace("no EHCI debug port registers\n");
        return FALSE;
    }

    Params = READ_REGISTER_ULONG((PULONG)(EhciDebug.Capabilities + EHCI_HCSPARAMS));
    if (Params == EHCI_GONE)
        return FALSE;

    Port = EHCI_HCSPARAMS_DEBUG(Params);
    KdUsbTrace("EHCI HCSPARAMS 0x%08lx, debug port %lu, registers at +0x%lx\n",
               Params, Port, (ULONG)(EhciDebug.Debug - EhciDebug.Capabilities));
    if (Port == 0 || Port > EHCI_HCSPARAMS_PORTS(Params))
        return FALSE;

    EhciDebug.Port = Port;
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
    else if (EhciDebug.Debug == NULL)
    {
        return FALSE;
    }

    if (!EhciStartController())
    {
        KdUsbTrace("EHCI controller did not start\n");
        return FALSE;
    }

    return EhciInitializeDevice();
}

/**
 * @brief
 * Collects one transfer from the host.
 *
 * A transfer ends with a packet shorter than eight bytes.
 *
 * @param[in] PacketType
 * The packet type wanted. A break-in poll gives up as soon as the device has
 * nothing.
 *
 * @param[out] Length
 * Receives the length of the transfer in KdUsbReceiveBuffer.
 *
 * @return
 * EhciDone with a transfer, EhciNak when none arrived, EhciFailed when the
 * link broke.
 */
static
EHCI_RESULT
NTAPI
EhciReceiveTransfer(
    _In_ ULONG PacketType,
    _Out_ PULONG Length)
{
    EHCI_RESULT Result = EhciNak;
    ULONG Received;
    ULONG Total;
    ULONG Try;

    *Length = 0;

    for (Try = 0; Try < 100000; Try++)
    {
        /* A transfer may already have come in while sending */
        if (EhciDebug.ReceiveComplete)
        {
            *Length = EhciDebug.ReceiveOffset;
            EhciDebug.ReceiveOffset = 0;
            EhciDebug.ReceiveComplete = FALSE;
            return EhciDone;
        }

        Result = EhciReadChunk(KdUsbReceiveBuffer + EhciDebug.ReceiveOffset, &Received);
        if (Result == EhciFailed)
            return Result;

        if (Result == EhciDone)
        {
            Total = EhciDebug.ReceiveOffset + Received;
            EhciDebug.ReceiveOffset = Total;

            if (Received < 8)
            {
                EhciDebug.ReceiveOffset = 0;
                *Length = Total;
                return EhciDone;
            }

            /* Too long to be a KD packet; start over */
            if (Total == KDUSB_TRANSFER_SIZE)
                EhciDebug.ReceiveOffset = 0;
        }
        else if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN && EhciDebug.ReceiveOffset == 0)
        {
            return Result;
        }
    }

    return Result;
}

/**
 * @brief
 * Sends a set of buffers to the host in eight byte packets.
 *
 * A transfer that fills its last packet is ended with a zero length one.
 *
 * @param[in] Buffers
 * The pieces of the packet, in order.
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
    EHCI_RESULT Result = EhciDone;
    UCHAR Chunk[8];
    ULONG Index = 0;
    ULONG Offset = 0;
    ULONG Fill;
    ULONG Last = 0;

    if (EhciDebug.Reinitialize)
    {
        if (!EhciReinitializeDevice())
            return;

        EhciDebug.Reinitialize = FALSE;
    }

    if (Count == 0)
        return;

    while (Index < Count)
    {
        for (Fill = 0; Fill < sizeof(Chunk); )
        {
            while (Index < Count && Offset == Buffers[Index].Length)
            {
                Index++;
                Offset = 0;
            }

            if (Index == Count)
                break;

            Chunk[Fill++] = ((PUCHAR)Buffers[Index].Data)[Offset++];
        }

        if (Fill != 0)
        {
            Last = Fill;
            Result = EhciSendChunkDraining(Chunk, Fill);
        }
    }

    if (Last == sizeof(Chunk))
        Result = EhciSendChunkDraining(Chunk, 0);

    if (Result == EhciFailed)
        EhciDebug.Reinitialize = TRUE;
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
    static CHAR Prefix[] = "NAME=";
    KDUSB_BUFFER Reply[2];
    EHCI_RESULT Result;
    ULONG Length;

    if (EhciDebug.Reinitialize)
    {
        if (!EhciReinitializeDevice())
            return KdPacketTimedOut;

        EhciDebug.Reinitialize = FALSE;
    }

    for (;;)
    {
        Result = EhciReceiveTransfer(PacketType, &Length);
        if (Result == EhciFailed)
        {
            EhciDebug.Reinitialize = TRUE;
            return KdPacketNeedsResend;
        }

        if (Result == EhciNak)
            return KdPacketTimedOut;

        if (!KdUsbIsNameQuery(Length))
            break;

        Reply[0].Data = Prefix;
        Reply[0].Length = sizeof(Prefix) - 1;
        Reply[1].Data = KdUsbTargetName;
        Reply[1].Length = KdUsbTargetNameLength;
        EhciSendBuffers(Reply, RTL_NUMBER_OF(Reply));
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
