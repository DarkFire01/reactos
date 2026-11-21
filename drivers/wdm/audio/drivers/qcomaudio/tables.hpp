/*
 * PROJECT:     ReactOS Qualcomm audio (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pin, node and filter descriptors for the wave and topology miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _QCOMAUDIO_TABLES_HPP_
#define _QCOMAUDIO_TABLES_HPP_

#define QCA_MAX_CHANNELS            2
#define QCA_MIN_BITS                16
#define QCA_MAX_BITS                16
#define QCA_MIN_RATE                8000
#define QCA_MAX_RATE                48000

static KSDATARANGE_AUDIO WaveRenderPcmRange[] =
{
    {
        {
            {
                sizeof(KSDATARANGE_AUDIO),
                0,
                0,
                0,
                {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
                {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)},
                {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}
            }
        },
        QCA_MAX_CHANNELS,
        QCA_MIN_BITS,
        QCA_MAX_BITS,
        QCA_MIN_RATE,
        QCA_MAX_RATE
    }
};

static PKSDATARANGE WaveRenderPcmRangePtr[] =
{
    PKSDATARANGE(&WaveRenderPcmRange[0])
};

static KSDATARANGE WaveRenderBridgeRange[] =
{
    {
        {
            sizeof(KSDATARANGE),
            0,
            0,
            0,
            {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
            {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
            {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
        }
    }
};

static PKSDATARANGE WaveRenderBridgeRangePtr[] =
{
    &WaveRenderBridgeRange[0]
};

static PCPIN_DESCRIPTOR WaveMiniportPins[] =
{
    // PIN_WAVE_RENDER_SINK
    {
        1, 1, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(WaveRenderPcmRangePtr),
            WaveRenderPcmRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_VOLUME_CONTROL,
            {0}
        }
    },
    // PIN_WAVE_RENDER_SOURCE
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(WaveRenderBridgeRangePtr),
            WaveRenderBridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            {0}
        }
    }
};

static PCNODE_DESCRIPTOR WaveMiniportNodes[] =
{
    // NODE_WAVE_DAC
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

static PCCONNECTION_DESCRIPTOR WaveMiniportConnections[] =
{
    { PCFILTER_NODE,   PIN_WAVE_RENDER_SINK,   NODE_WAVE_DAC,  1                      },
    { NODE_WAVE_DAC,   0,                      PCFILTER_NODE,  PIN_WAVE_RENDER_SOURCE }
};

static PCPROPERTY_ITEM WaveFilterProperties[] =
{
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_PROVIDERID,
      PCPROPERTY_ITEM_FLAG_GET, WavePropertyTelephony },
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_CALLINFO,
      PCPROPERTY_ITEM_FLAG_GET, WavePropertyTelephony },
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_CALLCONTROL,
      PCPROPERTY_ITEM_FLAG_SET, WavePropertyTelephony },
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_PROVIDERCHANGE,
      PCPROPERTY_ITEM_FLAG_SET, WavePropertyTelephony },
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_CALLHOLD,
      PCPROPERTY_ITEM_FLAG_SET, WavePropertyTelephony },
    { &KSPROPSETID_TelephonyControl, KSPROPERTY_TELEPHONY_MUTE_TX,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET, WavePropertyTelephony }
};

DEFINE_PCAUTOMATION_TABLE_PROP(WaveFilterAutomation, WaveFilterProperties);

static PCFILTER_DESCRIPTOR WaveMiniportFilterDescriptor =
{
    0,
    &WaveFilterAutomation,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(WaveMiniportPins),
    WaveMiniportPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(WaveMiniportNodes),
    WaveMiniportNodes,
    SIZEOF_ARRAY(WaveMiniportConnections),
    WaveMiniportConnections,
    0,
    NULL
};

static KSDATARANGE TopoBridgeRange[] =
{
    {
        {
            sizeof(KSDATARANGE),
            0,
            0,
            0,
            {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
            {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
            {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
        }
    }
};

static PKSDATARANGE TopoBridgeRangePtr[] =
{
    &TopoBridgeRange[0]
};

static PCPIN_DESCRIPTOR TopoMiniportPins[] =
{
    // PIN_TOPO_WAVE_IN
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(TopoBridgeRangePtr),
            TopoBridgeRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            {0}
        }
    },
    // PIN_TOPO_SPEAKER
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(TopoBridgeRangePtr),
            TopoBridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            {0}
        }
    },
    // PIN_TOPO_MICROPHONE
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(TopoBridgeRangePtr),
            TopoBridgeRangePtr,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            NULL,
            {0}
        }
    },
    // PIN_TOPO_CAPTURE_OUT
    {
        0, 0, 0,
        NULL,
        {
            0, NULL, 0, NULL,
            SIZEOF_ARRAY(TopoBridgeRangePtr),
            TopoBridgeRangePtr,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            {0}
        }
    }
};

static PCPROPERTY_ITEM TopoFilterProperties[] =
{
    { &KSPROPSETID_TelephonyTopology, KSPROPERTY_TELEPHONY_ENDPOINTIDPAIR,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET, TopoPropertyTelephony },
    { &KSPROPSETID_TelephonyTopology, KSPROPERTY_TELEPHONY_VOLUME,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, TopoPropertyTelephony }
};

DEFINE_PCAUTOMATION_TABLE_PROP(TopoFilterAutomation, TopoFilterProperties);

static PCPROPERTY_ITEM TopoVolumeProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropertyVolume
    }
};

static PCPROPERTY_ITEM TopoMuteProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        TopoPropertyMute
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(TopoVolumeAutomation, TopoVolumeProperties);
DEFINE_PCAUTOMATION_TABLE_PROP(TopoMuteAutomation, TopoMuteProperties);

static PCNODE_DESCRIPTOR TopoMiniportNodes[] =
{
    // NODE_TOPO_VOLUME
    { 0, &TopoVolumeAutomation, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME },
    // NODE_TOPO_MUTE
    { 0, &TopoMuteAutomation, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE }
};

static PCCONNECTION_DESCRIPTOR TopoMiniportConnections[] =
{
    { PCFILTER_NODE,    PIN_TOPO_WAVE_IN,   NODE_TOPO_VOLUME,   1                },
    { NODE_TOPO_VOLUME, 0,                  NODE_TOPO_MUTE,     1                },
    { NODE_TOPO_MUTE,   0,                  PCFILTER_NODE,      PIN_TOPO_SPEAKER },
    { PCFILTER_NODE,    PIN_TOPO_MICROPHONE, PCFILTER_NODE,     PIN_TOPO_CAPTURE_OUT }
};

static PCFILTER_DESCRIPTOR TopoMiniportFilterDescriptor =
{
    0,
    &TopoFilterAutomation,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(TopoMiniportPins),
    TopoMiniportPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(TopoMiniportNodes),
    TopoMiniportNodes,
    SIZEOF_ARRAY(TopoMiniportConnections),
    TopoMiniportConnections,
    0,
    NULL
};

#endif /* _QCOMAUDIO_TABLES_HPP_ */
