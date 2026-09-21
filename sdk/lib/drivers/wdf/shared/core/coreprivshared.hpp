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
// #include "wdfdevicepri.h"
// #include "wdfiotargetpri.h"
#include "wdfwmi.h"
#include "wdfchildlist.h"
#include "wdfpdo.h"
#include "wdffdo.h"
#include "wdfiotarget.h"
#include "wdfcontrol.h"
#include "wdfcx.h"
#include "wdfio.h"
#include "wdfqueryinterface.h"
// #include "wdftriage.h"

//
// Companion headers
//
#if (FX_CORE_MODE == FX_CORE_USER_MODE)
#include "wdfcompanion.h"
#endif

#if (FX_CORE_MODE == FX_CORE_KERNEL_MODE)
#include "wdfcompaniontarget.h"
#endif

#if (FX_CORE_MODE == FX_CORE_USER_MODE)
#include "fxirpum.hpp"
#else
#include "fxirpkm.hpp"
#endif

// <FxSystemWorkItem.hpp>
typedef
VOID
(*PFN_WDF_SYSTEMWORKITEM) (
    IN PVOID             Parameter
    );

#include "fxirpqueue.hpp"


// </FxSystemWorkItem.hpp>


#include "fxprobeandlock.h"
#include "fxpackage.hpp"
#include "fxcollection.hpp"
#include "fxdeviceinitshared.hpp"

#include "ifxmemory.hpp"
#include "fxcallback.hpp"
#include "fxrequestcontext.hpp"
#include "fxrequestcontexttypes.h"
#include "fxrequestbase.hpp"
#include "fxmemoryobject.hpp"
#include "fxmemorybuffer.hpp"

#include "fxmemorybufferfrompool.hpp"

#include "fxmemorybufferpreallocated.hpp"

#include "fxtransactionedlist.hpp"

//
// MERGE temp: We may not need these include files here,
// temporarily including them to verify they compile in shared code
//
#include "fxrequestvalidatefunctions.hpp"
#include "fxrequestcallbacks.hpp"

// support
#include "stringutil.hpp"
#include "fxautostring.hpp"
#include "fxstring.hpp"
#include "fxdevicetext.hpp"
#include "fxcallback.hpp"
#include "fxdisposelist.hpp"
#include "fxsystemthread.hpp"

#include "fxirppreprocessinfo.hpp"
#include "fxpnpcallbacks.hpp"

// device init
#include "fxcxdeviceinit.hpp"
#include "fxcxdeviceinfo.hpp"
#include "fxdeviceinit.hpp"

#include "fxdevicetomxinterface.hpp"

// request
#include "fxrequestmemory.hpp"
#include "fxrequest.hpp"
#include "fxrequestbuffer.hpp"
#include "fxsyncrequest.hpp"

// io target
#include "fxiotarget.hpp"
#include "fxiotargetself.hpp"

#include "fxsystemworkitem.hpp"
#include "fxcallbackmutexlock.hpp"
#include "fxdriver.hpp"

#include "fxdeviceinterface.hpp"
#include "fxqueryinterface.hpp"

#include "fxcallbackspinlock.hpp"
#include "fxdefaultirphandler.hpp"
#include "fxwmiirphandler.hpp"

// packages
#include "fxpkgio.hpp"
#include "fxpkgpnp.hpp"
#include "fxpkgfdo.hpp"
#include "fxpkgpdo.hpp"
#include "fxpkggeneral.hpp"
#include "fxfileobject.hpp"
#include "fxioqueue.hpp"
#include "fxdevice.hpp"
#include "fxtelemetry.hpp"

#include "fxchildlist.hpp"

#include "fxlookasidelist.hpp"

/*#if FX_IS_KERNEL_MODE
#include "wdfrequest.h"
#endif*/

#include "fxversionedstructures.h"
