/*
 * COPYRIGHT:       GPL, see COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            drivers/base/kdcom/kdcom.c
 * PURPOSE:         COM port functions for the kernel debugger.
 * PROGRAMMER:      Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#include "kddll.h"

#include <arc/arc.h>
#include <stdlib.h>
#include <ndk/halfuncs.h>

#include <cportlib/cportlib.h>
#include <cportlib/uartinfo.h>

/* GLOBALS ********************************************************************/

CPPORT KdComPort;
#ifdef _M_ARM64
/* QEMU virt PL011 UART base. Can be overridden via DEBUGPORT=COM:<addr>. */
#define ARM64_DEFAULT_DEBUG_UART_BASE 0x09000000ULL
#define ARM64_UART_DR                 0x00ULL
#define ARM64_UART_FR                 0x18ULL
#define ARM64_UART_FR_RXFE            0x10UL
#define ARM64_UART_FR_TXFF            0x20UL

static ULONG_PTR KdArm64UartBase;

static __forceinline
volatile ULONG *
KdArm64UartReg(IN ULONG_PTR Offset)
{
    return (volatile ULONG *)(ULONG_PTR)(KdArm64UartBase + Offset);
}

static __forceinline
VOID
KdArm64UartWriteByte(IN UCHAR Byte)
{
    while ((*KdArm64UartReg(ARM64_UART_FR) & ARM64_UART_FR_TXFF) != 0)
    {
    }

    *KdArm64UartReg(ARM64_UART_DR) = Byte;
}

static __forceinline
BOOLEAN
KdArm64UartReadByte(IN PUCHAR Byte,
                    IN BOOLEAN Wait)
{
    ULONG Timeout = 1024 * 200;

    while ((*KdArm64UartReg(ARM64_UART_FR) & ARM64_UART_FR_RXFE) != 0)
    {
        if (!Wait)
            return FALSE;

        if (Timeout-- == 0)
            return FALSE;
    }

    *Byte = (UCHAR)(*KdArm64UartReg(ARM64_UART_DR) & 0xFF);
    return TRUE;
}
#endif
#ifdef KDDEBUG
CPPORT KdDebugComPort;
#endif

/* DEBUGGING ******************************************************************/

#ifdef KDDEBUG
#include <stdio.h>
ULONG KdpDbgPrint(const char *Format, ...)
{
    va_list ap;
    int Length;
    char* ptr;
    CHAR Buffer[512];

    va_start(ap, Format);
    Length = _vsnprintf(Buffer, sizeof(Buffer), Format, ap);
    va_end(ap);

    /* Check if we went past the buffer */
    if (Length == -1)
    {
        /* Terminate it if we went over-board */
        Buffer[sizeof(Buffer) - 1] = '\n';

        /* Put maximum */
        Length = sizeof(Buffer);
    }

    ptr = Buffer;
    while (Length--)
    {
        if (*ptr == '\n')
            CpPutByte(&KdDebugComPort, '\r');

        CpPutByte(&KdDebugComPort, *ptr++);
    }

    return 0;
}
#endif

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
KdD0Transition(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdD3Transition(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdSave(IN BOOLEAN SleepTransition)
{
    /* Nothing to do on COM ports */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdRestore(IN BOOLEAN SleepTransition)
{
    /* Nothing to do on COM ports */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdpPortInitialize(
    _In_ PUCHAR PortAddress,
    _In_ ULONG BaudRate)
{
#ifdef _M_ARM64
    UNREFERENCED_PARAMETER(BaudRate);

    if (PortAddress == NULL)
        PortAddress = (PUCHAR)(ULONG_PTR)ARM64_DEFAULT_DEBUG_UART_BASE;

    KdArm64UartBase = (ULONG_PTR)PortAddress;
    KdComPortInUse = PortAddress;
    return STATUS_SUCCESS;
#else
    NTSTATUS Status;

    Status = CpInitialize(&KdComPort, PortAddress, BaudRate);
    if (!NT_SUCCESS(Status))
        return STATUS_INVALID_PARAMETER;

    KdComPortInUse = KdComPort.Address;
    return STATUS_SUCCESS;
#endif
}

/******************************************************************************
 * \name KdDebuggerInitialize0
 * \brief Phase 0 initialization.
 * \param [opt] LoaderBlock Pointer to the Loader parameter block. Can be NULL.
 * \return Status
 */
NTSTATUS
NTAPI
KdDebuggerInitialize0(IN PLOADER_PARAMETER_BLOCK LoaderBlock OPTIONAL)
{
#define CONST_STR_LEN(x) (sizeof(x)/sizeof(x[0]) - 1)

    ULONG ComPortNumber   = DEFAULT_DEBUG_PORT;
    ULONG ComPortBaudRate = DEFAULT_DEBUG_BAUD_RATE;
    PUCHAR ComPortAddress = NULL;

    PSTR CommandLine, PortString, BaudString;
    ULONG Value;

    /* Check if we have a LoaderBlock */
    if (LoaderBlock)
    {
        /* Get the Command Line */
        CommandLine = LoaderBlock->LoadOptions;

        /* Upcase it */
        _strupr(CommandLine);

        /* Get the port and baud rate */
        PortString = strstr(CommandLine, "DEBUGPORT");
        BaudString = strstr(CommandLine, "BAUDRATE");

        /* Check if we got the DEBUGPORT parameter */
        if (PortString)
        {
            /* Move past the actual string and any spaces */
            PortString += CONST_STR_LEN("DEBUGPORT");
            while (*PortString == ' ') ++PortString;
            /* Skip the equals sign */
            if (*PortString) ++PortString;

            /* Do we have a serial port? */
            if (_strnicmp(PortString, "COM", CONST_STR_LEN("COM")) != 0)
                return STATUS_INVALID_PARAMETER;

            /* Check for a valid serial port */
            PortString += CONST_STR_LEN("COM");
            if (*PortString != ':')
            {
                Value = (ULONG)atol(PortString);
                if (Value > MAX_COM_PORTS)
                    return STATUS_INVALID_PARAMETER;
                // if (Value > 0 && Value <= MAX_COM_PORTS)
                /* Set the port to use */
                ComPortNumber = Value;
            }
            else
            {
                /* Retrieve and set its address */
                Value = strtoul(PortString + 1, NULL, 0);
                if (Value)
                {
                    ComPortNumber = 0;
                    ComPortAddress = UlongToPtr(Value);
                }
            }
        }

        /* Check if we got a baud rate */
        if (BaudString)
        {
            /* Move past the actual string and any spaces */
            BaudString += CONST_STR_LEN("BAUDRATE");
            while (*BaudString == ' ') ++BaudString;

            /* Make sure we have a rate */
            if (*BaudString)
            {
                /* Read and set it */
                Value = (ULONG)atol(BaudString + 1);
                if (Value) ComPortBaudRate = Value;
            }
        }
    }

    if (!ComPortAddress)
#ifdef _M_ARM64
        ComPortAddress = UlongToPtr(ARM64_DEFAULT_DEBUG_UART_BASE);
#else
        ComPortAddress = UlongToPtr(BaseArray[ComPortNumber]);
#endif

#ifdef KDDEBUG
    /*
     * Try to find a free COM port and use it as the KD debugging port.
     * NOTE: Inspired by freeldr/comm/rs232.c, Rs232PortInitialize(...)
     */
    {
    /*
     * Enumerate COM ports from the last to the first one, and stop
     * when we find a valid port. If we reach the first list element
     * (the undefined COM port), no valid port was found.
     */
    PUCHAR Address = NULL;
    ULONG ComPort;
    for (ComPort = MAX_COM_PORTS; ComPort > 0; ComPort--)
    {
        /* Check if the port exist; skip the KD port */
        Address = UlongToPtr(BaseArray[ComPort]);
        if ((Address != ComPortAddress) && CpDoesPortExist(Address))
            break;
    }
    if (ComPort != 0 && Address != NULL)
        CpInitialize(&KdDebugComPort, Address, DEFAULT_BAUD_RATE);
    }
#endif

    /* Initialize the port */
    return KdpPortInitialize(ComPortAddress, ComPortBaudRate);
}

/******************************************************************************
 * \name KdDebuggerInitialize1
 * \brief Phase 1 initialization.
 * \param [opt] LoaderBlock Pointer to the Loader parameter block. Can be NULL.
 * \return Status
 */
NTSTATUS
NTAPI
KdDebuggerInitialize1(IN PLOADER_PARAMETER_BLOCK LoaderBlock OPTIONAL)
{
    return STATUS_SUCCESS;
}


VOID
NTAPI
KdpSendByte(IN UCHAR Byte)
{
#ifdef _M_ARM64
    KdArm64UartWriteByte(Byte);
#else
    /* Send the byte */
    CpPutByte(&KdComPort, Byte);
#endif
}

KDP_STATUS
NTAPI
KdpPollByte(OUT PUCHAR OutByte)
{
#ifdef _M_ARM64
    return KdArm64UartReadByte(OutByte, FALSE) ?
           KDP_PACKET_RECEIVED :
           KDP_PACKET_TIMEOUT;
#else
    USHORT Status;

    /* Poll the byte */
    Status = CpGetByte(&KdComPort, OutByte, FALSE, FALSE);
    switch (Status)
    {
        case CP_GET_SUCCESS:
            return KDP_PACKET_RECEIVED;

        case CP_GET_NODATA:
            return KDP_PACKET_TIMEOUT;

        case CP_GET_ERROR:
        default:
            return KDP_PACKET_RESEND;
    }
#endif
}

KDP_STATUS
NTAPI
KdpReceiveByte(OUT PUCHAR OutByte)
{
#ifdef _M_ARM64
    return KdArm64UartReadByte(OutByte, TRUE) ?
           KDP_PACKET_RECEIVED :
           KDP_PACKET_TIMEOUT;
#else
    USHORT Status;

    /* Get the byte */
    Status = CpGetByte(&KdComPort, OutByte, TRUE, FALSE);
    switch (Status)
    {
        case CP_GET_SUCCESS:
            return KDP_PACKET_RECEIVED;

        case CP_GET_NODATA:
            return KDP_PACKET_TIMEOUT;

        case CP_GET_ERROR:
        default:
            return KDP_PACKET_RESEND;
    }
#endif
}

KDP_STATUS
NTAPI
KdpPollBreakIn(VOID)
{
    KDP_STATUS KdStatus;
    UCHAR Byte;

    KdStatus = KdpPollByte(&Byte);
    if ((KdStatus == KDP_PACKET_RECEIVED) && (Byte == BREAKIN_PACKET_BYTE))
    {
        return KDP_PACKET_RECEIVED;
    }
    return KDP_PACKET_TIMEOUT;
}

/* EOF */
