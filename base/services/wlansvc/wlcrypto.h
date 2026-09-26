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

/* SAE on the ECC group 19 */

#define WLAN_SAE_GROUP              19
#define WLAN_SAE_SCALAR_LENGTH      32
#define WLAN_SAE_ELEMENT_LENGTH     64
#define WLAN_SAE_KCK_LENGTH         32
#define WLAN_SAE_PMK_LENGTH         32
#define WLAN_SAE_PMKID_LENGTH       16
#define WLAN_SAE_CONFIRM_LENGTH     32

typedef struct _WLAN_SAE
{
    PVOID State;
    BOOL HashToElement;
    UCHAR Scalar[WLAN_SAE_SCALAR_LENGTH];
    UCHAR Element[WLAN_SAE_ELEMENT_LENGTH];
    UCHAR PeerScalar[WLAN_SAE_SCALAR_LENGTH];
    UCHAR PeerElement[WLAN_SAE_ELEMENT_LENGTH];
    UCHAR Kck[WLAN_SAE_KCK_LENGTH];
    UCHAR Pmk[WLAN_SAE_PMK_LENGTH];
    UCHAR Pmkid[WLAN_SAE_PMKID_LENGTH];
} WLAN_SAE, *PWLAN_SAE;

BOOL
WlanSaeStart(
    _Out_ PWLAN_SAE Sae,
    _In_reads_bytes_(6) const UCHAR *OwnMac,
    _In_reads_bytes_(6) const UCHAR *PeerMac,
    _In_reads_bytes_(PasswordLength) const UCHAR *Password,
    _In_ ULONG PasswordLength,
    _In_reads_bytes_opt_(SsidLength) const UCHAR *Ssid,
    _In_ ULONG SsidLength,
    _In_ BOOL HashToElement,
    _In_reads_bytes_opt_(WLAN_SAE_SCALAR_LENGTH) const UCHAR *Rand,
    _In_reads_bytes_opt_(WLAN_SAE_SCALAR_LENGTH) const UCHAR *Mask);

BOOL
WlanSaeTakeCommit(
    _Inout_ PWLAN_SAE Sae,
    _In_reads_bytes_(WLAN_SAE_SCALAR_LENGTH) const UCHAR *PeerScalar,
    _In_reads_bytes_(WLAN_SAE_ELEMENT_LENGTH) const UCHAR *PeerElement);

VOID
WlanSaeConfirm(
    _In_ PWLAN_SAE Sae,
    _In_ USHORT SendConfirm,
    _Out_writes_bytes_(WLAN_SAE_CONFIRM_LENGTH) PUCHAR Confirm);

BOOL
WlanSaeCheckConfirm(
    _In_ PWLAN_SAE Sae,
    _In_ USHORT PeerSendConfirm,
    _In_reads_bytes_(WLAN_SAE_CONFIRM_LENGTH) const UCHAR *Confirm);

VOID
WlanSaeFinish(
    _Inout_ PWLAN_SAE Sae);
