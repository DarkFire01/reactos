/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     ReactOS ARM64 architecture asm macros
 *              Provides MSVC ARMASM64 and GCC/Clang GNU AS compatible
 *              function entry/exit macros, analogous to kxarm.h (ARM32)
 *              and kxamd64.inc (AMD64).
 * COPYRIGHT:   ReactOS Developers
 */

#define ENABLE_FRAME_POINTER 1

#ifdef _MSC_VER

/*
 * ================================================================
 * MSVC ARMASM64 section
 * Syntax: Microsoft ARMASM64 (armasm64.exe) assembler
 * ================================================================
 */

/*
 * cl /EP preprocessing can expand names that collide with MSVC intrinsics
 * (__debugbreak, __fastfail).  Use ARMASM_* macro names only (no #undef dance).
 */

    /* Persistent global string variables used to track function labels */
    GBLS __FuncStartLabel
    GBLS __FuncEndLabel
    GBLS __FuncArea
    GBLS __FuncExceptionHandler

    /*
     * __DeriveFunctionLabels - internal helper
     * Initialises the label strings for a function.
     */
    MACRO
    __DeriveFunctionLabels $FuncName
__FuncStartLabel    SETS "|$FuncName|"
__FuncEndLabel      SETS "|$FuncName._end|"
    MEND

    /*
     * __ExportName - emit an exported label (no PROC/ENDP frame)
     * Used for alternate entry points inside an existing function.
     */
    MACRO
    __ExportName $FuncName
        LCLS Name
Name    SETS "|$FuncName|"
        ALIGN 4
        EXPORT $Name
$Name
    MEND

    /*
     * __ExportProc - emit an exported function entry (PROC frame)
     */
    MACRO
    __ExportProc $FuncName
        LCLS Name
Name    SETS "|$FuncName|"
        ALIGN 4
        EXPORT $Name
$Name   PROC
    MEND

    /* ---- Section switches ---- */

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

    /*
     * NESTED_ENTRY - begin a non-leaf function (may call other functions /
     * modifies the stack).  Optional $AreaName overrides the default .text
     * section; optional $ExceptHandler names an exception handler.
     */
    MACRO
    NESTED_ENTRY $FuncName, $AreaName, $ExceptHandler
        __DeriveFunctionLabels $FuncName
__FuncArea SETS "|.text|"
        IF "$AreaName" != ""
__FuncArea SETS "$AreaName"
        ENDIF
__FuncExceptionHandler SETS ""
        IF "$ExceptHandler" != ""
__FuncExceptionHandler SETS "|$ExceptHandler|"
        ENDIF
        AREA $__FuncArea,CODE,READONLY
        __ExportProc $FuncName
        ROUT
    MEND

    /* PROLOG_END - marks the end of the function prologue (no-op here) */
    MACRO
    PROLOG_END
        ; Prologue boundary marker - no machine code emitted
    MEND

    /* NESTED_END - close a NESTED_ENTRY function */
    MACRO
    NESTED_END $FuncName
$__FuncEndLabel
        LTORG
        ENDP
__FuncStartLabel SETS ""
__FuncEndLabel SETS ""
    MEND

    /* LEAF_ENTRY - begin a leaf function (does not modify LR / stack) */
    MACRO
    LEAF_ENTRY $FuncName, $AreaName
        NESTED_ENTRY $FuncName, $AreaName
    MEND

    /* LEAF_END - close a LEAF_ENTRY function */
    MACRO
    LEAF_END $FuncName
        NESTED_END $FuncName
    MEND

    /*
     * ALTERNATE_ENTRY - declare a secondary public entry point inside an
     * already-open NESTED_ENTRY / LEAF_ENTRY function body.
     */
    MACRO
    ALTERNATE_ENTRY $FuncName
        __ExportName $FuncName
        ROUT
    MEND

    /*
     * Debug / assert helpers for ARMASM64 (see ksarm64.template.h ARM64_* codes).
     * Use DCD-encoded BRK (not "brk #imm"): cl /EP treats # as a directive start.
     * Encoding: 0xD4200000 | (imm16 << 5).
     */
    MACRO
    ARMASM_DEBUGBREAK
        DCD     0xd43e0000
    MEND

    MACRO
    ARMASM_ASSERTFAIL
        DCD     0xd43e0040
    MEND

    MACRO
    ARMASM_FASTFAIL
        DCD     0xd43e0060
    MEND

    MACRO
    ARMASM_BRKDIV0
        DCD     0xd43e0080
    MEND

    /* BRK #0xF001 — user/kernel debug service (__debugservice); see ksarm64.template.h */
    MACRO
    ARMASM_DEBUGSERVICE
        DCD     0xd43e0020
    MEND

#else /* !_MSC_VER */

/*
 * ================================================================
 * GAS / GCC / Clang section
 * Syntax: GNU Assembler (GAS) as used by GCC and Clang for AArch64
 * ================================================================
 */

/* Compatibility define: EQU → .equ */
#define EQU .equ

/*
 * IMPORT - declare an external symbol.
 * In GAS the assembler resolves externals automatically; this is a no-op
 * kept for source compatibility with ARMASM64 files.
 */
.macro IMPORT Name
    /* external reference - resolved by linker */
.endm

/* EXPORT - make a symbol globally visible */
.macro EXPORT Name
    .global \Name
.endm

/* ---- Section switches ---- */

.macro TEXTAREA
    .section .text, "ax"
    .align 2
.endm

.macro DATAAREA
    .section .data, "rw"
.endm

.macro RODATAAREA
    .section .rdata, "rw"
.endm

/*
 * NESTED_ENTRY - begin a (potentially non-leaf) function.
 * The optional Area argument is accepted for source compatibility but
 * ignored; GAS derives the section from the most recent section switch.
 */
.macro NESTED_ENTRY Name, Area:vararg
    .global \Name
    .align  2
    .type   \Name, %function
\Name:
.endm

/* PROLOG_END - marks prologue boundary; no-op in GAS */
.macro PROLOG_END
.endm

/* NESTED_END - close a NESTED_ENTRY and emit the ELF size directive */
.macro NESTED_END Name
    .size \Name, . - \Name
.endm

/* LEAF_ENTRY - begin a leaf function */
.macro LEAF_ENTRY Name, Area:vararg
    NESTED_ENTRY \Name
.endm

/* LEAF_END - close a LEAF_ENTRY */
.macro LEAF_END Name
    NESTED_END \Name
.endm

/*
 * ALTERNATE_ENTRY - secondary public entry point within an open function.
 * Exports the label but does NOT open a new PROC frame.
 */
.macro ALTERNATE_ENTRY Name
    .global \Name
\Name:
.endm

.macro ARMASM_DEBUGBREAK
    .inst 0xd43e0000
.endm

.macro ARMASM_ASSERTFAIL
    .inst 0xd43e0040
.endm

.macro ARMASM_FASTFAIL
    .inst 0xd43e0060
.endm

.macro ARMASM_BRKDIV0
    .inst 0xd43e0080
.endm

.macro ARMASM_DEBUGSERVICE
    .inst 0xd43e0020
.endm

/* END - marks end of source file; no-op in GAS (processed to EOF) */
#define END

/* Character constants */
#define CR  "\r"
#define LF  "\n"
#define NUL "\0"

#endif /* _MSC_VER */
