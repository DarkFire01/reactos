/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

   fxsupportpchum.h

Abstract:

    This module contains header definitions and include files needed by all
    modules in fx\support

Author:

Environment:
   User mode only

Revision History:

--*/

#ifndef __FX_SUPPORT_PCH_UM_HPP__
#define __FX_SUPPORT_PCH_UM_HPP__

extern "C" {
#include "mx.h"
}

#include "fxmin.hpp"

#include "fxpagedobject.hpp"
#include "fxregkey.hpp"
#include "fxcollection.hpp"
#include "fxstring.hpp"
#include "stringutil.hpp"

#include "fxdevicetext.hpp"

#include <wdfresource.h>
#include <fxresource.hpp>


#endif // __FX_SUPPORT_PCH_UM_HPP__
