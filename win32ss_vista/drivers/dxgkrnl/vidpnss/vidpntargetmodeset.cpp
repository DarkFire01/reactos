/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VidPn Target mode Interface
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <dxgkrnl.h>
//#define NDEBUG
#include <debug.h>

NTSTATUS
APIENTRY
DxgkVidPnAcquireTargetModeSet(IN_CONST_D3DKMDT_HVIDPN                                  hVidPn,
                              IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID                  VidPnTargetId,
                              OUT_PD3DKMDT_HVIDPNTARGETMODESET                         phVidPnTargetModeSet,
                              DEREF_OUT_CONST_PPDXGK_VIDPNTARGETMODESET_INTERFACE      ppVidPnTargetModeSetInterface)
{
    REACTOS_HVIDPN_TARGET_MODE hTargetModeSet;
    DPRINT1("DxgkVidPnAcquireTargetModeSet: Entry\n");
    *phVidPnTargetModeSet = (D3DKMDT_HVIDPNTARGETMODESET)&hTargetModeSet;

    //DXGK_VIDPNTARGETMODESET_INTERFACE* TargetModeSetInterface = *ppVidPnTargetModeSetInterface;
    __debugbreak();
    return 0;
}
