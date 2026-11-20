/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS library
 * FILE:        ndis/main.c
 * PURPOSE:     Driver entry point
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 *              Vizzini (vizzini@plasmic.com)
 * REVISIONS:
 *   CSH 01/08-2000 Created
 *   20 Aug 2003 Vizzini - NDIS4/5 revisions
 *   3  Oct 2003 Vizzini - formatting and minor bugfixing
 */

#include "ndissys.h"

#if DBG

/* See debug.h for debug/trace constants */
ULONG DebugTraceLevel = MIN_TRACE;

#endif /* DBG */

LONG CancelId;


VOID NTAPI MainUnload(
    PDRIVER_OBJECT DriverObject)
/*
 * FUNCTION: Unloads the driver
 * ARGUMENTS:
 *     DriverObject = Pointer to driver object created by the system
 */
{
  NDIS_DbgPrint(MAX_TRACE, ("Leaving.\n"));
}


NTSTATUS
NTAPI
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath)
/*
 * FUNCTION: Main driver entry point
 * ARGUMENTS:
 *     DriverObject = Pointer to a driver object for this driver
 *     RegistryPath = Registry node for configuration parameters
 * RETURNS:
 *     Status of driver initialization
 */
{
  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

  InitializeListHead(&ProtocolListHead);
  KeInitializeSpinLock(&ProtocolListLock);

  InitializeListHead(&MiniportListHead);
  KeInitializeSpinLock(&MiniportListLock);

  InitializeListHead(&AdapterListHead);
  KeInitializeSpinLock(&AdapterListLock);

  DriverObject->DriverUnload = MainUnload;

  CancelId = 0;

  CoreHookInitialize();

  return STATUS_SUCCESS;
}


/*
 * @implemented
 */
VOID
_cdecl
NdisWriteErrorLogEntry(
    IN  NDIS_HANDLE     NdisAdapterHandle,
    IN  NDIS_ERROR_CODE ErrorCode,
    IN  ULONG           NumberOfErrorValues,
    ...)
/*
 * FUNCTION: Write a syslog error
 * ARGUMENTS:
 *     NdisAdapterHandle:  Handle passed into MiniportInitialize
 *     ErrorCode:  32-bit error code to be logged
 *     NumberOfErrorValues: number of errors to log
 *     Variable: list of log items
 * NOTES:
 *     - THIS IS >CDECL<
 *     - The values go out as the entry's dump data, where miniports record why they failed
 */
{
  PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisAdapterHandle;
  PIO_ERROR_LOG_PACKET Packet = NULL;
  ULONG Values[8];
  ULONG Count, Index;
  va_list Arguments;

  Count = min(NumberOfErrorValues, RTL_NUMBER_OF(Values));
  va_start(Arguments, NumberOfErrorValues);
  for (Index = 0; Index < Count; Index++)
      Values[Index] = va_arg(Arguments, ULONG);
  va_end(Arguments);

  NDIS_DbgPrint(MIN_TRACE, ("ERROR: ErrorCode 0x%x, %lu value(s)\n", ErrorCode, NumberOfErrorValues));
  for (Index = 0; Index < Count; Index++)
      NDIS_DbgPrint(MIN_TRACE, ("    value %lu: 0x%08lx\n", Index, Values[Index]));

  if (Adapter && Adapter->NdisMiniportBlock.DeviceObject)
  {
      Packet = IoAllocateErrorLogEntry(Adapter->NdisMiniportBlock.DeviceObject,
                                       (UCHAR)(FIELD_OFFSET(IO_ERROR_LOG_PACKET, DumpData) + Count * sizeof(ULONG)));
  }

  if (!Packet)
      return;

  RtlZeroMemory(Packet, FIELD_OFFSET(IO_ERROR_LOG_PACKET, DumpData));
  Packet->ErrorCode = (NTSTATUS)ErrorCode;
  Packet->DumpDataSize = (USHORT)(Count * sizeof(ULONG));
  RtlCopyMemory(Packet->DumpData, Values, Count * sizeof(ULONG));
  IoWriteErrorLogEntry(Packet);
}


/*
 * @implemented
 */
NDIS_STATUS
EXPORT
NdisWriteEventLogEntry(
    IN  PVOID       LogHandle,
    IN  NDIS_STATUS EventCode,
    IN  ULONG       UniqueEventValue,
    IN  USHORT      NumStrings,
    IN  PVOID       StringsList OPTIONAL,
    IN  ULONG       DataSize,
    IN  PVOID       Data        OPTIONAL)
/*
 * FUNCTION: Log an event in the system event log
 * ARGUMENTS:
 *     LogHandle: pointer to the driver object of the protocol logging the event
 *     EventCode: NDIS_STATUS_XXX describing the event
 *     UniqueEventValue: identifies this instance of the error value
 *     NumStrings: number of strings in StringList
 *     StringList: list of strings to log
 *     DataSize: number of bytes in Data
 *     Data: binary dump data to help analyzing the event
 * NOTES:
 *     - NTAPI, not CDECL like WriteError...
 *     - FIXME Needs to use the real log interface, once there is one
 */
{
  /*
   * just returning true until we have an event log
   */
  NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));
  return NDIS_STATUS_SUCCESS;
}

/* EOF */
