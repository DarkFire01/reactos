/*++

Copyright (c) Microsoft Corporation

Module Name:

    pnppriv.hpp

Abstract:

    This module implements the pnp package for the driver frameworks.

Author:



Environment:

    Both kernel and user mode

Revision History:



--*/

#ifndef _PNPPRIV_H_
#define _PNPPRIV_H_

#if ((FX_CORE_MODE)==(FX_CORE_USER_MODE))
#define FX_IS_USER_MODE (TRUE)
#define FX_IS_KERNEL_MODE (FALSE)
#elif ((FX_CORE_MODE)==(FX_CORE_KERNEL_MODE))
#define FX_IS_USER_MODE (FALSE)
#define FX_IS_KERNEL_MODE (TRUE)
#endif

//
// common header file for all irphandler\* files
//
#include "shared/irphandlers/irphandlerspriv.hpp"

//
// public headers
//
#include "wdfdevice.h"
#include "wdfchildlist.h"
#include "wdfpdo.h"
#include "wdffdo.h"
#include "wdfqueryinterface.h"
#include "wdfmemory.h"
#include "wdfwmi.h"



















#ifndef _INTERRUPT_COMMON_H_
#include "wdfinterrupt.h"
#endif
#ifndef _WUDFDDI_TYPES_PRIVATE_H_
#include "wdfrequest.h"
#include "wdfio.h"
#endif

//
// private headers
//
#include "fxwaitlock.hpp"
#include "fxtransactionedlist.hpp"
#include "fxrelateddevicelist.hpp"
#include "fxcollection.hpp"

// support
#include "stringutil.hpp"
#include "fxstring.hpp"
#include "fxdevicetext.hpp"
#include "fxcallback.hpp"
#include "fxsystemthread.hpp"
#include "fxresource.hpp"

// io
#include "fxpkgioshared.hpp"

//
// FxDeviceInitShared.hpp is new header with definition of PdoInit split from
// FxDeviceInit.hpp
//
#include "fxdeviceinitshared.hpp"

// bus
#include "fxchildlist.hpp"

// FxDevice To Shared interface header
#include "fxdevicetomxinterface.hpp"

// mode specific headers
#if FX_IS_KERNEL_MODE
#include "pnpprivkm.hpp"
#elif FX_IS_USER_MODE
#include "pnpprivum.hpp"
#endif

#include "fxspinlock.hpp"

#if FX_IS_KERNEL_MODE
#include "fxinterruptkm.hpp"
#elif FX_IS_USER_MODE
#include "fxinterruptum.hpp"
#endif

#if FX_IS_KERNEL_MODE
#include "fxperftracekm.hpp"
#endif

#include "fxtelemetry.hpp"

// pnp
#include "fxrelateddevice.hpp"
#include "fxdeviceinterface.hpp"
#include "fxqueryinterface.hpp"
#include "fxpnpcallbacks.hpp"
#include "fxpackage.hpp"
#include "fxpkgpnp.hpp"
#include "fxwatchdog.hpp"
#include "fxpkgpdo.hpp"
#include "fxpkgfdo.hpp"

// wmi
#include "fxwmiirphandler.hpp"
#include "fxwmiprovider.hpp"
#include "fxwmiinstance.hpp"


VOID
PnpPassThroughQIWorker(
    __in    MxDeviceObject* Device,
    __inout FxIrp* Irp,
    __inout FxIrp* ForwardIrp
    );

VOID
CopyQueryInterfaceToIrpStack(
    __in PPOWER_THREAD_INTERFACE PowerThreadInterface,
    __in FxIrp* Irp
    );

_Must_inspect_result_
NTSTATUS
NTAPI
SendDeviceUsageNotification(
    __in MxDeviceObject* RelatedDevice,
    __in FxIrp* RelatedIrp,
    __in MxWorkItem* Workitem,
    __in FxIrp* OriginalIrp,
    __in BOOLEAN Revert
    );

_Must_inspect_result_
NTSTATUS
NTAPI
GetStackCapabilities(
    __in PFX_DRIVER_GLOBALS DriverGlobals,
    __in MxDeviceObject* DeviceInStack,
    __in_opt PD3COLD_SUPPORT_INTERFACE D3ColdInterface,
    __out PSTACK_DEVICE_CAPABILITIES Capabilities
    );

VOID
NTAPI
SetD3ColdSupport(
    __in PFX_DRIVER_GLOBALS DriverGlobals,
    __in MxDeviceObject* DeviceInStack,
    __in PD3COLD_SUPPORT_INTERFACE D3ColdInterface,
    __in BOOLEAN UseD3Cold
    );

#define SET_TRI_STATE_FROM_STATE_BITS(state, S, FieldName)      \
{                                                               \
    switch (state & (FxPnpState##FieldName##Mask)) {            \
    case FxPnpState##FieldName##False :                         \
        S->FieldName =  WdfFalse;                               \
        break;                                                  \
    case FxPnpState##FieldName##True:                           \
        S->FieldName =  WdfTrue;                                \
        break;                                                  \
    case FxPnpState##FieldName##UseDefault :                    \
    default:                                                    \
        S->FieldName =  WdfUseDefault;                          \
        break;                                                  \
    }                                                           \
}

LONG
__inline
FxGetValueBits(
    __in WDF_TRI_STATE State,
    __in LONG TrueValue,
    __in LONG UseDefaultValue
    )
{
    switch (State) {
    case WdfFalse:      return 0x0;
    case WdfTrue:       return TrueValue;
    case WdfUseDefault:
    default:            return UseDefaultValue;
    }
}

#define GET_PNP_STATE_BITS_FROM_STRUCT(S, FieldName)\
    FxGetValueBits(S->FieldName,                    \
                 FxPnpState##FieldName##True,       \
                 FxPnpState##FieldName##UseDefault)

#define GET_PNP_CAP_BITS_FROM_STRUCT(S, FieldName)  \
    FxGetValueBits(S->FieldName,                    \
                 FxPnpCap##FieldName##True,         \
                 FxPnpCap##FieldName##UseDefault)

#define GET_POWER_CAP_BITS_FROM_STRUCT(S, FieldName)\
    FxGetValueBits(S->FieldName,                    \
                   FxPowerCap##FieldName##True,     \
                   FxPowerCap##FieldName##UseDefault)

__inline
VOID
FxSetPnpDeviceStateBit(
    __in PNP_DEVICE_STATE* PnpDeviceState,
    __in LONG ExternalState,
    __in LONG InternalState,
    __in LONG BitMask,
    __in LONG TrueValue
    )
{
    LONG bits;

    bits = InternalState & BitMask;

    if (bits == TrueValue) {
        *PnpDeviceState |= ExternalState;
    }
    else if (bits == 0) {  // 0 is the always false for every bit-set
        *PnpDeviceState &= ~ExternalState;
    }
}

#define SET_PNP_DEVICE_STATE_BIT(State, ExternalState, value, Name) \
    FxSetPnpDeviceStateBit(State,                                   \
                           ExternalState,                           \
                           state,                                   \
                           FxPnpState##Name##Mask,                  \
                           FxPnpState##Name##True)

#define SET_PNP_CAP_IF_TRUE(caps, pCaps, FieldName)                         \
{                                                                           \
    if ((caps & FxPnpCap##FieldName##Mask) == FxPnpCap##FieldName##True) {  \
        pCaps->FieldName = TRUE;                                            \
    }                                                                       \
}

#define SET_PNP_CAP_IF_FALSE(caps, pCaps, FieldName)                        \
{                                                                           \
    if ((caps & FxPnpCap##FieldName##Mask) == FxPnpCap##FieldName##False) { \
        pCaps->FieldName = FALSE;                                           \
    }                                                                       \
}

#define SET_PNP_CAP(caps, pCaps, FieldName)                                 \
{                                                                           \
    if ((caps & FxPnpCap##FieldName##Mask) == FxPnpCap##FieldName##False) { \
        pCaps->FieldName = FALSE;                                           \
    }                                                                       \
    else if ((caps & FxPnpCap##FieldName##Mask) == FxPnpCap##FieldName##True) {  \
        pCaps->FieldName = TRUE;                                            \
    }                                                                       \
}

#define SET_POWER_CAP(caps, pCaps, FieldName)                               \
{                                                                           \
    if ((caps & FxPowerCap##FieldName##Mask) == FxPowerCap##FieldName##False) { \
        pCaps->FieldName = FALSE;                                           \
    }                                                                       \
    else if ((caps & FxPowerCap##FieldName##Mask) == FxPowerCap##FieldName##True) {  \
        pCaps->FieldName = TRUE;                                            \
    }                                                                       \
}

_Must_inspect_result_
PVOID
GetIoMgrObjectForWorkItemAllocation(
    VOID
    );

#include "fxversionedstructures.h"

#endif // _PNPPRIV_H_
