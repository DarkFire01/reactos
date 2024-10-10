#include <ntifs.h>
UINT32 LaunchUACPI();
#include <debug.h>

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    LaunchUACPI();
    DPRINT1("DriverEntry: Exit\n");
    __debugbreak();

    return 0;
}