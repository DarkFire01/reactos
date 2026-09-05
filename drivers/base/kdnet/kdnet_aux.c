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
 * Only the extension's ranges are marked. This module's own image and its
 * packet buffers are not, which would have to change before a debug session
 * could survive a hibernate: the loader frees anything not claimed here, and
 * the transport would resume against memory that has since been reused.
 * Claiming them needs this image's base and extent, which the module is not
 * told at the point the range is set.
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
