/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Header file to hold win32k dxvista information
 * COPYRIGHT:   Copyright 2025 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

VOID
APIENTRY
DxStartupDxgkInt(VOID);

/**
 * @brief TRUE when at least one WDDM adapter has finished starting, i.e. when the CDD has
 *        something to talk to. Not a cached flag - see the definition in ntgdi/d3dkmt.c.
 */
BOOLEAN
APIENTRY
DxIsWddmDisplayAvailable(VOID);
