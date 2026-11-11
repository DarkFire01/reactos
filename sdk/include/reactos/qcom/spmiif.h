/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SPMI PMIC arbiter interface (qcomspmi.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * Reads registers from a PMIC on the SPMI bus. Sid is the slave the PMIC sits
 * at, Address is the peripheral in the high byte and the register in the low
 * byte. Up to four bytes come back at once.
 */
NTSTATUS
NTAPI
QcSpmiRead(
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length);
