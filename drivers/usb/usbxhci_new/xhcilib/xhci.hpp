/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Resource definitions
 * COPYRIGHT:       Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "xhcihardware.hpp"

class XHCI {
public:
	static	NTSTATUS			NoOp();

								XHCI();
								~XHCI();
				// Doorbell register functions
	inline	UINT32				ReadDoorReg32(UINT32 reg);
	inline	void				WriteDoorReg32(UINT32 reg, UINT32 value);
private:
	/* hehehehehehehehehheeheheh - kekw */
 	PULONG BaseIoAdress;
    PULONG OperationalRegs;
    PULONG RunTimeRegisterBase;
    PULONG DoorBellRegisterBase;
    UCHAR FrameLengthAdjustment;
    BOOLEAN IsStarted;
    USHORT HcSystemErrors;
    ULONG PortRoutingControl;
    USHORT NumberOfPorts; // HCSPARAMS1 => N_PORTS
    USHORT PortPowerControl; // HCSPARAMS => Port Power Control (PPC)
    USHORT PageSize;
    USHORT MaxScratchPadBuffers;
    PMDL ScratchPadArrayMDL;
    PMDL ScratchPadBufferMDL;
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
};