/** @file

Root include file for Mde Package UEFI, UEFI_APPLICATION type modules.

This is the include file for any module of type UEFI and UEFI_APPLICATION. Uefi modules only use
types defined via this include file and can be ported easily to any
environment.

Copyright (c) 2006 - 2010, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials are licensed and made available under
the terms and conditions of the BSD License that accompanies this distribution.
The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php.

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef __PI_UEFI_H__
#define __PI_UEFI_H__

#include "UefiBaseType.h"
#include <UefiSpec.h>

#define EFI_DISK_IO_PROTOCOL_REVISION 0x00010000

#define EFI_DISK_IO_PROTOCOL_GUID \
 {0xCE345171,0xBA0B,0x11d2,\
 {0x8e,0x4F,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
 
typedef struct _EFI_DISK_IO_PROTOCOL {
 UINT64 Revision;
 EFI_DISK_READ ReadDisk;
 EFI_DISK_WRITE WriteDisk;
} EFI_DISK_IO_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_DISK_READ) (
 IN EFI_DISK_IO_PROTOCOL *This,
 IN UINT32 MediaId,
 IN UINT64 Offset,
 IN UINTN BufferSize,
 OUT VOID *Buffer
 );

#endif

