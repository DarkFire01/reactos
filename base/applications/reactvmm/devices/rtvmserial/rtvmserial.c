/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A 16550A serial port, and somewhere for its bytes to go
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

/* Registers, as offsets from whichever address the port was put at */
#define UART_RBR            0   /* Read: what arrived */
#define UART_THR            0   /* Write: what to send */
#define UART_DLL            0   /* Both, while the divisor is showing */
#define UART_IER            1
#define UART_DLM            1
#define UART_IIR            2   /* Read */
#define UART_FCR            2   /* Write */
#define UART_LCR            3
#define UART_MCR            4
#define UART_LSR            5
#define UART_MSR            6
#define UART_SCR            7

#define UART_REGISTER_COUNT 8

/* Interrupt enable */
#define IER_RECEIVED        0x01
#define IER_SEND_EMPTY      0x02
#define IER_LINE_STATUS     0x04
#define IER_MODEM_STATUS    0x08
#define IER_KNOWN           0x0F

/* Interrupt identification, lowest number is the most pressing */
#define IIR_NONE_PENDING    0x01
#define IIR_MODEM_STATUS    0x00
#define IIR_SEND_EMPTY      0x02
#define IIR_RECEIVED        0x04
#define IIR_LINE_STATUS     0x06
/* A receive interrupt that fired on the timer rather than the trigger level */
#define IIR_RECEIVE_TIMEOUT 0x0C
#define IIR_FIFO_ENABLED    0xC0

/* Queue control */
#define FCR_ENABLE          0x01
#define FCR_CLEAR_RECEIVE   0x02
#define FCR_CLEAR_SEND      0x04
#define FCR_TRIGGER_MASK    0xC0

/* Line control */
#define LCR_DIVISOR_LATCH   0x80
#define LCR_BREAK           0x40

/* Modem control */
#define MCR_DTR             0x01
#define MCR_RTS             0x02
#define MCR_OUT1            0x04
#define MCR_OUT2            0x08
#define MCR_LOOPBACK        0x10

/* Line status */
#define LSR_DATA_READY      0x01
#define LSR_OVERRUN         0x02
#define LSR_PARITY_ERROR    0x04
#define LSR_FRAMING_ERROR   0x08
#define LSR_BREAK           0x10
#define LSR_SEND_EMPTY      0x20
#define LSR_SEND_IDLE       0x40
#define LSR_QUEUE_ERROR     0x80

/* Modem status, the four that say a line moved and the four that say where it is */
#define MSR_DELTA_CTS       0x01
#define MSR_DELTA_DSR       0x02
#define MSR_RING_ENDED      0x04
#define MSR_DELTA_CARRIER   0x08
#define MSR_CTS             0x10
#define MSR_DSR             0x20
#define MSR_RING            0x40
#define MSR_CARRIER         0x80

/* What the real part holds, and so what a driver counting on it expects */
#define UART_QUEUE_SIZE     16

/*
 * How much may be waiting to go out. The wire is far slower than the machine
 * writing to it, so something has to hold the difference; when even this is
 * full the oldest of it goes, because a port whose writes block would stop the
 * processor for as long as whatever is reading took to come back.
 */
#define UART_SEND_SIZE      8192

/* How long to leave a pipe between one look for something plugging in and the next */
#define UART_LISTEN_PAUSE   500

/* Where the two ports a PC has always had live */
#define SERIAL_COM1_PORT    0x03F8
#define SERIAL_COM1_LINE    4
#define SERIAL_COM2_PORT    0x02F8
#define SERIAL_COM2_LINE    3

/* TYPES **********************************************************************/

typedef struct _SERIAL_QUEUE
{
    UCHAR Data[UART_QUEUE_SIZE];
    ULONG Count;
    ULONG Read;
    ULONG Write;
} SERIAL_QUEUE, *PSERIAL_QUEUE;

typedef struct _SERIAL_DEVICE
{
    RTVM_DEVICE Device;

    USHORT BasePort;
    ULONG Line;

    /* The registers that are just kept */
    UCHAR InterruptEnable;
    UCHAR LineControl;
    UCHAR ModemControl;
    UCHAR LineStatus;
    UCHAR ModemStatus;
    UCHAR Scratch;
    UCHAR QueueControl;
    USHORT Divisor;

    /* What has arrived and not been read yet */
    SERIAL_QUEUE Received;

    /* Whether the queues are on, which changes what the identify register says */
    BOOLEAN QueuesEnabled;
    UCHAR ReceiveTrigger;

    /* What is holding the line up, so it is only let go once */
    BOOLEAN LineAsserted;

    /* One lock for everything, because the reader runs on its own thread */
    CRITICAL_SECTION Lock;

    /* Where the bytes go and where they come from */
    HANDLE Output;
    HANDLE Input;
    HANDLE Reader;
    volatile LONG Stopping;

    /*
     * Whether the other end is a pipe waiting to be plugged into, and whether
     * anything is plugged in yet. A port with nothing on the other end still
     * works: the bytes go nowhere, which is what a real one does.
     */
    BOOLEAN Listening;
    volatile LONG Attached;

    /* Whether the machine is held until something plugs in */
    BOOLEAN HoldForAttach;

    /*
     * One place each for a read, a write and a wait for something to plug in.
     * Both directions of a pipe go through the one handle, and two waits on a
     * handle are taken one at a time unless each carries its own: without
     * these, the write saying what the machine is doing would queue behind a
     * read of something nobody has typed yet, which never comes back.
     */
    HANDLE ReadDone;
    HANDLE WriteDone;
    HANDLE AttachDone;

    /*
     * What has come in and not reached the queue the guest reads yet. The
     * queue on a part of this kind is sixteen bytes and whatever is on the
     * other end arrives in far larger lumps than that: without somewhere to
     * hold the rest, everything past the sixteenth byte of every lump is lost,
     * which a debugger on the other end sees as a machine that answers a
     * fraction of what it is asked.
     */
    UCHAR Incoming[UART_SEND_SIZE];
    ULONG InCount;
    ULONG InRead;
    ULONG InWrite;

    /* What is waiting to go out, and the one that takes it there */
    UCHAR Sending[UART_SEND_SIZE];
    ULONG SendCount;
    ULONG SendRead;
    ULONG SendWrite;
    HANDLE Writer;
    HANDLE Ready;

    CHAR Backend[MAX_PATH];
} SERIAL_DEVICE, *PSERIAL_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE SerialVtable;

/* FUNCTIONS ******************************************************************/

static
VOID
SerialQueueReset(
    _Out_ PSERIAL_QUEUE Queue)
{
    RtlZeroMemory(Queue, sizeof(*Queue));
}

static
BOOLEAN
SerialQueuePut(
    _Inout_ PSERIAL_QUEUE Queue,
    _In_ UCHAR Value,
    _In_ ULONG Limit)
{
    if (Queue->Count >= Limit)
        return FALSE;

    Queue->Data[Queue->Write] = Value;
    Queue->Write = (Queue->Write + 1) % UART_QUEUE_SIZE;
    Queue->Count++;
    return TRUE;
}

static
BOOLEAN
SerialQueueTake(
    _Inout_ PSERIAL_QUEUE Queue,
    _Out_ PUCHAR Value)
{
    if (Queue->Count == 0)
        return FALSE;

    *Value = Queue->Data[Queue->Read];
    Queue->Read = (Queue->Read + 1) % UART_QUEUE_SIZE;
    Queue->Count--;
    return TRUE;
}

/**
 * @brief
 * Works out what the port would be asking for, and holds its line to match.
 *
 * @remarks
 * Called with the lock held, after anything that could have changed what is
 * pending. The line is only touched when it would change, because the manager
 * counts holders and a port that asserts twice is never let go.
 *
 * OUT2 is not a modem signal on a PC: it gates the line to the controller, and
 * a port whose driver has not set it is wired to nothing.
 */
static
VOID
SerialUpdateLine(
    _Inout_ PSERIAL_DEVICE Serial)
{
    const RTVM_HOST_INTERFACE *Host = Serial->Device.Host;
    BOOLEAN Pending = FALSE;

    if ((Serial->InterruptEnable & IER_LINE_STATUS) &&
        (Serial->LineStatus & (LSR_OVERRUN | LSR_PARITY_ERROR |
                               LSR_FRAMING_ERROR | LSR_BREAK)))
    {
        Pending = TRUE;
    }

    if ((Serial->InterruptEnable & IER_RECEIVED) && (Serial->Received.Count != 0))
        Pending = TRUE;

    /* Nothing is ever in flight here, so the sender is always empty */
    if (Serial->InterruptEnable & IER_SEND_EMPTY)
        Pending = TRUE;

    if ((Serial->InterruptEnable & IER_MODEM_STATUS) && (Serial->ModemStatus & 0x0F))
        Pending = TRUE;

    if (!(Serial->ModemControl & MCR_OUT2))
        Pending = FALSE;

    if (Pending == Serial->LineAsserted)
        return;

    Serial->LineAsserted = Pending;

    if (Host->SetInterruptLine != NULL)
        Host->SetInterruptLine(Host->Context, Serial->Line, Pending);
}

/**
 * @brief
 * Says which of the pending reasons the port would report first.
 */
static
UCHAR
SerialIdentify(
    _In_ PSERIAL_DEVICE Serial)
{
    UCHAR Value;

    if ((Serial->InterruptEnable & IER_LINE_STATUS) &&
        (Serial->LineStatus & (LSR_OVERRUN | LSR_PARITY_ERROR |
                               LSR_FRAMING_ERROR | LSR_BREAK)))
    {
        Value = IIR_LINE_STATUS;
    }
    else if ((Serial->InterruptEnable & IER_RECEIVED) && (Serial->Received.Count != 0))
    {
        Value = IIR_RECEIVED;
    }
    else if (Serial->InterruptEnable & IER_SEND_EMPTY)
    {
        Value = IIR_SEND_EMPTY;
    }
    else if ((Serial->InterruptEnable & IER_MODEM_STATUS) && (Serial->ModemStatus & 0x0F))
    {
        Value = IIR_MODEM_STATUS;
    }
    else
    {
        Value = IIR_NONE_PENDING;
    }

    if (Serial->QueuesEnabled)
        Value |= IIR_FIFO_ENABLED;

    return Value;
}

/**
 * @brief
 * Takes one byte the guest sent, either out to the backend or straight back.
 *
 * @remarks
 * Loopback is how a driver works out whether there is a port here at all, so
 * it has to be answered before anything is written anywhere.
 */
static
VOID
SerialSend(
    _Inout_ PSERIAL_DEVICE Serial,
    _In_ UCHAR Value)
{
    DWORD Written;

    if (Serial->ModemControl & MCR_LOOPBACK)
    {
        if (!SerialQueuePut(&Serial->Received, Value,
                            Serial->QueuesEnabled ? UART_QUEUE_SIZE : 1))
        {
            Serial->LineStatus |= LSR_OVERRUN;
        }

        Serial->LineStatus |= LSR_DATA_READY;
        return;
    }

    if (Serial->Output == INVALID_HANDLE_VALUE)
        return;

    /* A pipe with nothing plugged into it swallows bytes, as a plug would */
    if (Serial->Listening &&
        (InterlockedCompareExchange(&Serial->Attached, 0, 0) == 0))
    {
        return;
    }

    /* Put where the thread that writes will find it, rather than written here */
    if (Serial->SendCount >= UART_SEND_SIZE)
    {
        Serial->SendRead = (Serial->SendRead + 1) % UART_SEND_SIZE;
        Serial->SendCount--;
    }

    Serial->Sending[Serial->SendWrite] = Value;
    Serial->SendWrite = (Serial->SendWrite + 1) % UART_SEND_SIZE;
    Serial->SendCount++;

    UNREFERENCED_PARAMETER(Written);

    if (Serial->Ready != NULL)
        SetEvent(Serial->Ready);
}

/**
 * @brief
 * Moves what has come in into the queue the guest reads, as far as it fits.
 *
 * @remarks
 * Called both when something arrives and every time the guest takes a byte,
 * because a queue of sixteen empties one byte at a time and the only moment
 * there is room for more is just after one has gone. The lock is held by the
 * caller either way.
 */
static
VOID
SerialTopUp(
    _Inout_ PSERIAL_DEVICE Serial)
{
    const ULONG Limit = Serial->QueuesEnabled ? UART_QUEUE_SIZE : 1;

    while ((Serial->InCount != 0) && (Serial->Received.Count < Limit))
    {
        if (!SerialQueuePut(&Serial->Received, Serial->Incoming[Serial->InRead],
                            Limit))
        {
            break;
        }

        Serial->InRead = (Serial->InRead + 1) % UART_SEND_SIZE;
        Serial->InCount--;
    }

    if (Serial->Received.Count != 0)
        Serial->LineStatus |= LSR_DATA_READY;
}

/**
 * @brief
 * Waits for something started on a pipe to finish.
 *
 * @remarks
 * Checked rather than simply waited on, so that a machine being taken down
 * does not have to wait for a read of something nobody is ever going to type.
 */
static
BOOLEAN
SerialAwait(
    _Inout_ PSERIAL_DEVICE Serial,
    _In_ HANDLE Done,
    _Inout_ LPOVERLAPPED Overlapped,
    _Out_ PDWORD Moved)
{
    *Moved = 0;

    /* Finished on the spot, which a pipe is allowed to do */
    if (GetLastError() != ERROR_IO_PENDING)
        return FALSE;

    while (WaitForSingleObject(Done, 200) != WAIT_OBJECT_0)
    {
        if (InterlockedCompareExchange(&Serial->Stopping, 0, 0) != 0)
            return FALSE;
    }

    return GetOverlappedResult(Serial->Input, Overlapped, Moved, FALSE) != FALSE;
}

/* Waits for something to plug into the pipe. False means it never did */
static
BOOLEAN
SerialAwaitClient(
    _Inout_ PSERIAL_DEVICE Serial)
{
    OVERLAPPED Waiting;
    DWORD Moved;

    RtlZeroMemory(&Waiting, sizeof(Waiting));
    Waiting.hEvent = Serial->AttachDone;

    if (ConnectNamedPipe(Serial->Input, &Waiting))
        return TRUE;

    if (GetLastError() == ERROR_PIPE_CONNECTED)
        return TRUE;

    return SerialAwait(Serial, Serial->AttachDone, &Waiting, &Moved);
}

/**
 * @brief
 * Takes what is waiting out to wherever the port goes.
 *
 * @remarks
 * Not on the thread running the processor. Whatever is on the other end reads
 * when it feels like it, and a machine that stopped until it did would be a
 * machine whose speed was set by whoever was watching it.
 */
static
DWORD
WINAPI
SerialWriter(
    _In_ LPVOID Parameter)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Parameter;
    UCHAR Buffer[256];
    DWORD Written;
    ULONG Count;

    while (InterlockedCompareExchange(&Serial->Stopping, 0, 0) == 0)
    {
        WaitForSingleObject(Serial->Ready, 100);

        for (;;)
        {
            EnterCriticalSection(&Serial->Lock);

            for (Count = 0; (Count < sizeof(Buffer)) && (Serial->SendCount != 0);
                 Count++)
            {
                Buffer[Count] = Serial->Sending[Serial->SendRead];
                Serial->SendRead = (Serial->SendRead + 1) % UART_SEND_SIZE;
                Serial->SendCount--;
            }

            LeaveCriticalSection(&Serial->Lock);

            if (Count == 0)
                break;

            if (Serial->Listening)
            {
                OVERLAPPED Going;

                RtlZeroMemory(&Going, sizeof(Going));
                Going.hEvent = Serial->WriteDone;

                if (!WriteFile(Serial->Output, Buffer, Count, &Written, &Going) &&
                    !SerialAwait(Serial, Serial->WriteDone, &Going, &Written))
                {
                    break;
                }
            }
            else if (!WriteFile(Serial->Output, Buffer, Count, &Written, NULL))
            {
                break;
            }
        }
    }

    return 0;
}

/**
 * @brief
 * Reads the backend for as long as the machine is running.
 *
 * @remarks
 * A port that nobody is typing at blocks here, which is why this is not done
 * on the thread running the processor. What arrives goes in the queue and the
 * line is worked out again, because a byte turning up is a reason to interrupt.
 */
static
DWORD
WINAPI
SerialReader(
    _In_ LPVOID Parameter)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Parameter;
    const RTVM_HOST_INTERFACE *Host = Serial->Device.Host;
    UCHAR Buffer[64];
    DWORD Read;
    DWORD Index;

    while (InterlockedCompareExchange(&Serial->Stopping, 0, 0) == 0)
    {
        /*
         * A pipe answers nothing until something plugs into it, so waiting for
         * that is the first thing done and the thing gone back to whenever
         * whatever was plugged in goes away again.
         */
        if (Serial->Listening &&
            (InterlockedCompareExchange(&Serial->Attached, 0, 0) == 0))
        {
            if (!SerialAwaitClient(Serial))
            {
                if (InterlockedCompareExchange(&Serial->Stopping, 0, 0) != 0)
                    break;

                Sleep(UART_LISTEN_PAUSE);
                continue;
            }

            InterlockedExchange(&Serial->Attached, 1);

            Host->Log(Host->Context, RtvmLogInfo,
                      "%s: something is on the other end now\n",
                      Serial->Device.Name);
        }

        if (Serial->Listening)
        {
            OVERLAPPED Coming;

            RtlZeroMemory(&Coming, sizeof(Coming));
            Coming.hEvent = Serial->ReadDone;

            if (!ReadFile(Serial->Input, Buffer, sizeof(Buffer), &Read, &Coming))
                SerialAwait(Serial, Serial->ReadDone, &Coming, &Read);
        }
        else if (!ReadFile(Serial->Input, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Read = 0;
        }

        if (Read == 0)
        {
            if (!Serial->Listening)
                break;

            /* Unplugged. Put the pipe back to listening and wait again */
            InterlockedExchange(&Serial->Attached, 0);
            DisconnectNamedPipe(Serial->Input);

            Host->Log(Host->Context, RtvmLogInfo,
                      "%s: the other end went away\n", Serial->Device.Name);
            continue;
        }

        /*
         * Held here rather than pushed straight at the guest's queue. What
         * arrives comes in lumps far bigger than that queue, and the guest
         * only makes room a byte at a time.
         */
        EnterCriticalSection(&Serial->Lock);

        for (Index = 0; Index < Read; Index++)
        {
            if (Serial->InCount >= UART_SEND_SIZE)
            {
                Serial->LineStatus |= LSR_OVERRUN;
                break;
            }

            Serial->Incoming[Serial->InWrite] = Buffer[Index];
            Serial->InWrite = (Serial->InWrite + 1) % UART_SEND_SIZE;
            Serial->InCount++;
        }

        SerialTopUp(Serial);
        SerialUpdateLine(Serial);

        LeaveCriticalSection(&Serial->Lock);
    }

    return 0;
}

/* THE DEVICE *****************************************************************/

static
RTVM_STATUS
RTVMAPI
SerialStart(
    _In_ PRTVM_DEVICE Device)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    Status = Host->ClaimPortRange(Host->Context,
                                  Device,
                                  Serial->BasePort,
                                  UART_REGISTER_COUNT);
    if (!RTVM_SUCCESS(Status))
    {
        Host->Log(Host->Context, RtvmLogError,
                  "%s: ports %04x through %04x are already answered for\n",
                  Device->Name,
                  Serial->BasePort,
                  Serial->BasePort + UART_REGISTER_COUNT - 1);
        return Status;
    }

    /*
     * Held here until something plugs in, when that was asked for. A machine
     * that came up before anything was listening would have said everything it
     * had to say about coming up before there was anything to say it to, and
     * what it says then is usually the whole reason for watching.
     */
    if (Serial->Listening)
    {
        Serial->ReadDone = CreateEventA(NULL, TRUE, FALSE, NULL);
        Serial->WriteDone = CreateEventA(NULL, TRUE, FALSE, NULL);
        Serial->AttachDone = CreateEventA(NULL, TRUE, FALSE, NULL);
    }

    if (Serial->HoldForAttach)
    {
        Host->Log(Host->Context, RtvmLogInfo,
                  "%s: held until something plugs into the other end\n",
                  Device->Name);

        if (SerialAwaitClient(Serial))
            InterlockedExchange(&Serial->Attached, 1);
    }

    if (Serial->Input != INVALID_HANDLE_VALUE)
    {
        Serial->Reader = CreateThread(NULL, 0, SerialReader, Serial, 0, NULL);
        if (Serial->Reader == NULL)
        {
            Host->Log(Host->Context, RtvmLogWarning,
                      "%s: nothing will be read back, the reader would not start\n",
                      Device->Name);
        }
    }

    if (Serial->Output != INVALID_HANDLE_VALUE)
    {
        Serial->Ready = CreateEventA(NULL, FALSE, FALSE, NULL);
        Serial->Writer = CreateThread(NULL, 0, SerialWriter, Serial, 0, NULL);

        if (Serial->Writer == NULL)
        {
            Host->Log(Host->Context, RtvmLogWarning,
                      "%s: nothing will be written out, the writer would not start\n",
                      Device->Name);
        }
    }

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: 16550A at %04x on line %lu, %s\n",
              Device->Name,
              Serial->BasePort,
              Serial->Line,
              Serial->Backend);

    return RtvmOk;
}

static
VOID
RTVMAPI
SerialStop(
    _In_ PRTVM_DEVICE Device)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;

    InterlockedExchange(&Serial->Stopping, 1);

    /* The one taking bytes out goes first, so the last of them still get out */
    if (Serial->Writer != NULL)
    {
        if (Serial->Ready != NULL)
            SetEvent(Serial->Ready);

        WaitForSingleObject(Serial->Writer, 2000);
        CloseHandle(Serial->Writer);
        Serial->Writer = NULL;
    }

    if (Serial->Ready != NULL)
    {
        CloseHandle(Serial->Ready);
        Serial->Ready = NULL;
    }

    if (Serial->WriteDone != NULL)
    {
        CloseHandle(Serial->WriteDone);
        Serial->WriteDone = NULL;
    }

    /*
     * The reader is sitting in a read, or in a wait for something to plug in,
     * and neither comes back on its own. The handle is closed under it, which
     * is what ends either one.
     */
    if (Serial->Input != INVALID_HANDLE_VALUE)
    {
        if (Serial->Listening)
            DisconnectNamedPipe(Serial->Input);

        CloseHandle(Serial->Input);
        Serial->Input = INVALID_HANDLE_VALUE;

        /* The same handle both ways when it is a pipe, and now closed */
        if (Serial->Listening)
            Serial->Output = INVALID_HANDLE_VALUE;
    }

    if (Serial->Reader != NULL)
    {
        WaitForSingleObject(Serial->Reader, 2000);
        CloseHandle(Serial->Reader);
        Serial->Reader = NULL;
    }

    if (Serial->ReadDone != NULL)
    {
        CloseHandle(Serial->ReadDone);
        Serial->ReadDone = NULL;
    }

    if (Serial->AttachDone != NULL)
    {
        CloseHandle(Serial->AttachDone);
        Serial->AttachDone = NULL;
    }
}

static
VOID
RTVMAPI
SerialReset(
    _In_ PRTVM_DEVICE Device)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;

    EnterCriticalSection(&Serial->Lock);

    Serial->InterruptEnable = 0;
    Serial->LineControl = 0x03;
    Serial->ModemControl = 0;
    Serial->LineStatus = LSR_SEND_EMPTY | LSR_SEND_IDLE;
    Serial->ModemStatus = 0;
    Serial->Scratch = 0;
    Serial->QueueControl = 0;
    Serial->QueuesEnabled = FALSE;
    Serial->ReceiveTrigger = 1;
    Serial->Divisor = 12;

    SerialQueueReset(&Serial->Received);
    Serial->InCount = 0;
    Serial->InRead = 0;
    Serial->InWrite = 0;
    SerialUpdateLine(Serial);

    LeaveCriticalSection(&Serial->Lock);
}

static
VOID
RTVMAPI
SerialDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;

    SerialStop(Device);

    if (Serial->Output != INVALID_HANDLE_VALUE)
        CloseHandle(Serial->Output);

    DeleteCriticalSection(&Serial->Lock);
    free(Serial);
}

static
RTVM_STATUS
RTVMAPI
SerialIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;
    ULONG Register = (ULONG)(Port - Serial->BasePort);
    UCHAR Result = 0xFF;
    UCHAR Taken;

    UNREFERENCED_PARAMETER(Width);

    if (Register >= UART_REGISTER_COUNT)
        return RtvmNotClaimed;

    EnterCriticalSection(&Serial->Lock);

    switch (Register)
    {
        case UART_RBR:
            if (Serial->LineControl & LCR_DIVISOR_LATCH)
            {
                Result = (UCHAR)(Serial->Divisor & 0xFF);
                break;
            }

            if (SerialQueueTake(&Serial->Received, &Taken))
                Result = Taken;

            /* A byte has gone, so there is room for the next of what arrived */
            SerialTopUp(Serial);

            if (Serial->Received.Count == 0)
                Serial->LineStatus &= ~LSR_DATA_READY;

            SerialUpdateLine(Serial);
            break;

        case UART_IER:
            if (Serial->LineControl & LCR_DIVISOR_LATCH)
                Result = (UCHAR)(Serial->Divisor >> 8);
            else
                Result = Serial->InterruptEnable;
            break;

        case UART_IIR:
            Result = SerialIdentify(Serial);

            /*
             * Reading this is how a driver acknowledges that the sender is
             * empty, and the only way that reason goes away.
             */
            if ((Result & 0x0F) == IIR_SEND_EMPTY)
            {
                Serial->InterruptEnable &= ~IER_SEND_EMPTY;
                SerialUpdateLine(Serial);
            }
            break;

        case UART_LCR:
            Result = Serial->LineControl;
            break;

        case UART_MCR:
            Result = Serial->ModemControl;
            break;

        case UART_LSR:
            Result = Serial->LineStatus;

            /* The error bits are only reported once */
            Serial->LineStatus &= ~(LSR_OVERRUN | LSR_PARITY_ERROR |
                                    LSR_FRAMING_ERROR | LSR_BREAK);
            SerialUpdateLine(Serial);
            break;

        case UART_MSR:
            if (Serial->ModemControl & MCR_LOOPBACK)
            {
                /*
                 * Turned around on itself: what the driver raised comes back
                 * on the matching input, which is what it checks for.
                 */
                Result = 0;
                if (Serial->ModemControl & MCR_DTR) Result |= MSR_DSR;
                if (Serial->ModemControl & MCR_RTS) Result |= MSR_CTS;
                if (Serial->ModemControl & MCR_OUT1) Result |= MSR_RING;
                if (Serial->ModemControl & MCR_OUT2) Result |= MSR_CARRIER;
            }
            else
            {
                Result = Serial->ModemStatus;
            }

            /* The four that say a line moved are cleared by being read */
            Serial->ModemStatus &= ~0x0F;
            SerialUpdateLine(Serial);
            break;

        case UART_SCR:
            Result = Serial->Scratch;
            break;
    }

    LeaveCriticalSection(&Serial->Lock);

    *Value = Result;
    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
SerialIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PSERIAL_DEVICE Serial = (PSERIAL_DEVICE)Device->DeviceContext;
    ULONG Register = (ULONG)(Port - Serial->BasePort);
    UCHAR Byte = (UCHAR)(Value & 0xFF);

    UNREFERENCED_PARAMETER(Width);

    if (Register >= UART_REGISTER_COUNT)
        return RtvmNotClaimed;

    EnterCriticalSection(&Serial->Lock);

    switch (Register)
    {
        case UART_THR:
            if (Serial->LineControl & LCR_DIVISOR_LATCH)
            {
                Serial->Divisor = (USHORT)((Serial->Divisor & 0xFF00) | Byte);
                break;
            }

            SerialSend(Serial, Byte);

            /*
             * Sending takes no time here, so the sender is empty again before
             * the write returns and the driver may be told so at once.
             */
            Serial->LineStatus |= LSR_SEND_EMPTY | LSR_SEND_IDLE;
            SerialUpdateLine(Serial);
            break;

        case UART_IER:
            if (Serial->LineControl & LCR_DIVISOR_LATCH)
            {
                Serial->Divisor = (USHORT)((Serial->Divisor & 0x00FF) | (Byte << 8));
                break;
            }

            Serial->InterruptEnable = Byte & IER_KNOWN;
            SerialUpdateLine(Serial);
            break;

        case UART_FCR:
            Serial->QueueControl = Byte;
            Serial->QueuesEnabled = (Byte & FCR_ENABLE) != 0;

            if (Byte & FCR_CLEAR_RECEIVE)
            {
                SerialQueueReset(&Serial->Received);
    Serial->InCount = 0;
    Serial->InRead = 0;
    Serial->InWrite = 0;
                Serial->InCount = 0;
                Serial->InRead = 0;
                Serial->InWrite = 0;
                Serial->LineStatus &= ~LSR_DATA_READY;
            }

            switch (Byte & FCR_TRIGGER_MASK)
            {
                case 0x00: Serial->ReceiveTrigger = 1; break;
                case 0x40: Serial->ReceiveTrigger = 4; break;
                case 0x80: Serial->ReceiveTrigger = 8; break;
                default:   Serial->ReceiveTrigger = 14; break;
            }

            SerialUpdateLine(Serial);
            break;

        case UART_LCR:
            Serial->LineControl = Byte;
            break;

        case UART_MCR:
            Serial->ModemControl = Byte & 0x1F;
            SerialUpdateLine(Serial);
            break;

        case UART_LSR:
        case UART_MSR:
            /* Reported by the port, not set by the driver */
            break;

        case UART_SCR:
            Serial->Scratch = Byte;
            break;
    }

    LeaveCriticalSection(&Serial->Lock);
    return RtvmOk;
}

static const RTVM_DEVICE_VTABLE SerialVtable =
{
    sizeof(SerialVtable),
    SerialStart,
    SerialStop,
    SerialReset,
    SerialDestroy,
    SerialIoRead,
    SerialIoWrite,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

/**
 * @brief
 * Reads one setting out of the parameter string.
 *
 * @param[in] Parameters
 * Settings separated by commas, as name=value or a bare name.
 *
 * @param[in] Name
 * Which one to look for.
 *
 * @return
 * Where the value starts, or NULL when the setting is not there. A bare name
 * answers with the empty string, so that being present can be told from being
 * absent.
 */
static
PCSTR
SerialSetting(
    _In_opt_ PCSTR Parameters,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Walk = Parameters;

    while ((Walk != NULL) && (*Walk != '\0'))
    {
        if ((_strnicmp(Walk, Name, Length) == 0) &&
            ((Walk[Length] == '=') || (Walk[Length] == ',') || (Walk[Length] == '\0')))
        {
            return (Walk[Length] == '=') ? &Walk[Length + 1] : &Walk[Length];
        }

        Walk = strchr(Walk, ',');
        if (Walk != NULL)
            Walk++;
    }

    return NULL;
}

static
VOID
SerialCopySetting(
    _In_ PCSTR Value,
    _Out_writes_z_(Size) PSTR Buffer,
    _In_ SIZE_T Size)
{
    SIZE_T Index = 0;

    while ((Index + 1 < Size) && (Value[Index] != '\0') && (Value[Index] != ','))
    {
        Buffer[Index] = Value[Index];
        Index++;
    }

    Buffer[Index] = '\0';
}

/**
 * @brief
 * Opens wherever the port's bytes are meant to go.
 *
 * @remarks
 * A pipe is the one worth having: the manager's window sits on the other end
 * and the machine can be watched while it runs. A file is for a boot nobody is
 * watching, and is opened for append so that a second run does not lose the
 * first. With neither, the port still works and the bytes go nowhere, which is
 * what a real one with nothing plugged into it does.
 */
static
VOID
SerialOpenBackend(
    _Inout_ PSERIAL_DEVICE Serial,
    _In_opt_ PCSTR Parameters)
{
    CHAR Path[MAX_PATH];
    PCSTR Value;
    HANDLE Handle;

    Serial->Output = INVALID_HANDLE_VALUE;
    Serial->Input = INVALID_HANDLE_VALUE;

    Value = SerialSetting(Parameters, "pipe");
    if ((Value != NULL) && (*Value != '\0'))
    {
        CHAR Name[MAX_PATH];

        SerialCopySetting(Value, Path, sizeof(Path));
        StringCchPrintfA(Name, sizeof(Name), "\\\\.\\pipe\\%s", Path);

        Handle = CreateNamedPipeA(Name,
                                  PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  1,
                                  4096,
                                  4096,
                                  0,
                                  NULL);
        if (Handle != INVALID_HANDLE_VALUE)
        {
            Serial->Output = Handle;
            Serial->Input = Handle;
            Serial->Listening = TRUE;
            Serial->HoldForAttach = (SerialSetting(Parameters, "wait") != NULL);
            StringCchPrintfA(Serial->Backend, sizeof(Serial->Backend),
                             "waiting for something on %s", Name);
            return;
        }
    }

    Value = SerialSetting(Parameters, "file");
    if ((Value != NULL) && (*Value != '\0'))
    {
        SerialCopySetting(Value, Path, sizeof(Path));

        Handle = CreateFileA(Path,
                             FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
        if (Handle != INVALID_HANDLE_VALUE)
        {
            Serial->Output = Handle;
            StringCchPrintfA(Serial->Backend, sizeof(Serial->Backend),
                             "writing to %s", Path);
            return;
        }
    }

    StringCchCopyA(Serial->Backend, sizeof(Serial->Backend), "not connected");
}

static
RTVM_STATUS
RTVMAPI
SerialCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PSERIAL_DEVICE Serial;
    PCSTR Value;

    Serial = (PSERIAL_DEVICE)calloc(1, sizeof(*Serial));
    if (Serial == NULL)
        return RtvmNoMemory;

    Serial->Device.Size = sizeof(Serial->Device);
    Serial->Device.Vtable = &SerialVtable;
    Serial->Device.Host = Host;
    Serial->Device.DeviceContext = Serial;

    /* The second port unless told otherwise, because the first is the default */
    if (SerialSetting(Parameters, "com2") != NULL)
    {
        Serial->BasePort = SERIAL_COM2_PORT;
        Serial->Line = SERIAL_COM2_LINE;
        StringCchCopyA(Serial->Device.Name, sizeof(Serial->Device.Name), "com2");
    }
    else
    {
        Serial->BasePort = SERIAL_COM1_PORT;
        Serial->Line = SERIAL_COM1_LINE;
        StringCchCopyA(Serial->Device.Name, sizeof(Serial->Device.Name), "com1");
    }

    /* A port put somewhere of its own, for a machine that wants more than two */
    Value = SerialSetting(Parameters, "port");
    if ((Value != NULL) && (*Value != '\0'))
        Serial->BasePort = (USHORT)strtoul(Value, NULL, 0);

    Value = SerialSetting(Parameters, "line");
    if ((Value != NULL) && (*Value != '\0'))
        Serial->Line = strtoul(Value, NULL, 0);

    InitializeCriticalSection(&Serial->Lock);
    SerialOpenBackend(Serial, Parameters);
    SerialReset(&Serial->Device);

    *Device = &Serial->Device;
    return RtvmOk;
}

/* WHAT THIS MODULE HAS *******************************************************/

static const RTVM_DEVICE_CLASS SerialClasses[] =
{
    {
        "serial",
        "16550A serial port, to a pipe or a file",
        SerialCreate
    }
};

static const RTVM_DEVICE_MODULE SerialModule =
{
    sizeof(SerialModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmserial",
    "Serial ports",
    RTL_NUMBER_OF(SerialClasses),
    SerialClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &SerialModule;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/* EOF */
