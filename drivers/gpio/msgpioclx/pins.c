/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pin connect, disconnect and I/O
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

/**
 * @brief
 * Splits a controller wide pin number into a bank and an offset within it.
 *
 * @return
 * TRUE when the pin exists on this controller.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
GcxPinToBank(
    PGCX_CONTROLLER Controller,
    ULONG Pin,
    PBANK_ID BankId,
    PPIN_NUMBER Offset)
{
    ULONG PinsPerBank = Controller->Information.NumberOfPinsPerBank;

    if (PinsPerBank == 0 || Pin >= Controller->Information.TotalPins)
        return FALSE;

    *BankId = (BANK_ID)(Pin / PinsPerBank);
    *Offset = (PIN_NUMBER)(Pin % PinsPerBank);
    return TRUE;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxConnectIoPins(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PPIN_NUMBER PinNumberTable,
    USHORT PinCount,
    GPIO_CONNECT_IO_PINS_MODE ConnectMode,
    UCHAR PullConfiguration)
{
    GPIO_CONNECT_IO_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_ConnectIoPins == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.ConnectMode = ConnectMode;
    Parameters.PullConfiguration = PullConfiguration;
    return Controller->Registration.CLIENT_ConnectIoPins(GcxClientContext(Controller),
                                                         &Parameters);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxDisconnectIoPins(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PPIN_NUMBER PinNumberTable,
    USHORT PinCount,
    GPIO_CONNECT_IO_PINS_MODE DisconnectMode)
{
    GPIO_DISCONNECT_IO_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_DisconnectIoPins == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.DisconnectMode = DisconnectMode;
    return Controller->Registration.CLIENT_DisconnectIoPins(GcxClientContext(Controller),
                                                            &Parameters);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxReadGpioPins(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PPIN_NUMBER PinNumberTable,
    ULONG PinCount,
    PULONG64 Buffer)
{
    GPIO_READ_PINS_PARAMETERS Parameters;

    *Buffer = 0;

    if (Controller->Registration.CLIENT_ReadGpioPins == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.Buffer = Buffer;
    return Controller->Registration.CLIENT_ReadGpioPins(GcxClientContext(Controller),
                                                        &Parameters);
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxWriteGpioPins(
    PGCX_CONTROLLER Controller,
    BANK_ID BankId,
    PPIN_NUMBER PinNumberTable,
    ULONG PinCount,
    PULONG64 Buffer)
{
    GPIO_WRITE_PINS_PARAMETERS Parameters;

    if (Controller->Registration.CLIENT_WriteGpioPins == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = BankId;
    Parameters.PinNumberTable = PinNumberTable;
    Parameters.PinCount = PinCount;
    Parameters.Buffer = Buffer;
    return Controller->Registration.CLIENT_WriteGpioPins(GcxClientContext(Controller),
                                                         &Parameters);
}
