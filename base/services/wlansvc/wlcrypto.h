/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Cryptographic primitives the supplicant uses
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WLAN_SHA1_LENGTH            20
#define WLAN_SHA256_LENGTH          32
#define WLAN_CMAC_LENGTH            16

VOID
WlanCryptoInitialize(VOID);

BOOL
WlanCryptoRandom(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length);

BOOL
WlanCryptoPbkdf2Sha1(
    _In_reads_bytes_(PasswordLength) const UCHAR *Password,
    _In_ ULONG PasswordLength,
    _In_reads_bytes_(SaltLength) const UCHAR *Salt,
    _In_ ULONG SaltLength,
    _In_ ULONG Iterations,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength);

VOID
WlanCryptoHmacSha1(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_SHA1_LENGTH) PUCHAR Mac);

VOID
WlanCryptoHmacSha256(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_SHA256_LENGTH) PUCHAR Mac);

BOOL
WlanCryptoAesCmac(
    _In_reads_bytes_(16) const UCHAR *Key,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(WLAN_CMAC_LENGTH) PUCHAR Mac);

BOOL
WlanCryptoAesUnwrap(
    _In_reads_bytes_(KekLength) const UCHAR *Kek,
    _In_ ULONG KekLength,
    _In_reads_bytes_(InputLength) const UCHAR *Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_(OutputSize, *OutputLength) PUCHAR Output,
    _In_ ULONG OutputSize,
    _Out_ PULONG OutputLength);

BOOL
WlanCryptoPrfSha1(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_ PCSTR Label,
    _In_reads_bytes_(DataLength) const UCHAR *Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength);

BOOL
WlanCryptoKdfSha256(
    _In_reads_bytes_(KeyLength) const UCHAR *Key,
    _In_ ULONG KeyLength,
    _In_ PCSTR Label,
    _In_reads_bytes_(ContextLength) const UCHAR *Context,
    _In_ ULONG ContextLength,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength);

VOID
WlanCryptoWipe(
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Length);
