/*
 * PROJECT:     ReactOS ARM64EC support library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     CHPE metadata, emulator dispatch pointers and load config for ARM64EC images
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

/* The linker builds these tables from the EC and x64 code in the image */
    IMPORT __hybrid_code_map
    IMPORT __hybrid_code_map_count
    IMPORT __x64_code_ranges_to_entry_points
    IMPORT __x64_code_ranges_to_entry_points_count
    IMPORT __arm64x_redirection_metadata
    IMPORT __arm64x_redirection_metadata_count
    IMPORT __arm64x_native_entrypoint
    IMPORT __arm64x_extra_rfe_table
    IMPORT __arm64x_extra_rfe_table_size
    IMPORT __hybrid_auxiliary_iat
    IMPORT __hybrid_auxiliary_iat_copy
    IMPORT __hybrid_auxiliary_delayload_iat
    IMPORT __hybrid_auxiliary_delayload_iat_copy
    IMPORT __hybrid_image_info_bitfield

/* Emulator entry points, filled in by the loader. Until then the indirect
   call checks leave x11 alone, so every target is called directly. */
    AREA |.00cfg|,DATA,READONLY,ALIGN=3

    EXPORT __os_arm64x_dispatch_call_no_redirect
    EXPORT __os_arm64x_dispatch_ret
    EXPORT __os_arm64x_dispatch_icall
    EXPORT __os_arm64x_check_icall
    EXPORT __os_arm64x_dispatch_call
    EXPORT __os_arm64x_check_call
    EXPORT __os_arm64x_dispatch_icall_cfg
    EXPORT __os_arm64x_check_icall_cfg
    EXPORT __os_arm64x_get_x64_information
    EXPORT __os_arm64x_set_x64_information
    EXPORT __os_arm64x_dispatch_fptr
    EXPORT __os_arm64x_x64_jump
    EXPORT __os_arm64x_helper3
    EXPORT __os_arm64x_helper4
    EXPORT __os_arm64x_helper5
    EXPORT __os_arm64x_helper6
    EXPORT __os_arm64x_helper7
    EXPORT __os_arm64x_helper8

__os_arm64x_dispatch_call_no_redirect
    DCQ 0
__os_arm64x_dispatch_ret
    DCQ 0
__os_arm64x_dispatch_icall
__os_arm64x_check_icall
    DCQ EcCallDirect
__os_arm64x_dispatch_call
__os_arm64x_check_call
    DCQ 0
__os_arm64x_dispatch_icall_cfg
__os_arm64x_check_icall_cfg
    DCQ EcCallDirect
__os_arm64x_get_x64_information
    DCQ 0
__os_arm64x_set_x64_information
    DCQ 0
__os_arm64x_dispatch_fptr
__os_arm64x_x64_jump
    DCQ 0
__os_arm64x_helper3
    DCQ 0
__os_arm64x_helper4
    DCQ 0
__os_arm64x_helper5
    DCQ 0
__os_arm64x_helper6
    DCQ 0
__os_arm64x_helper7
    DCQ 0
__os_arm64x_helper8
    DCQ 0

/* IMAGE_ARM64EC_METADATA, version 2 */
    AREA |.rdata|,DATA,READONLY,ALIGN=3

    EXPORT __chpe_metadata
__chpe_metadata
    DCD     2
    DCRVA   __hybrid_code_map
    DCD     __hybrid_code_map_count
    DCRVA   __x64_code_ranges_to_entry_points
    DCRVA   __arm64x_redirection_metadata
    DCRVA   __os_arm64x_dispatch_call_no_redirect
    DCRVA   __os_arm64x_dispatch_ret
    DCRVA   __os_arm64x_check_call
    DCRVA   __os_arm64x_check_icall
    DCRVA   __os_arm64x_check_icall_cfg
    DCRVA   __arm64x_native_entrypoint
    DCRVA   __hybrid_auxiliary_iat
    DCD     __x64_code_ranges_to_entry_points_count
    DCD     __arm64x_redirection_metadata_count
    DCRVA   __os_arm64x_get_x64_information
    DCRVA   __os_arm64x_set_x64_information
    DCRVA   __arm64x_extra_rfe_table
    DCD     __arm64x_extra_rfe_table_size
    DCRVA   __os_arm64x_x64_jump
    DCRVA   __hybrid_auxiliary_iat_copy
    DCRVA   __hybrid_auxiliary_delayload_iat
    DCRVA   __hybrid_auxiliary_delayload_iat_copy
    DCD     __hybrid_image_info_bitfield
    DCRVA   __os_arm64x_helper3
    DCRVA   __os_arm64x_helper4
    DCRVA   __os_arm64x_helper5
    DCRVA   __os_arm64x_helper6
    DCRVA   __os_arm64x_helper7
    DCRVA   __os_arm64x_helper8

/* IMAGE_LOAD_CONFIG_DIRECTORY64, only CHPEMetadataPointer is set, the linker fills the DVRT fields.
   The linker takes the x64 view load config from the start of the section holding it. */
    AREA |.rdata$lcfg|,DATA,READONLY,ALIGN=3

    EXPORT _load_config_used
_load_config_used
    DCD     0x140
    SPACE   0xC8 - 4
    DCQ     __chpe_metadata
    SPACE   0x140 - 0xD0

    TEXTAREA

    /* The default indirect call check, the target in x11 is native code */
    LEAF_ENTRY EcCallDirect
    ret
    LEAF_END EcCallDirect

    END
