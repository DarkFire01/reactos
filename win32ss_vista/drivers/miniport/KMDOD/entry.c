
 // Standard C-runtime headers
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// NTOS headers
#include <ntddk.h>
#include <debug.h>
NTSTATUS
NTAPI
CppDriverEntry(
    _In_  DRIVER_OBJECT*  pDriverObject,
    _In_  UNICODE_STRING* pRegistryPath);

NTSTATUS
NTAPI 
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    PAGED_CODE();
    DPRINT1("Entry Cpp Driver Entry\n");
    CppDriverEntry(DriverObject, RegistryPath);
    DPRINT1("return from Cpp Driver Entry\n");
    return 0;
}