/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Configuration Manager, ARM64 processor description
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* SMBIOS processor information fields */
#define CMP_PROCESSOR_MANUFACTURER      0x07
#define CMP_PROCESSOR_VERSION           0x10
#define CMP_PROCESSOR_MAX_SPEED         0x14
#define CMP_PROCESSOR_CURRENT_SPEED     0x16

/* FUNCTIONS *****************************************************************/

static
NTSTATUS
CmpCreateVolatileKey(
    _In_ PCWSTR Path,
    _Out_ PHANDLE KeyHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    ULONG Disposition;

    RtlInitUnicodeString(&KeyName, Path);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    return ZwCreateKey(KeyHandle,
                       KEY_READ | KEY_WRITE,
                       &ObjectAttributes,
                       0,
                       NULL,
                       REG_OPTION_VOLATILE,
                       &Disposition);
}

static
NTSTATUS
CmpCreateProcessorKey(
    _In_ PCWSTR Class,
    _In_ ULONG Number,
    _Out_ PHANDLE KeyHandle)
{
    WCHAR Buffer[96];
    HANDLE ClassHandle;
    NTSTATUS Status;

    /* The loader does not describe the processors, so the class key may be missing */
    Status = RtlStringCbPrintfW(Buffer,
                                sizeof(Buffer),
                                L"\\Registry\\Machine\\Hardware\\Description\\System\\%s",
                                Class);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = CmpCreateVolatileKey(Buffer, &ClassHandle);
    if (!NT_SUCCESS(Status))
        return Status;
    ZwClose(ClassHandle);

    Status = RtlStringCbPrintfW(Buffer,
                                sizeof(Buffer),
                                L"\\Registry\\Machine\\Hardware\\Description\\System\\%s\\%lu",
                                Class,
                                Number);
    if (!NT_SUCCESS(Status))
        return Status;

    return CmpCreateVolatileKey(Buffer, KeyHandle);
}

static
VOID
CmpSetStringValue(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    ZwSetValueKey(KeyHandle,
                  &ValueName,
                  0,
                  REG_SZ,
                  (PVOID)Value,
                  (ULONG)(wcslen(Value) + 1) * sizeof(WCHAR));
}

/* Firmware strings are often padded, the registry keeps them without the spaces */
static
VOID
CmpSetFirmwareString(
    _In_ HANDLE KeyHandle,
    _In_ PCWSTR Name,
    _In_ PCSTR Value)
{
    WCHAR Buffer[64];
    ULONG Length;

    while (*Value == ' ')
        Value++;

    for (Length = 0; (Value[Length] != ANSI_NULL) && (Length < RTL_NUMBER_OF(Buffer) - 1); Length++)
        Buffer[Length] = (WCHAR)(UCHAR)Value[Length];

    while ((Length != 0) && (Buffer[Length - 1] == L' '))
        Length--;

    if (Length == 0)
        return;

    Buffer[Length] = UNICODE_NULL;
    CmpSetStringValue(KeyHandle, Name, Buffer);
}

static
USHORT
CmpGetSmbiosWord(
    _In_ PSMBIOS_STRUCTURE_HEADER Header,
    _In_ UCHAR Offset)
{
    if (Header->Length < Offset + sizeof(USHORT))
        return 0;

    return *(USHORT UNALIGNED *)((PUCHAR)Header + Offset);
}

static
PCSTR
CmpGetSmbiosStringField(
    _In_ PSMBIOS_STRUCTURE_HEADER Header,
    _In_ UCHAR Offset)
{
    if (Header->Length <= Offset)
        return NULL;

    return ExpGetSmbiosString(Header, ((PUCHAR)Header)[Offset]);
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
CmpInitializeMachineDependentConfiguration(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PSMBIOS_STRUCTURE_HEADER Processor;
    PCSTR Manufacturer = NULL, Version = NULL;
    UNICODE_STRING ValueName;
    WCHAR Identifier[64];
    HANDLE KeyHandle;
    PKPRCB Prcb;
    ULONG64 Midr;
    ULONG i, Data, MHz = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(LoaderBlock);

    /* ARM64 has no brand string or speed register, the firmware describes the processor */
    Processor = ExpFindSmbiosStructure(SMBIOS_TYPE_PROCESSOR);
    if (Processor)
    {
        Manufacturer = CmpGetSmbiosStringField(Processor, CMP_PROCESSOR_MANUFACTURER);
        Version = CmpGetSmbiosStringField(Processor, CMP_PROCESSOR_VERSION);

        /* An unknown current speed is reported as 0 */
        MHz = CmpGetSmbiosWord(Processor, CMP_PROCESSOR_CURRENT_SPEED);
        if (MHz == 0)
            MHz = CmpGetSmbiosWord(Processor, CMP_PROCESSOR_MAX_SPEED);
    }

    for (i = 0; i < (ULONG)KeNumberProcessors; i++)
    {
        Prcb = KiProcessorBlock[i];
        Midr = Prcb->ProcessorState.ArchState.Midr_El1;

        /* MIDR_EL1 splits into implementer, architecture, part number and revision */
        RtlStringCbPrintfW(Identifier,
                           sizeof(Identifier),
                           L"ARMv8 (64-bit) Family 8 Model %X Revision %3X",
                           (ULONG)((Midr >> 4) & 0xFFF),
                           (ULONG)(((Midr >> 20) & 0xF) << 8 | (Midr & 0xF)));

        Status = CmpCreateProcessorKey(L"CentralProcessor", i, &KeyHandle);
        if (!NT_SUCCESS(Status))
            return Status;

        CmpSetStringValue(KeyHandle, L"Identifier", Identifier);

        CmpSetStringValue(KeyHandle, L"ProcessorNameString", L"ARM64 Processor");
        if (Version)
            CmpSetFirmwareString(KeyHandle, L"ProcessorNameString", Version);

        RtlStringCbPrintfW(Identifier, sizeof(Identifier), L"CP %02X", (ULONG)((Midr >> 24) & 0xFF));
        CmpSetStringValue(KeyHandle, L"VendorIdentifier", Identifier);
        if (Manufacturer)
            CmpSetFirmwareString(KeyHandle, L"VendorIdentifier", Manufacturer);

        /* Power information reads the speed from the PRCB too */
        if (Prcb->MHz == 0)
            Prcb->MHz = MHz;

        Data = Prcb->MHz;
        RtlInitUnicodeString(&ValueName, L"~MHz");
        ZwSetValueKey(KeyHandle, &ValueName, 0, REG_DWORD, &Data, sizeof(Data));

        Data = Prcb->FeatureBits;
        RtlInitUnicodeString(&ValueName, L"FeatureSet");
        ZwSetValueKey(KeyHandle, &ValueName, 0, REG_DWORD, &Data, sizeof(Data));
        ZwClose(KeyHandle);

        Status = CmpCreateProcessorKey(L"FloatingPointProcessor", i, &KeyHandle);
        if (NT_SUCCESS(Status))
        {
            CmpSetStringValue(KeyHandle, L"Identifier", L"ARMv8 Advanced SIMD");
            ZwClose(KeyHandle);
        }
    }

    return STATUS_SUCCESS;
}
