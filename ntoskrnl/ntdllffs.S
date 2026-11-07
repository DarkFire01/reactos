/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64EC system call table of ntdll, indexed by system call number
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

    /* The memory calls the emulator gets notified about are the wrappers in ldrec.c */
    MACRO
    SYSCALL_FFS $Name
    LCLS NtFuncName
    IF "$Name" = "AllocateVirtualMemory" :LOR: "$Name" = "FreeVirtualMemory" :LOR: "$Name" = "ProtectVirtualMemory"
NtFuncName SETS "|#LdrpEc$Name|"
    ELSE
NtFuncName SETS "|#Nt$Name|"
    ENDIF
    IMPORT $NtFuncName
    DCQ $NtFuncName
    MEND

    RODATAAREA

    ALIGN 8
    EXPORT __arm64ec_syscall_ffs
__arm64ec_syscall_ffs

#define SVC_(name, argcount) SYSCALL_FFS name
#include <sysfuncs.h>
#undef SVC_

__arm64ec_syscall_ffs_end

    EXPORT __arm64ec_syscall_ffs_size
__arm64ec_syscall_ffs_size
    DCD (__arm64ec_syscall_ffs_end - __arm64ec_syscall_ffs) / 8

    END
/* EOF */
