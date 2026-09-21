/*++

Copyright (c) Microsoft Corporation

Module Name:

    FxObjectInfoKm.cpp

Abstract:

    This file contains object info split from globals.cpp

    This is because objects incorporated in KMDF and UMDF will differ

Author:




Environment:

    Kernel mode only

Revision History:

--*/

#include "fxobjectpch.hpp"

#include "fxmemorybufferpreallocated.hpp"
#include "fxuserobject.hpp"
#include "fxusbdevice.hpp"
#include "fxusbpipe.hpp"
#include "fxusbinterface.hpp"
#include "wudfrdnonpnp.hpp"
#include "fxcompaniontarget.hpp"

#include "fxobjectinfodatakm.hpp"
