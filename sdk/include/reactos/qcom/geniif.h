/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QUPv3 GENI serial engine I2C master interface (qcomgeni.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

typedef struct _QCOM_GENI_I2C *PQCOM_GENI_I2C;

/*
 * Where the engine and its clocks live. The engine clock is fed by one root
 * clock generator (RCG) and gated by a branch, the AHB clocks of the wrapper
 * are gated by votes in the same vote register.
 */
typedef struct _QCOM_GENI_I2C_CONFIG
{
    PHYSICAL_ADDRESS EngineBase;
    PHYSICAL_ADDRESS ClockControllerBase;
    ULONG ClockVoteOffset;
    ULONG ClockVoteMask;
    ULONG EngineBranchOffset;
    ULONG EngineRcgOffset;
    ULONG BusSpeedHz;
} QCOM_GENI_I2C_CONFIG, *PQCOM_GENI_I2C_CONFIG;

typedef const QCOM_GENI_I2C_CONFIG *PCQCOM_GENI_I2C_CONFIG;

NTSTATUS
NTAPI
QcGeniI2cOpen(
    _In_ PCQCOM_GENI_I2C_CONFIG Config,
    _Out_ PQCOM_GENI_I2C *I2c);

VOID
NTAPI
QcGeniI2cClose(
    _In_ PQCOM_GENI_I2C I2c);

/*
 * Writes then reads with a repeated start. Either phase may be empty.
 * STATUS_NO_SUCH_DEVICE means the address was not acknowledged.
 */
NTSTATUS
NTAPI
QcGeniI2cTransfer(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ USHORT Address,
    _In_reads_bytes_opt_(WriteLength) const UCHAR *WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_opt_(ReadLength) PUCHAR ReadBuffer,
    _In_ ULONG ReadLength);

VOID
NTAPI
QcGeniI2cDump(
    _In_ PQCOM_GENI_I2C I2c,
    _In_z_ PCSTR Label);
