/*
 * PROJECT:     ReactOS Qualcomm audio (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cellular call audio controls on the wave and topology filters
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The telephony service drives a call through two property sets. On the wave
 * filter it reads the call state, turns the call path on and off, holds it,
 * moves it between providers and mutes the uplink. On the topology filter it
 * picks the render and capture endpoints the call uses and its volume. One
 * modem means one provider and one call path, so the state lives here. The
 * lock is a kernel mutex since the call path talks to the dsp at PASSIVE_LEVEL.
 */

#include "precomp.h"

#define QCA_LOG(...) DbgPrint("[qcomaudio] " __VA_ARGS__)

typedef struct _QCA_CALL
{
    KMUTEX Lock;
    TELEPHONY_CALLTYPE CallType;
    TELEPHONY_CALLSTATE CallState;
    TELEPHONY_CALLSTATE StateBeforeChange;
    BOOL MuteTx;
    LONG Volume;
    BOOLEAN HavePair;
    KSTOPOLOGY_ENDPOINTIDPAIR Pair;
} QCA_CALL, *PQCA_CALL;

static QCA_CALL QcaCall;

#ifdef _MSC_VER
#pragma code_seg("PAGE")
#endif

VOID
NTAPI
TelephonyInitialize(VOID)
{
    PAGED_CODE();

    RtlZeroMemory(&QcaCall, sizeof(QcaCall));
    KeInitializeMutex(&QcaCall.Lock, 0);
    QcaCall.CallType = TELEPHONY_CALLTYPE_CIRCUITSWITCHED;
    QcaCall.CallState = TELEPHONY_CALLSTATE_DISABLED;
    QcaCall.Volume = QCA_VOLUME_MAX;
}

/* Starts the call audio of the modem on the dsp, toward the board call ports */
static
NTSTATUS
TelephonyStartPath(
    _In_ TELEPHONY_CALLTYPE CallType)
{
    NTSTATUS Status;

    PAGED_CODE();

    /* Only the modem's circuit switched voice session is driven so far */
    if (CallType != TELEPHONY_CALLTYPE_CIRCUITSWITCHED)
    {
        QCA_LOG("call audio for call type %u not available\n", CallType);
        return STATUS_NOT_SUPPORTED;
    }

    QcQ6VoiceSetTxMute(QcaCall.MuteTx ? TRUE : FALSE, 0);
    Status = QcQ6VoiceStart();
    if (!NT_SUCCESS(Status))
        QCA_LOG("dsp voice session did not start 0x%08lx\n", Status);
    return Status;
}

static
VOID
TelephonyStopPath(VOID)
{
    PAGED_CODE();

    QcQ6VoiceStop();
}

NTSTATUS
NTAPI
WavePropertyTelephony(PPCPROPERTY_REQUEST Request)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Need;

    PAGED_CODE();

    if (Request->Node != MAXULONG)
        return STATUS_INVALID_PARAMETER;

    switch (Request->PropertyItem->Id)
    {
        case KSPROPERTY_TELEPHONY_PROVIDERID:
            Need = sizeof(ULONG);
            break;
        case KSPROPERTY_TELEPHONY_CALLINFO:
            Need = sizeof(KSTELEPHONY_CALLINFO);
            break;
        case KSPROPERTY_TELEPHONY_CALLCONTROL:
            Need = sizeof(KSTELEPHONY_CALLCONTROL);
            break;
        case KSPROPERTY_TELEPHONY_PROVIDERCHANGE:
            Need = sizeof(KSTELEPHONY_PROVIDERCHANGE);
            break;
        case KSPROPERTY_TELEPHONY_CALLHOLD:
        case KSPROPERTY_TELEPHONY_MUTE_TX:
            Need = sizeof(BOOL);
            break;
        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Request->ValueSize < Need)
    {
        Request->ValueSize = Need;
        return STATUS_BUFFER_TOO_SMALL;
    }

    KeWaitForSingleObject(&QcaCall.Lock, Executive, KernelMode, FALSE, NULL);

    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        switch (Request->PropertyItem->Id)
        {
            case KSPROPERTY_TELEPHONY_PROVIDERID:
                *(PULONG)Request->Value = 0;
                break;

            case KSPROPERTY_TELEPHONY_CALLINFO:
            {
                PKSTELEPHONY_CALLINFO Info = (PKSTELEPHONY_CALLINFO)Request->Value;
                Info->CallType = QcaCall.CallType;
                Info->CallState = QcaCall.CallState;
                break;
            }

            case KSPROPERTY_TELEPHONY_MUTE_TX:
                *(PBOOL)Request->Value = QcaCall.MuteTx;
                break;

            default:
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }

        if (NT_SUCCESS(Status))
            Request->ValueSize = Need;
    }
    else if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        switch (Request->PropertyItem->Id)
        {
            case KSPROPERTY_TELEPHONY_CALLCONTROL:
            {
                PKSTELEPHONY_CALLCONTROL Control = (PKSTELEPHONY_CALLCONTROL)Request->Value;

                if (Control->CallControlOp == TELEPHONY_CALLCONTROLOP_ENABLE)
                {
                    if (QcaCall.CallState != TELEPHONY_CALLSTATE_DISABLED)
                    {
                        Status = STATUS_INVALID_DEVICE_STATE;
                        break;
                    }

                    Status = TelephonyStartPath(Control->CallType);
                    if (NT_SUCCESS(Status))
                    {
                        QcaCall.CallType = Control->CallType;
                        QcaCall.CallState = TELEPHONY_CALLSTATE_ENABLED;
                    }
                }
                else if (Control->CallControlOp == TELEPHONY_CALLCONTROLOP_DISABLE)
                {
                    if (QcaCall.CallState != TELEPHONY_CALLSTATE_DISABLED)
                        TelephonyStopPath();
                    QcaCall.CallState = TELEPHONY_CALLSTATE_DISABLED;
                    QcaCall.MuteTx = FALSE;
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }

                QCA_LOG("call control op %u type %u: 0x%08lx, state %u\n",
                        Control->CallControlOp, Control->CallType, Status, QcaCall.CallState);
                break;
            }

            case KSPROPERTY_TELEPHONY_PROVIDERCHANGE:
            {
                PKSTELEPHONY_PROVIDERCHANGE Change = (PKSTELEPHONY_PROVIDERCHANGE)Request->Value;

                /* A handover keeps the path up while the call type moves to the new provider */
                if (Change->ProviderChangeOp == TELEPHONY_PROVIDERCHANGEOP_BEGIN)
                {
                    if (QcaCall.CallState == TELEPHONY_CALLSTATE_DISABLED)
                    {
                        Status = STATUS_INVALID_DEVICE_STATE;
                        break;
                    }
                    QcaCall.StateBeforeChange = QcaCall.CallState;
                    QcaCall.CallState = TELEPHONY_CALLSTATE_PROVIDERTRANSITION;
                }
                else if (Change->ProviderChangeOp == TELEPHONY_PROVIDERCHANGEOP_END)
                {
                    if (QcaCall.CallState != TELEPHONY_CALLSTATE_PROVIDERTRANSITION)
                    {
                        Status = STATUS_INVALID_DEVICE_STATE;
                        break;
                    }
                    QcaCall.CallType = Change->CallType;
                    QcaCall.CallState = QcaCall.StateBeforeChange;
                }
                else if (Change->ProviderChangeOp == TELEPHONY_PROVIDERCHANGEOP_CANCEL)
                {
                    if (QcaCall.CallState == TELEPHONY_CALLSTATE_PROVIDERTRANSITION)
                        QcaCall.CallState = QcaCall.StateBeforeChange;
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
                break;
            }

            case KSPROPERTY_TELEPHONY_CALLHOLD:
                if (QcaCall.CallState == TELEPHONY_CALLSTATE_ENABLED && *(PBOOL)Request->Value)
                    QcaCall.CallState = TELEPHONY_CALLSTATE_HOLD;
                else if (QcaCall.CallState == TELEPHONY_CALLSTATE_HOLD && !*(PBOOL)Request->Value)
                    QcaCall.CallState = TELEPHONY_CALLSTATE_ENABLED;
                else
                    Status = STATUS_INVALID_DEVICE_STATE;
                break;

            case KSPROPERTY_TELEPHONY_MUTE_TX:
                QcaCall.MuteTx = *(PBOOL)Request->Value ? TRUE : FALSE;
                Status = QcQ6VoiceSetTxMute(QcaCall.MuteTx ? TRUE : FALSE, 0);
                break;

            default:
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
        }
    }
    else
    {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    KeReleaseMutex(&QcaCall.Lock, FALSE);
    return Status;
}

NTSTATUS
NTAPI
TopoPropertyTelephony(PPCPROPERTY_REQUEST Request)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Need;

    PAGED_CODE();

    if (Request->Node != MAXULONG)
        return STATUS_INVALID_PARAMETER;

    if (Request->PropertyItem->Id == KSPROPERTY_TELEPHONY_ENDPOINTIDPAIR)
        Need = sizeof(KSTOPOLOGY_ENDPOINTIDPAIR);
    else if (Request->PropertyItem->Id == KSPROPERTY_TELEPHONY_VOLUME)
        Need = sizeof(LONG);
    else
        return STATUS_INVALID_DEVICE_REQUEST;

    if ((Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) &&
        Request->PropertyItem->Id == KSPROPERTY_TELEPHONY_VOLUME)
    {
        return TopoBasicSupport(Request, VT_I4, TRUE);
    }

    if (Request->ValueSize < Need)
    {
        Request->ValueSize = Need;
        return STATUS_BUFFER_TOO_SMALL;
    }

    KeWaitForSingleObject(&QcaCall.Lock, Executive, KernelMode, FALSE, NULL);

    if (Request->PropertyItem->Id == KSPROPERTY_TELEPHONY_ENDPOINTIDPAIR)
    {
        PKSTOPOLOGY_ENDPOINTIDPAIR Pair = (PKSTOPOLOGY_ENDPOINTIDPAIR)Request->Value;

        if (Request->Verb & KSPROPERTY_TYPE_GET)
        {
            if (QcaCall.HavePair)
            {
                *Pair = QcaCall.Pair;
            }
            else
            {
                /* The speaker and the microphone until the service picks */
                RtlZeroMemory(Pair, sizeof(*Pair));
                Pair->RenderEndpoint.PinId = PIN_TOPO_SPEAKER;
                Pair->CaptureEndpoint.PinId = PIN_TOPO_MICROPHONE;
            }
            Request->ValueSize = Need;
        }
        else if (Request->Verb & KSPROPERTY_TYPE_SET)
        {
            if (Pair->RenderEndpoint.PinId != PIN_TOPO_SPEAKER ||
                Pair->CaptureEndpoint.PinId != PIN_TOPO_MICROPHONE)
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            else
            {
                QcaCall.Pair = *Pair;
                QcaCall.HavePair = TRUE;
            }
            QCA_LOG("call endpoints render pin %lu capture pin %lu: 0x%08lx\n",
                    Pair->RenderEndpoint.PinId, Pair->CaptureEndpoint.PinId, Status);
        }
        else
        {
            Status = STATUS_INVALID_DEVICE_REQUEST;
        }
    }
    else
    {
        if (Request->Verb & KSPROPERTY_TYPE_GET)
        {
            *(PLONG)Request->Value = QcaCall.Volume;
            Request->ValueSize = Need;
        }
        else if (Request->Verb & KSPROPERTY_TYPE_SET)
        {
            LONG Level = *(PLONG)Request->Value;

            if (Level < QCA_VOLUME_MIN)
                Level = QCA_VOLUME_MIN;
            if (Level > QCA_VOLUME_MAX)
                Level = QCA_VOLUME_MAX;
            QcaCall.Volume = Level;
        }
        else
        {
            Status = STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    KeReleaseMutex(&QcaCall.Lock, FALSE);
    return Status;
}

#ifdef _MSC_VER
#pragma code_seg()
#endif
