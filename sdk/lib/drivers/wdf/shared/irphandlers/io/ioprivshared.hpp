/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    iopriv.hpp

Abstract:

    This module defines private interfaces for the I/O package

Author:



Environment:

    Both kernel and user mode

Revision History:

--*/

#ifndef _IOPRIV_H_
#define _IOPRIV_H_

#if ((FX_CORE_MODE)==(FX_CORE_USER_MODE))
#define FX_IS_USER_MODE (TRUE)
#define FX_IS_KERNEL_MODE (FALSE)
#elif ((FX_CORE_MODE)==(FX_CORE_KERNEL_MODE))
#define FX_IS_USER_MODE (FALSE)
#define FX_IS_KERNEL_MODE (TRUE)
#endif

/*#if defined(MODE_AGNSOTIC_FXPKGIO_NOT_IN_SHARED_FOLDER)
#include <fx.hpp>
#else
// common header file for all irphandler\* files
#include "irphandlerspriv.hpp"
#endif

#if FX_IS_USER_MODE
#define PWDF_REQUEST_PARAMETERS  PVOID
#define PFN_WDF_REQUEST_CANCEL   PVOID
#define PIO_CSQ_IRP_CONTEXT      PVOID
#endif*/

extern "C" {
#include "mx.h"
}

#include "fxmin.hpp"

#include "wdfmemory.h"
#include "wdfrequest.h"
#include "wdfio.h"
#include "wdfdevice.h"
#include "wdfwmi.h"
#include "wdfchildlist.h"
#include "wdfpdo.h"
#include "wdffdo.h"
#include "fxirpqueue.hpp"
#include "fxcallback.hpp"

// <FxSystemWorkItem.hpp>
__drv_functionClass(EVT_SYSTEMWORKITEM)
__drv_maxIRQL(PASSIVE_LEVEL)
__drv_maxFunctionIRQL(DISPATCH_LEVEL)
__drv_sameIRQL
typedef
VOID
EVT_SYSTEMWORKITEM(
    __in PVOID Parameter
    );

typedef EVT_SYSTEMWORKITEM *PFN_WDF_SYSTEMWORKITEM;

// </FxSystemWorkItem.hpp>

#include "fxsystemthread.hpp"

#include "fxcallbackspinlock.hpp"
#include "fxcallbackmutexlock.hpp"
#include "fxtransactionedlist.hpp"

#if (FX_CORE_MODE == FX_CORE_KERNEL_MODE)
#include "fxirpkm.hpp"
#else
#include "fxirpum.hpp"
#endif


#include "fxpackage.hpp"
#include "fxcollection.hpp"
#include "fxdeviceinitshared.hpp"
#include "fxdevicetomxinterface.hpp"

#include "ifxmemory.hpp"
#include "fxcallback.hpp"
#include "fxrequestcontext.hpp"
#include "fxrequestcontexttypes.h"
#include "fxrequestbase.hpp"
#include "fxmemoryobject.hpp"
#include "fxmemorybufferpreallocated.hpp"
#include "fxrequestmemory.hpp"
#include "fxrequest.hpp"
#include "fxrequestbuffer.hpp"
#include "fxsyncrequest.hpp"

#include "shared/irphandlers/irphandlerspriv.hpp"
#include "fxpkgpnp.hpp"
#include "fxpkgio.hpp"
#include "fxioqueue.hpp"
#include "fxioqueuecallbacks.hpp"


#include "fxversionedstructures.h"

#endif  //_IOPRIV_H_
