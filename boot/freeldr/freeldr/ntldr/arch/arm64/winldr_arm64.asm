; PROJECT:         EFI Windows Loader
; LICENSE:         GPL - See COPYING in the top level directory
; FILE:            boot/freeldr/freeldr/ntldr/arch/arm64/winldr_arm64.asm
; PURPOSE:         ARM64 assembly helper for system instructions

    EXPORT Arm64InvalidateTlbVmalle1Is

    AREA |.text|, CODE, READONLY

; void Arm64InvalidateTlbVmalle1Is(void)
; Invalidate all TLB entries for EL1 using VMALLE1IS
Arm64InvalidateTlbVmalle1Is PROC
    TLBI VMALLE1IS
    DSB NSH
    ISB
    RET
    ENDP

    END
