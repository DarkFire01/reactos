
#include <ksarm64.h>

    TEXTAREA

    LEAF_ENTRY LdrInitializeThunk
    ARMASM_ASSERTFAIL
    ret
    LEAF_END LdrInitializeThunk

    LEAF_ENTRY KiRaiseUserExceptionDispatcher
    ARMASM_ASSERTFAIL
    ret
    LEAF_END KiRaiseUserExceptionDispatcher

    LEAF_ENTRY KiUserApcDispatcher
    ARMASM_ASSERTFAIL
    ret
    LEAF_END KiUserApcDispatcher

    LEAF_ENTRY KiUserCallbackDispatcher
    ARMASM_ASSERTFAIL
    ret
    LEAF_END KiUserCallbackDispatcher

    LEAF_ENTRY KiUserExceptionDispatcher
    ARMASM_ASSERTFAIL
    ret
    LEAF_END KiUserExceptionDispatcher

    END
