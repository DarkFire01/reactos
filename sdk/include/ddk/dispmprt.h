/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Dispmprt public header
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

typedef struct _KMDDOD_INITIALIZATION_DATA {
    ULONG                                   Version;
    //TODO: Fill these out with the pfns
} KMDDOD_INITIALIZATION_DATA, *PKMDDOD_INITIALIZATION_DATA;

typedef struct _DRIVER_INITIALIZATION_DATA {
    ULONG                                   Version;
    //TODO: Fill these out with the pfns
} DRIVER_INITIALIZATION_DATA, *PDRIVER_INITIALIZATION_DATA;
