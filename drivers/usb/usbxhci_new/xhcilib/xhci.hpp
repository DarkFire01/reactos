/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Resource definitions
 * COPYRIGHT:       Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once
extern "C" {
    #include <ntddk.h>
    #include <windef.h>
    #include <stdio.h>
}

class XHCI {
public:
	static	NTSTATUS			NoOp();

								XHCI();
								~XHCI();
private:
};