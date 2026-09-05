/*
 * PROJECT:     ReactOS Networking Debugging Module
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Misc. Functions for kdnet
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdnet.h"

/**
 * @brief
 * Marks the memory the debug transport needs across hibernation.
 *
 * Only the extension is asked to claim its ranges. This module's own image and
 * packet buffers are not claimed, and there is no point claiming them yet:
 * PoSetHiberRange is @unimplemented in this kernel (ntoskrnl/po/power.c), so
 * every range handed to it is discarded. Marking anything here would only add
 * UNIMPLEMENTED noise to the boot log.
 *
 * The extension is still called, because whatever it does with the import is
 * its own business and it may not go through PoSetHiberRange at all.
 */
VOID
NTAPI
KdSetHiberRange(VOID)
{
    if (KdNetExtensibilityExports != NULL &&
        KdNetExtensibilityExports->KdSetHibernateRange != NULL)
    {
        KdNetExtensibilityExports->KdSetHibernateRange();
    }
}
