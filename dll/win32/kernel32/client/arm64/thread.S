/*
 * Thread and process start thunks for ARM64.
 * BaseInitializeContext sets X0 = start address, X1 = parameter (threads),
 * and Pc to these entry points (see client/utils.c).
 */
#include <ksarm64.h>

    IMPORT BaseThreadStartup
    IMPORT BaseProcessStartup

    TEXTAREA

    LEAF_ENTRY BaseThreadStartupThunk
    b BaseThreadStartup
    LEAF_END BaseThreadStartupThunk

    LEAF_ENTRY BaseProcessStartThunk
    b BaseProcessStartup
    LEAF_END BaseProcessStartThunk

    END
