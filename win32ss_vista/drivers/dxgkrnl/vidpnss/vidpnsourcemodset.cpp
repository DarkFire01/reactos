/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VidPn Source Mode interface
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <dxgkrnl.h>
//#define NDEBUG
#include <debug.h>

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetGETNUMMODES(IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
                                      OUT_PSIZE_T_CONST                             pNumSourceModes)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetACQUIREFIRSTMODEINFO(IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
                                               DEREF_OUT_CONST_PPD3DKMDT_VIDPN_SOURCE_MODE   ppFirstVidPnSourceModeInfo)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetACQUIRENEXTMODEINFO(
    IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
    IN_CONST_PD3DKMDT_VIDPN_SOURCE_MODE_CONST     pVidPnSourceModeInfo,
    DEREF_OUT_CONST_PPD3DKMDT_VIDPN_SOURCE_MODE   ppNextVidPnSourceModeInfo)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetACQUIREPINNEDMODEINFO(
    IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
    DEREF_OUT_CONST_PPD3DKMDT_VIDPN_SOURCE_MODE  ppPinnedVidPnSourceModeInfo)
{
    DPRINT1("FIXME: We do NOT support preenumeration of video modes!\n");
    *ppPinnedVidPnSourceModeInfo = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetRELEASEMODEINFO(
    IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
    IN_CONST_PD3DKMDT_VIDPN_SOURCE_MODE_CONST     pVidPnSourceModeInfo)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetCREATENEWMODEINFO(IN_CONST_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
                                            DEREF_OUT_PPD3DKMDT_VIDPN_SOURCE_MODE         ppNewVidPnSourceModeInfo)
{
    UNIMPLEMENTED;
    __debugbreak();

    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSourceModeSetAddMode(
    IN_D3DKMDT_HVIDPNSOURCEMODESET          hVidPnSourceModeSet,
    IN_PD3DKMDT_VIDPN_SOURCE_MODE_CONST     pVidPnSourceModeInfo)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkDdiVidPnSOURCEMODESET_PINMODE(
    IN_D3DKMDT_HVIDPNSOURCEMODESET                 hVidPnSourceModeSet,
    IN_CONST_D3DKMDT_VIDEO_PRESENT_SOURCE_MODE_ID  VidPnSourceModeId)
{
    UNIMPLEMENTED;
    __debugbreak();
    return 0;
}

NTSTATUS
APIENTRY
DxgkVidPnAcquireSourceModeSet(IN_CONST_D3DKMDT_HVIDPN                                hVidPn,
                              IN_CONST_D3DDDI_VIDEO_PRESENT_SOURCE_ID                VidPnSourceId,
                              OUT_PD3DKMDT_HVIDPNSOURCEMODESET                       phVidPnSourceModeSet,
                              DEREF_OUT_CONST_PPDXGK_VIDPNSOURCEMODESET_INTERFACE    ppVidPnSourceModeSetInterface)
{
    REACTOS_HVIDPN_SOURCE_MODE VidPnSourceHandle = {0};
    DPRINT1("DxgkVidPnAcquireSourceModeSet: Entry\n");
    *phVidPnSourceModeSet = (D3DKMDT_HVIDPNSOURCEMODESET)&VidPnSourceHandle;
    //DXGK_VIDPNSOURCEMODESET_INTERFACE SourceModeSetInterface = {0};
    #if 0
    PDXGK_VIDPNSOURCEMODESET_INTERFACE SourceModeSetInterface = *ppVidPnSourceModeSetInterface;
    SourceModeSetInterface.pfnGetNumModes = DxgkDdiVidPnSourceModeSetGETNUMMODES;
    SourceModeSetInterface.pfnAcquireFirstModeInfo = DxgkDdiVidPnSourceModeSetACQUIREFIRSTMODEINFO;
    SourceModeSetInterface.pfnAcquireNextModeInfo = DxgkDdiVidPnSourceModeSetACQUIRENEXTMODEINFO;
    SourceModeSetInterface.pfnAcquirePinnedModeInfo = DxgkDdiVidPnSourceModeSetACQUIREPINNEDMODEINFO;
    SourceModeSetInterface.pfnReleaseModeInfo = DxgkDdiVidPnSourceModeSetRELEASEMODEINFO;
    SourceModeSetInterface.pfnCreateNewModeInfo = DxgkDdiVidPnSourceModeSetCREATENEWMODEINFO;
    SourceModeSetInterface.pfnAddMode = DxgkDdiVidPnSourceModeSetAddMode;
    SourceModeSetInterface.pfnPinMode = DxgkDdiVidPnSourceModeSetPINMODE;
    #endif
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
DxgkVidPnCreateNewSourceModeSet(IN_CONST_D3DKMDT_HVIDPN hVidPn,
                                IN_CONST_D3DDDI_VIDEO_PRESENT_SOURCE_ID                VidPnSourceId,
                                OUT_PD3DKMDT_HVIDPNSOURCEMODESET                       phNewVidPnSourceModeSet,
                                DEREF_OUT_CONST_PPDXGK_VIDPNSOURCEMODESET_INTERFACE    ppVidPnSourceModeSetInterface)
{
    DPRINT1("DxgkVidPnReleaseSourceModeSet: Entry\n");
    REACTOS_HVIDPN_SOURCE_MODE VidPnSourceHandle = {0};
    DPRINT1("DxgkVidPnAcquireSourceModeSet: Entry\n");
   // *phVidPnSourceModeSet = (D3DKMDT_HVIDPNSOURCEMODESET)&VidPnSourceHandle;
    //DXGK_VIDPNSOURCEMODESET_INTERFACE SourceModeSetInterface = {0};
    #if 0
    PDXGK_VIDPNSOURCEMODESET_INTERFACE SourceModeSetInterface = *ppVidPnSourceModeSetInterface;
    SourceModeSetInterface.pfnGetNumModes = DxgkDdiVidPnSourceModeSetGETNUMMODES;
    SourceModeSetInterface.pfnAcquireFirstModeInfo = DxgkDdiVidPnSourceModeSetACQUIREFIRSTMODEINFO;
    SourceModeSetInterface.pfnAcquireNextModeInfo = DxgkDdiVidPnSourceModeSetACQUIRENEXTMODEINFO;
    SourceModeSetInterface.pfnAcquirePinnedModeInfo = DxgkDdiVidPnSourceModeSetACQUIREPINNEDMODEINFO;
    SourceModeSetInterface.pfnReleaseModeInfo = DxgkDdiVidPnSourceModeSetRELEASEMODEINFO;
    SourceModeSetInterface.pfnCreateNewModeInfo = DxgkDdiVidPnSourceModeSetCREATENEWMODEINFO;
    SourceModeSetInterface.pfnAddMode = DxgkDdiVidPnSourceModeSetAddMode;
    SourceModeSetInterface.pfnPinMode = DxgkDdiVidPnSourceModeSetPINMODE;
    #endif
    return 0;
}