/*
 * PROJECT:     ReactOS WLAN service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SAE, the WPA3 personal authentication, on the ECC group 19
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * SymCrypt finds the password element and does the curve arithmetic. The
 * key schedule and the confirm exchange around it are done here.
 */

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <symcrypt.h>
#include <symcrypt_low_level.h>

#include "wlcrypto.h"

/**
 * @brief
 * Starts an SAE exchange: finds the password element, by hunting and pecking
 * or from the SSID with hash-to-element, and makes our commit scalar and element.
 *
 * @param[in] Rand, Mask
 * The random choices, normally NULL. A test passes fixed ones.
 */
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
    _In_reads_bytes_opt_(WLAN_SAE_SCALAR_LENGTH) const UCHAR *Mask)
{
    PSYMCRYPT_802_11_SAE_CUSTOM_STATE State;
    UCHAR RandBuffer[WLAN_SAE_SCALAR_LENGTH];
    UCHAR MaskBuffer[WLAN_SAE_SCALAR_LENGTH];
    UCHAR Pt[WLAN_SAE_ELEMENT_LENGTH];
    SYMCRYPT_ERROR Error;

    RtlZeroMemory(Sae, sizeof(*Sae));

    State = SymCryptCallbackAlloc(sizeof(*State));
    if (State == NULL)
        return FALSE;

    /* Zeroed buffers make SymCrypt pick the values itself */
    RtlZeroMemory(RandBuffer, sizeof(RandBuffer));
    RtlZeroMemory(MaskBuffer, sizeof(MaskBuffer));
    if (Rand != NULL)
        RtlCopyMemory(RandBuffer, Rand, sizeof(RandBuffer));
    if (Mask != NULL)
        RtlCopyMemory(MaskBuffer, Mask, sizeof(MaskBuffer));

    if (HashToElement)
    {
        Error = SymCrypt802_11SaeCustomCreatePT(Ssid, SsidLength, Password, PasswordLength,
                                                NULL, 0, Pt);
        if (Error == SYMCRYPT_NO_ERROR)
            Error = SymCrypt802_11SaeCustomInitH2E(State, Pt, OwnMac, PeerMac, RandBuffer, MaskBuffer);
        SymCryptWipeKnownSize(Pt, sizeof(Pt));
    }
    else
    {
        Error = SymCrypt802_11SaeCustomInit(State, OwnMac, PeerMac, Password, PasswordLength,
                                            NULL, RandBuffer, MaskBuffer);
    }

    SymCryptWipeKnownSize(RandBuffer, sizeof(RandBuffer));
    SymCryptWipeKnownSize(MaskBuffer, sizeof(MaskBuffer));

    if (Error == SYMCRYPT_NO_ERROR)
        Error = SymCrypt802_11SaeCustomCommitCreate(State, Sae->Scalar, Sae->Element);

    if (Error != SYMCRYPT_NO_ERROR)
    {
        SymCrypt802_11SaeCustomDestroy(State);
        SymCryptCallbackFree(State);
        return FALSE;
    }

    Sae->State = State;
    Sae->HashToElement = HashToElement;
    return TRUE;
}

/**
 * @brief
 * Takes the peer's commit and derives the KCK, the PMK and the PMKID from it.
 */
BOOL
WlanSaeTakeCommit(
    _Inout_ PWLAN_SAE Sae,
    _In_reads_bytes_(WLAN_SAE_SCALAR_LENGTH) const UCHAR *PeerScalar,
    _In_reads_bytes_(WLAN_SAE_ELEMENT_LENGTH) const UCHAR *PeerElement)
{
    UCHAR Secret[WLAN_SAE_SCALAR_LENGTH];
    UCHAR ScalarSum[WLAN_SAE_SCALAR_LENGTH];
    UCHAR Salt[WLAN_SHA256_LENGTH];
    UCHAR KeySeed[WLAN_SHA256_LENGTH];
    UCHAR Keys[WLAN_SAE_KCK_LENGTH + WLAN_SAE_PMK_LENGTH];
    BOOL Ok = FALSE;

    if (Sae->State == NULL)
        return FALSE;

    /* Our own commit sent back is a reflection, not a peer */
    if (RtlEqualMemory(PeerScalar, Sae->Scalar, WLAN_SAE_SCALAR_LENGTH) &&
        RtlEqualMemory(PeerElement, Sae->Element, WLAN_SAE_ELEMENT_LENGTH))
    {
        return FALSE;
    }

    if (SymCrypt802_11SaeCustomCommitProcess(Sae->State, PeerScalar, PeerElement,
                                             Secret, ScalarSum) != SYMCRYPT_NO_ERROR)
    {
        goto Done;
    }

    RtlCopyMemory(Sae->PeerScalar, PeerScalar, WLAN_SAE_SCALAR_LENGTH);
    RtlCopyMemory(Sae->PeerElement, PeerElement, WLAN_SAE_ELEMENT_LENGTH);

    /* keyseed = H(salt, k), with no rejected groups making the salt all zeros,
       then KCK || PMK = KDF(keyseed, "SAE KCK and PMK", scalar sum) */
    RtlZeroMemory(Salt, sizeof(Salt));
    WlanCryptoHmacSha256(Salt, sizeof(Salt), Secret, sizeof(Secret), KeySeed);
    if (!WlanCryptoKdfSha256(KeySeed, sizeof(KeySeed), "SAE KCK and PMK",
                             ScalarSum, sizeof(ScalarSum), Keys, sizeof(Keys)))
    {
        goto Done;
    }

    RtlCopyMemory(Sae->Kck, Keys, WLAN_SAE_KCK_LENGTH);
    RtlCopyMemory(Sae->Pmk, Keys + WLAN_SAE_KCK_LENGTH, WLAN_SAE_PMK_LENGTH);
    RtlCopyMemory(Sae->Pmkid, ScalarSum, WLAN_SAE_PMKID_LENGTH);
    Ok = TRUE;

Done:
    SymCryptWipeKnownSize(Secret, sizeof(Secret));
    SymCryptWipeKnownSize(KeySeed, sizeof(KeySeed));
    SymCryptWipeKnownSize(Keys, sizeof(Keys));
    return Ok;
}

/* confirm = HMAC(KCK, send-confirm || scalar || element || peer scalar || peer element),
   ours or the peer's depending on which side comes first */
static
VOID
SaeConfirmValue(
    _In_ PWLAN_SAE Sae,
    _In_ USHORT SendConfirm,
    _In_ BOOL Ours,
    _Out_writes_bytes_(WLAN_SAE_CONFIRM_LENGTH) PUCHAR Confirm)
{
    SYMCRYPT_HMAC_SHA256_EXPANDED_KEY Key;
    SYMCRYPT_HMAC_SHA256_STATE State;
    UCHAR Counter[2];

    Counter[0] = (UCHAR)SendConfirm;
    Counter[1] = (UCHAR)(SendConfirm >> 8);

    SymCryptHmacSha256ExpandKey(&Key, Sae->Kck, WLAN_SAE_KCK_LENGTH);
    SymCryptHmacSha256Init(&State, &Key);
    SymCryptHmacSha256Append(&State, Counter, sizeof(Counter));
    SymCryptHmacSha256Append(&State, Ours ? Sae->Scalar : Sae->PeerScalar, WLAN_SAE_SCALAR_LENGTH);
    SymCryptHmacSha256Append(&State, Ours ? Sae->Element : Sae->PeerElement, WLAN_SAE_ELEMENT_LENGTH);
    SymCryptHmacSha256Append(&State, Ours ? Sae->PeerScalar : Sae->Scalar, WLAN_SAE_SCALAR_LENGTH);
    SymCryptHmacSha256Append(&State, Ours ? Sae->PeerElement : Sae->Element, WLAN_SAE_ELEMENT_LENGTH);
    SymCryptHmacSha256Result(&State, Confirm);
    SymCryptWipeKnownSize(&Key, sizeof(Key));
}

/**
 * @brief
 * Our confirm for the given send-confirm counter.
 */
VOID
WlanSaeConfirm(
    _In_ PWLAN_SAE Sae,
    _In_ USHORT SendConfirm,
    _Out_writes_bytes_(WLAN_SAE_CONFIRM_LENGTH) PUCHAR Confirm)
{
    SaeConfirmValue(Sae, SendConfirm, TRUE, Confirm);
}

/**
 * @brief
 * Checks the peer's confirm, which proves it knows the password.
 */
BOOL
WlanSaeCheckConfirm(
    _In_ PWLAN_SAE Sae,
    _In_ USHORT PeerSendConfirm,
    _In_reads_bytes_(WLAN_SAE_CONFIRM_LENGTH) const UCHAR *Confirm)
{
    UCHAR Expected[WLAN_SAE_CONFIRM_LENGTH];

    SaeConfirmValue(Sae, PeerSendConfirm, FALSE, Expected);
    return SymCryptEqual(Expected, Confirm, sizeof(Expected));
}

/**
 * @brief
 * Ends the exchange and wipes everything but the PMK and PMKID.
 */
VOID
WlanSaeFinish(
    _Inout_ PWLAN_SAE Sae)
{
    if (Sae->State != NULL)
    {
        SymCrypt802_11SaeCustomDestroy(Sae->State);
        SymCryptCallbackFree(Sae->State);
        Sae->State = NULL;
    }

    SymCryptWipeKnownSize(Sae->Kck, sizeof(Sae->Kck));
}
