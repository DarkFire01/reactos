#include "precomp.h"
//#define NDEBUG
#include <debug.h>


UNICODE_STRING ProcessorHardwareIds = {0, 0, NULL};
LPWSTR ProcessorIdString = NULL;
LPWSTR ProcessorNameString = NULL;

static
CODE_SEG("INIT")
NTSTATUS
AcpiRegOpenKey(IN HANDLE ParentKeyHandle,
               IN LPCWSTR KeyName,
               IN ACCESS_MASK DesiredAccess,
               OUT HANDLE KeyHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;

    RtlInitUnicodeString(&Name, KeyName);

    InitializeObjectAttributes(&ObjectAttributes,
                               &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               ParentKeyHandle,
                               NULL);

    return ZwOpenKey(KeyHandle,
                     DesiredAccess,
                     &ObjectAttributes);
}

static
CODE_SEG("INIT")
NTSTATUS
AcpiRegQueryValue(IN HANDLE KeyHandle,
                  IN LPWSTR ValueName,
                  OUT PULONG Type OPTIONAL,
                  OUT PVOID Data OPTIONAL,
                  IN OUT PULONG DataLength OPTIONAL)
{
    PKEY_VALUE_PARTIAL_INFORMATION ValueInfo;
    UNICODE_STRING Name;
    ULONG BufferLength = 0;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, ValueName);

    if (DataLength != NULL)
        BufferLength = *DataLength;

    /* Check if the caller provided a valid buffer */
    if ((Data != NULL) && (BufferLength != 0))
    {
        BufferLength += FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);

        /* Allocate memory for the value */
        ValueInfo = ExAllocatePoolWithTag(PagedPool, BufferLength, 'MpcA');
        if (ValueInfo == NULL)
            return STATUS_NO_MEMORY;
    }
    else
    {
        /* Caller didn't provide a valid buffer, assume he wants the size only */
        ValueInfo = NULL;
        BufferLength = 0;
    }

    /* Query the value */
    Status = ZwQueryValueKey(KeyHandle,
                             &Name,
                             KeyValuePartialInformation,
                             ValueInfo,
                             BufferLength,
                             &BufferLength);

    if (DataLength != NULL)
        *DataLength = BufferLength;

    /* Check if we have the size only */
    if (ValueInfo == NULL)
    {
        /* Check for unexpected status */
        if ((Status != STATUS_BUFFER_OVERFLOW) &&
            (Status != STATUS_BUFFER_TOO_SMALL))
        {
            return Status;
        }

        /* All is well */
        Status = STATUS_SUCCESS;
    }
    /* Otherwise the caller wanted data back, check if we got it */
    else if (NT_SUCCESS(Status))
    {
        if (Type != NULL)
            *Type = ValueInfo->Type;

        /* Copy it */
        RtlMoveMemory(Data, ValueInfo->Data, ValueInfo->DataLength);

        /* if the type is REG_SZ and data is not 0-terminated
         * and there is enough space in the buffer NT appends a \0 */
        if (((ValueInfo->Type == REG_SZ) ||
             (ValueInfo->Type == REG_EXPAND_SZ) ||
             (ValueInfo->Type == REG_MULTI_SZ)) &&
            (ValueInfo->DataLength <= *DataLength - sizeof(WCHAR)))
        {
            WCHAR *ptr = (WCHAR *)((ULONG_PTR)Data + ValueInfo->DataLength);
            if ((ptr > (WCHAR *)Data) && ptr[-1])
                *ptr = 0;
        }
    }

    /* Free the memory and return status */
    if (ValueInfo != NULL)
    {
        ExFreePoolWithTag(ValueInfo, 'MpcA');
    }

    return Status;
}

static
CODE_SEG("INIT")
NTSTATUS
GetProcessorInformation(VOID)
{
    LPWSTR ProcessorIdentifier = NULL;
    LPWSTR ProcessorVendorIdentifier = NULL;
    LPWSTR HardwareIdsBuffer = NULL;
    HANDLE ProcessorHandle = NULL;
    ULONG Length = 0, Level1Length = 0, Level2Length = 0, Level3Length = 0;
    SIZE_T HardwareIdsLength = 0;
    SIZE_T VendorIdentifierLength;
    ULONG i;
    PWCHAR Ptr;
    NTSTATUS Status;

    DPRINT("GetProcessorInformation()\n");

    /* Open the key for CPU 0 */
    Status = AcpiRegOpenKey(NULL,
                            L"\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0",
                            KEY_READ,
                            &ProcessorHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to open CentralProcessor registry key: 0x%lx\n", Status);
        goto done;
    }

    /* Query the processor identifier length */
    Status = AcpiRegQueryValue(ProcessorHandle,
                               L"Identifier",
                               NULL,
                               NULL,
                               &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to query Identifier value: 0x%lx\n", Status);
        goto done;
    }

    /* Remember the length as fallback for level 1-3 length */
    Level1Length = Level2Length = Level3Length = Length;

    /* Allocate a buffer large enough to be zero terminated */
    Length += sizeof(UNICODE_NULL);
    ProcessorIdentifier = ExAllocatePoolWithTag(PagedPool, Length, 'IpcA');
    if (ProcessorIdentifier == NULL)
    {
        DPRINT1("Failed to allocate 0x%lx bytes\n", Length);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    /* Query the processor identifier string */
    Status = AcpiRegQueryValue(ProcessorHandle,
                               L"Identifier",
                               NULL,
                               ProcessorIdentifier,
                               &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to query Identifier value: 0x%lx\n", Status);
        goto done;
    }

    /* Query the processor name length */
    Length = 0;
    Status = AcpiRegQueryValue(ProcessorHandle,
                               L"ProcessorNameString",
                               NULL,
                               NULL,
                               &Length);
    if (NT_SUCCESS(Status))
    {
        /* Allocate a buffer large enough to be zero terminated */
        Length += sizeof(UNICODE_NULL);
        ProcessorNameString = ExAllocatePoolWithTag(PagedPool, Length, 'IpcA');
        if (ProcessorNameString == NULL)
        {
            DPRINT1("Failed to allocate 0x%lx bytes\n", Length);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }

        /* Query the processor name string */
        Status = AcpiRegQueryValue(ProcessorHandle,
                                   L"ProcessorNameString",
                                   NULL,
                                   ProcessorNameString,
                                   &Length);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to query ProcessorNameString value: 0x%lx\n", Status);
            goto done;
        }
    }

    /* Query the vendor identifier length */
    Length = 0;
    Status = AcpiRegQueryValue(ProcessorHandle,
                               L"VendorIdentifier",
                               NULL,
                               NULL,
                               &Length);
    if (!NT_SUCCESS(Status) || (Length == 0))
    {
        DPRINT1("Failed to query VendorIdentifier value: 0x%lx\n", Status);
        goto done;
    }

    /* Allocate a buffer large enough to be zero terminated */
    Length += sizeof(UNICODE_NULL);
    ProcessorVendorIdentifier = ExAllocatePoolWithTag(PagedPool, Length, 'IpcA');
    if (ProcessorVendorIdentifier == NULL)
    {
        DPRINT1("Failed to allocate 0x%lx bytes\n", Length);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    /* Query the vendor identifier string */
    Status = AcpiRegQueryValue(ProcessorHandle,
                               L"VendorIdentifier",
                               NULL,
                               ProcessorVendorIdentifier,
                               &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to query VendorIdentifier value: 0x%lx\n", Status);
        goto done;
    }

    /* Change spaces to underscores */
    for (i = 0; i < wcslen(ProcessorIdentifier); i++)
    {
        if (ProcessorIdentifier[i] == L' ')
            ProcessorIdentifier[i] = L'_';
    }

    Ptr = wcsstr(ProcessorIdentifier, L"Stepping");
    if (Ptr != NULL)
    {
        Ptr--;
        Level1Length = (ULONG)(Ptr - ProcessorIdentifier);
    }

    Ptr = wcsstr(ProcessorIdentifier, L"Model");
    if (Ptr != NULL)
    {
        Ptr--;
        Level2Length = (ULONG)(Ptr - ProcessorIdentifier);
    }

    Ptr = wcsstr(ProcessorIdentifier, L"Family");
    if (Ptr != NULL)
    {
        Ptr--;
        Level3Length = (ULONG)(Ptr - ProcessorIdentifier);
    }

    VendorIdentifierLength = (USHORT)wcslen(ProcessorVendorIdentifier);

    /* Calculate the size of the full REG_MULTI_SZ data (see swprintf below) */
    HardwareIdsLength = (5 + VendorIdentifierLength + 3 + Level1Length + 1 +
                         1 + VendorIdentifierLength + 3 + Level1Length + 1 +
                         5 + VendorIdentifierLength + 3 + Level2Length + 1 +
                         1 + VendorIdentifierLength + 3 + Level2Length + 1 +
                         5 + VendorIdentifierLength + 3 + Level3Length + 1 +
                         1 + VendorIdentifierLength + 3 + Level3Length + 1 +
                         1) * sizeof(WCHAR);

    /* Allocate a buffer to the data */
    HardwareIdsBuffer = ExAllocatePoolWithTag(PagedPool, HardwareIdsLength, 'IpcA');
    if (HardwareIdsBuffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Length = 0;
    Length += swprintf(&HardwareIdsBuffer[Length], L"ACPI\\%s_-_%.*s", ProcessorVendorIdentifier, Level1Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    Length += swprintf(&HardwareIdsBuffer[Length], L"*%s_-_%.*s", ProcessorVendorIdentifier, Level1Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    Length += swprintf(&HardwareIdsBuffer[Length], L"ACPI\\%s_-_%.*s", ProcessorVendorIdentifier, Level2Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    Length += swprintf(&HardwareIdsBuffer[Length], L"*%s_-_%.*s", ProcessorVendorIdentifier, Level2Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    Length += swprintf(&HardwareIdsBuffer[Length], L"ACPI\\%s_-_%.*s", ProcessorVendorIdentifier, Level3Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    Length += swprintf(&HardwareIdsBuffer[Length], L"*%s_-_%.*s", ProcessorVendorIdentifier, Level3Length, ProcessorIdentifier);
    HardwareIdsBuffer[Length++] = UNICODE_NULL;
    HardwareIdsBuffer[Length++] = UNICODE_NULL;

    /* Make sure we counted correctly */
    NT_ASSERT(Length * sizeof(WCHAR) == HardwareIdsLength);

    ProcessorHardwareIds.Length = (SHORT)HardwareIdsLength;
    ProcessorHardwareIds.MaximumLength = ProcessorHardwareIds.Length;
    ProcessorHardwareIds.Buffer = HardwareIdsBuffer;

    Length = (5 + VendorIdentifierLength + 3 + Level1Length + 1) * sizeof(WCHAR);
    ProcessorIdString = ExAllocatePoolWithTag(PagedPool, Length, 'IpcA');
    if (ProcessorIdString != NULL)
    {
        Length = swprintf(ProcessorIdString, L"ACPI\\%s_-_%.*s", ProcessorVendorIdentifier, Level1Length, ProcessorIdentifier);
        ProcessorIdString[Length++] = UNICODE_NULL;
        DPRINT("ProcessorIdString: %S\n", ProcessorIdString);
    }

done:
    if (ProcessorHandle != NULL)
        ZwClose(ProcessorHandle);

    if (ProcessorIdentifier != NULL)
        ExFreePoolWithTag(ProcessorIdentifier, 'IpcA');

    if (ProcessorVendorIdentifier != NULL)
        ExFreePoolWithTag(ProcessorVendorIdentifier, 'IpcA');

    if (!NT_SUCCESS(Status))
    {
        if (HardwareIdsBuffer != NULL)
            ExFreePoolWithTag(HardwareIdsBuffer, 'IpcA');
    }

    return Status;
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS Status;
    Status = GetProcessorInformation();
    if (!NT_SUCCESS(Status))
    {
        NT_ASSERT(FALSE);
        return Status;
    }

    DPRINT("ACPI_NEW: Entry %X\n",  ACPIInitUACPI());
    UNIMPLEMENTED;
    return 1;
}
