
add_definitions(-D_NTSYSTEM_)

list(APPEND SOURCE
    arbiter.c
    entry.c
    handler.c
    ordering.c
    range.c
    transaction.c)

add_library(arbiter ${SOURCE})
add_dependencies(arbiter bugcodes xdk)

#
# HAL-local variant.  The generic arbiter is also pulled into the x86 HAL (for
# the PCI interrupt arbiter).  Built as-is, its PAGED_CODE() checks KeGetCurrentIrql
#
add_library(arbiter_hal ${SOURCE})
target_compile_definitions(arbiter_hal PRIVATE _NTHAL_ _NTHALDLL_)
add_dependencies(arbiter_hal bugcodes xdk)
