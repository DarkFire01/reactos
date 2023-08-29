/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DISPLIB static library
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */
#define WINE_D3DK
#include <ntddk.h>
#include <windef.h>
#include <winerror.h>
#include <stdio.h>
#include <dispmprt.h>
#include <reactos/drivers/wddm/wddm_shared.h>
#include <wdm.h>

typedef
NTSTATUS
NTAPI
DXGKPORT_INITIALIZE(_In_ PDRIVER_OBJECT DriverObject,
					_In_ PUNICODE_STRING SourceString,
					_In_ PVOID DriverInitData);

typedef DXGKPORT_INITIALIZE                      *PDXGKPORT_INITIALIZE;

NTSTATUS
NTAPI
DisplibLoadDxgkrnl(_In_  ULONG IoControlCode,
                   _Out_ PDXGKPORT_INITIALIZE* DxgkInitPfn);

