#include <dxgkrnl.h>
#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI 
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    return TRUE;
} // end DriverEntry()