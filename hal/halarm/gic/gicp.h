/*
 * Private GIC implementation header - included only from gic.c.
 * Exposes nothing to the rest of the HAL; all public declarations
 * come from hal/halarm/include/gic.h and include/halp.h.
 */
#include <hal.h>

/*
 * Explicitly pull in the ARM64-specific NDK types so that KTRAP_FRAME,
 * KIPCR, PKIPCR and related structures are always resolved to the ReactOS
 * ARM64 definitions regardless of IntelliSense include-path ordering.
 */
#include <ndk/arm64/ketypes.h>

