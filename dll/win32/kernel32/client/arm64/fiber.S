/*
 * Fiber stubs for ARM64 (same bring-up approach as amd64/fiber.S).
 */
#include <ksarm64.h>

    TEXTAREA

    LEAF_ENTRY BaseFiberStartup
    ARMASM_ASSERTFAIL
    ret
    LEAF_END BaseFiberStartup

    LEAF_ENTRY SwitchToFiber
    ARMASM_ASSERTFAIL
    ret
    LEAF_END SwitchToFiber

    END
