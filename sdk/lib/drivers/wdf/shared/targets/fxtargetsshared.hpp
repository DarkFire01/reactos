/*++

Copyright (c) Microsoft Corporation

Module Name:

    corepriv.hpp

Abstract:

    This is the main driver framework.

Author:



Environment:

    Both kernel and user mode

Revision History:

--*/

#pragma once

#if ((FX_CORE_MODE)==(FX_CORE_USER_MODE))
#define FX_IS_USER_MODE (TRUE)
#define FX_IS_KERNEL_MODE (FALSE)
#elif ((FX_CORE_MODE)==(FX_CORE_KERNEL_MODE))
#define FX_IS_USER_MODE (FALSE)
#define FX_IS_KERNEL_MODE (TRUE)
#endif

extern "C" {
#include "mx.h"
}

#include "fxmin.hpp"



#include "wdfmemory.h"
#include "wdfrequest.h"
#include "wdfdevice.h"
#include "wdfwmi.h"
#include "wdfchildlist.h"
#include "wdfpdo.h"
#include "wdffdo.h"
#include "wdfiotarget.h"
#include "wdfcontrol.h"
#include "wdfcx.h"
#include "wdfio.h"
#include "wdfqueryinterface.h"

#include "fxirpqueue.hpp"
#include "fxcallback.hpp"
#if (FX_CORE_MODE == FX_CORE_USER_MODE)
#include "fxirpum.hpp"
#elif ((FX_CORE_MODE)==(FX_CORE_KERNEL_MODE))
#include "fxirpkm.hpp"
#endif
#include "fxtransactionedlist.hpp"

#include "fxcollection.hpp"
#include "fxdeviceinitshared.hpp"
#include "fxdevicetomxinterface.hpp"
#include "fxrequestcontext.hpp"
#include "fxrequestcontexttypes.h"
#include "fxrequestbase.hpp"
#include "fxrequestbuffer.hpp"
#include "ifxmemory.hpp"
#include "fxiotarget.hpp"
#include "fxiotargetremote.hpp"
#include "fxiotargetself.hpp"

#include "fxversionedstructures.h"
