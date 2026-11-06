/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ReactOS ARM64 architecture asm macros for armasm64 and GAS
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef __KXARM64_H__
#define __KXARM64_H__

#ifdef _ARM64EC_
    /* EC code symbols carry a # prefix, the plain name is the x64 view */
    #define EC_PREFIX_TOKEN #
    #define EC_PREFIX() EC_PREFIX_TOKEN
    #define EC_JOIN(Name) EC_PREFIX() ## Name
    #define A64NAME(Name) |EC_JOIN(Name)|
#else
    #define A64NAME(Name) Name
#endif

#ifdef _MSC_VER

    /* armasm64 labels start in the first column and take no colon */
    #define LABEL(Name) Name

    GBLA __ComdatAlignment
__ComdatAlignment SETA 2

    /* Symbol name without the bars, set by __SetPlainName */
    GBLS __PlainName

    /* Entry thunk for the next NESTED_ENTRY, empty when there is none */
    GBLS __PendingEntryThunk
__PendingEntryThunk SETS ""

    MACRO
    __SetPlainName $FuncName
__PlainName SETS "$FuncName"
        IF (__PlainName :LEFT: 1) = "|"
__PlainName SETS (__PlainName :LEFT: (:LEN: __PlainName - 1)) :RIGHT: (:LEN: __PlainName - 2)
        ENDIF
    MEND

    MACRO
    TEXTAREA
        AREA |.text|,ALIGN=2,CODE,READONLY
    MEND

    MACRO
    DATAAREA
        AREA |.data|,DATA
    MEND

    MACRO
    RODATAAREA
        AREA |.rdata|,DATA,READONLY
    MEND

    MACRO
    XDATAAREA
        AREA |.xdata|,ALIGN=2,READONLY
    MEND

    MACRO
    PDATAAREA
        AREA |.pdata|,ALIGN=2,READONLY
    MEND

    /* Image relative address, IMAGE_REL_ARM64_ADDR32NB */
    MACRO
    DCRVA $Symbol
        DCD $Symbol
        RELOC 2
    MEND

    MACRO
    SET_COMDAT_ALIGNMENT $Alignment
__ComdatAlignment SETA $Alignment
    MEND

    MACRO
    NESTED_ENTRY $FuncName
        LCLS Name
        __SetPlainName $FuncName
Name    SETS "|":CC:__PlainName:CC:"|"
        ALIGN 4
        EXPORT $Name
        IF __PendingEntryThunk <> ""
        /* The emulator finds the entry thunk through the word before the function */
        DCD ($__PendingEntryThunk - ({PC} + 4)) + 1
__PendingEntryThunk SETS ""
        ENDIF
$Name   PROC
    MEND

    MACRO
    NESTED_END $FuncName
        LTORG
        ENDP
    MEND

    MACRO
    LEAF_ENTRY $FuncName
        NESTED_ENTRY $FuncName
    MEND

    MACRO
    LEAF_END $FuncName
        NESTED_END $FuncName
    MEND

    /* Each function gets its own section, aligned by SET_COMDAT_ALIGNMENT */
    MACRO
    LEAF_ENTRY_COMDAT $FuncName
        LCLS Area
        __SetPlainName $FuncName
Area    SETS "|.text$$mn$$":CC:__PlainName:CC:"|"
        AREA $Area,ALIGN=$__ComdatAlignment,CODE,READONLY
        NESTED_ENTRY $FuncName
    MEND

    MACRO
    ALTERNATE_ENTRY $FuncName
        LCLS Name
        __SetPlainName $FuncName
Name    SETS "|":CC:__PlainName:CC:"|"
        EXPORT $Name
$Name
    MEND

#ifdef _ARM64EC_
    /* x64 entry for an EC function with up to four integer args (x0-x3 are rcx, rdx, r8, r9).
       x9 is the EC target, lr the x64 return address, $SaveQ preserves q6-q15 for the caller. */
    MACRO
    ARM64EC_ENTRY_THUNK $FuncName, $ParamCount, $SaveQ
        LCLS Thunk
        LCLS Area
        ASSERT $ParamCount <= 4
        __SetPlainName $FuncName
Thunk   SETS "|":CC:__PlainName:CC:"$$entry_thunk|"
        /* Same section as the LEAF_ENTRY_COMDAT that follows, the entry word is section relative */
Area    SETS "|.text$$mn$$":CC:__PlainName:CC:"|"
        AREA $Area,ALIGN=$__ComdatAlignment,CODE,READONLY
        IMPORT __os_arm64x_dispatch_ret
        ALIGN 4
$Thunk  PROC
        stp     fp, lr, [sp, #-16]!
        mov     fp, sp
        IF $SaveQ <> 0
        stp     q6, q7, [sp, #-32]!
        stp     q8, q9, [sp, #-32]!
        stp     q10, q11, [sp, #-32]!
        stp     q12, q13, [sp, #-32]!
        stp     q14, q15, [sp, #-32]!
        ENDIF
        blr     x9
        mov     x8, x0
        IF $SaveQ <> 0
        ldp     q14, q15, [sp], #32
        ldp     q12, q13, [sp], #32
        ldp     q10, q11, [sp], #32
        ldp     q8, q9, [sp], #32
        ldp     q6, q7, [sp], #32
        ENDIF
        ldp     fp, lr, [sp], #16
        adrp    xip0, __os_arm64x_dispatch_ret
        ldr     xip0, [xip0, __os_arm64x_dispatch_ret]
        br      xip0
        ENDP
__PendingEntryThunk SETS Thunk
    MEND
#else
    MACRO
    ARM64EC_ENTRY_THUNK $FuncName, $ParamCount, $SaveQ
    MEND
#endif

#else

    #define LABEL(Name) Name:
    #define EQU =

.macro TEXTAREA
    .section .text, "xr"
    .p2align 2
.endm

.macro DATAAREA
    .section .data, "dw"
.endm

.macro RODATAAREA
    .section .rdata, "dr"
.endm

.macro XDATAAREA
    .section .xdata, "dr"
    .p2align 2
.endm

.macro PDATAAREA
    .section .pdata, "dr"
    .p2align 2
.endm

.macro DCRVA Symbol
    .rva \Symbol
.endm

/* The armasm [FUNC] attribute has no GAS counterpart */
.macro EXPORT Name, Attribute
    .global \Name
.endm

/* Only weak aliases need anything, plain imports resolve at link time */
.macro IMPORT Name, Kind, Target
    .ifc \Kind,WEAK
        .weak \Name
        .set \Name, \Target
    .endif
.endm

.set __ComdatAlignment, 2

.macro SET_COMDAT_ALIGNMENT Alignment
    .set __ComdatAlignment, \Alignment
.endm

.macro NESTED_ENTRY Name
    .global \Name
    .p2align 2
\Name:
.endm

.macro NESTED_END Name
    .ltorg
.endm

.macro LEAF_ENTRY Name
    NESTED_ENTRY \Name
.endm

.macro LEAF_END Name
    NESTED_END \Name
.endm

.macro LEAF_ENTRY_COMDAT Name
    .section .text$mn$\Name, "xr"
    .p2align __ComdatAlignment
    NESTED_ENTRY \Name
.endm

.macro ALTERNATE_ENTRY Name
    .global \Name
\Name:
.endm

.macro ARM64EC_ENTRY_THUNK A64Name, Results, Params
.endm

/* armasm ALIGN takes a byte count, code sections pad with nop */
.macro ALIGN Alignment, Offset, Pad, PadSize
    .balign \Alignment
.endm

.macro dcb Values:vararg
    .byte \Values
.endm

.macro dcw Values:vararg
    .short \Values
.endm

.macro dcd Values:vararg
    .long \Values
.endm

.macro dcq Values:vararg
    .quad \Values
.endm

.macro END
.endm

#endif

#endif /* __KXARM64_H__ */
