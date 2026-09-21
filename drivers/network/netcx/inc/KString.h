/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Pool allocated counted string
 *
 * Layout recovered from NetAdapterCx.pdb: a UNICODE_STRING that carries its
 * own nonpaged allocation, so it can be handed around in a KPtr.
 */

#pragma once

#include <KMacros.h>
#include <knew.h>

namespace Rtl
{

struct KRTL_CLASS_DPC_ALLOC KString :
    public NONPAGED_OBJECT<'KStr'>,
    public UNICODE_STRING
{
};

}
