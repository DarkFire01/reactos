/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RPMh regulator interface (qcomrpmh.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * A voltage managed regulator (VRM) resource behind RPMh. The selector is
 * computed from the linear range the PMIC hardware exposes, so a caller only
 * needs the resource name from the command database and its range.
 */
typedef struct _QCOM_RPMH_VRM_REQUEST
{
    PCSTR Resource;
    ULONG TargetMicrovolts;
    ULONG MinMicrovolts;
    ULONG StepMicrovolts;
    ULONG Mode;
} QCOM_RPMH_VRM_REQUEST, *PQCOM_RPMH_VRM_REQUEST;

typedef const QCOM_RPMH_VRM_REQUEST *PCQCOM_RPMH_VRM_REQUEST;

/*
 * Sets the voltage and mode of a VRM regulator and turns it on through one
 * active set command on the applications RSC. Returns STATUS_NO_SUCH_DEVICE
 * when the resource is not in the command database.
 */
NTSTATUS
NTAPI
QcRpmhEnableRegulator(
    _In_ PCQCOM_RPMH_VRM_REQUEST Request);
