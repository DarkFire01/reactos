/** @file
  DfBrTypes.h - universal fixed-width types for DefyBrowser.

  Deliberately tiny and self-contained so it can be compiled in:
    - hosted C (Win32 / POSIX)
    - UEFI (where libc is absent; stdint.h is still provided by EDK2's
      BaseTypes.h via a shim, but this header stays decoupled).

  Primitive type aliases are lowercase game-engine style (`u32`, `i32`,
  `f32`, `b32`). Struct types, functions, enums, and constants follow
  UEFI / Win32 conventions: PascalCase with a `DfBr` / `DFBR_` prefix.

  `b32` is a 32-bit boolean. Avoids the sub-word-store / padding quirks
  C99 `_Bool` can cause and matches the Handmade Hero convention.
**/

#ifndef DFBR_TYPES_H_
#define DFBR_TYPES_H_

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;

typedef float     f32;
typedef double    f64;

typedef size_t    usize;
typedef ptrdiff_t isize;

typedef i32       b32;

#define DFBR_TRUE   ((b32)1)
#define DFBR_FALSE  ((b32)0)

#ifndef DFBR_NULL
#define DFBR_NULL   ((void*)0)
#endif

/** Result codes returned by DfBr subsystems. Kept small and non-overlapping
    with any libc errno; we deliberately don't piggy-back on EFI_STATUS so
    the hosted build has zero EFI dependencies. **/
typedef i32 DfBrResult;

#define DFBR_OK                     0
#define DFBR_ERR_INVALID_ARG       -1
#define DFBR_ERR_OUT_OF_MEMORY     -2
#define DFBR_ERR_NOT_FOUND         -3
#define DFBR_ERR_PARSE             -4
#define DFBR_ERR_IO                -5
#define DFBR_ERR_UNSUPPORTED       -6

#define DFBR_UNUSED(x)     ((void)(x))
#define DFBR_ARRAY_LEN(a)  (sizeof(a) / sizeof((a)[0]))

#endif /* DFBR_TYPES_H_ */
