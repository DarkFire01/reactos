/*
 * PROJECT:         ReactOS Boot Loader
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            boot/armllb/boot.s
 * PURPOSE:         Implements the entry point for ARM machines
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

	AREA    ARMLLBHEADER, DATA, READONLY

HeaderMagic   dcq 0x56190527 /* Header Magic */
CheckSum      dcq 0x5E4B8444 /* Checksum */
Timestamp     dcq 0x483BE54C /* Timestamp */
Imagesize     dcq 0x0CA10000 /* Image size (64K) */
Loadaddress   dcq 0x00000081 /* Load address */
Entrypoint    dcq 0x40000081 /* Entrypoint */
DataCheckSum  dcq 0x90873DD8 /* Data Checksum ('setenv verify n' must be set!) */
LinuxOS       dcb 5 		 /* Linux OS */
ARMlabel      dcb 2 		 /* ARM */
Kernel        dcb 2 		 /* Kernel */
NoCompression dcb 0 		 /* No compression */
	AREA    ARMLLBEntryPoint, CODE, READONLY

	ldr lr, L_LlbStartup
 	ldr sp, L_BootStackEnd
	bx lr

L_BootStackEnd dcq 0x81014000

	END
L_LlbStartup
	dcq LlbStartup
/* EOF */
