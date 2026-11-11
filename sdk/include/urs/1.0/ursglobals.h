/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     URS client globals and function table storage
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

WDF_EXTERN_C_START

typedef struct _URS_DRIVER_GLOBALS
{
    ULONG Reserved;
} URS_DRIVER_GLOBALS, *PURS_DRIVER_GLOBALS;

typedef VOID (NTAPI *PFN_URSFUNC)(VOID);

/* Owned by the client stub library, filled in by the class bind */
extern PFN_URSFUNC UrsFunctions[];
extern PURS_DRIVER_GLOBALS UrsDriverGlobals;

WDF_EXTERN_C_END
