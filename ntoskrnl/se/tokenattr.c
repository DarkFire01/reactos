/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Token security attributes (TokenSecurityAttributes and NtQuerySecurityAttributesToken)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
SepValueElementSize(
    _In_ USHORT ValueType)
{
    switch (ValueType)
    {
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_INT64:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_UINT64:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_BOOLEAN:
            return sizeof(ULONG64);

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
            return sizeof(UNICODE_STRING);

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
            return sizeof(TOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE);

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
            return sizeof(TOKEN_SECURITY_ATTRIBUTE_OCTET_STRING_VALUE);

        default:
            return 0;
    }
}

/* Numeric and FQBN value arrays are 8 byte aligned, the others pointer aligned */
static
ULONG
SepValueArrayAlignment(
    _In_ USHORT ValueType)
{
    switch (ValueType)
    {
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
            return sizeof(PVOID);

        default:
            return sizeof(ULONG64);
    }
}

static
PVOID
SepValueData(
    _In_ USHORT ValueType,
    _In_ PSEP_SECURITY_ATTRIBUTE_VALUE Value,
    _Out_ PULONG DataLength)
{
    switch (ValueType)
    {
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
            *DataLength = Value->String.Length;
            return Value->String.Buffer;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
            *DataLength = Value->Fqbn.Name.Length;
            return Value->Fqbn.Name.Buffer;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
            *DataLength = Value->Octet.ValueLength;
            return Value->Octet.pValue;

        default:
            *DataLength = 0;
            return NULL;
    }
}

static
VOID
SepFreeSecurityAttribute(
    _In_ _Post_invalid_ PSEP_SECURITY_ATTRIBUTE Attribute)
{
    PLIST_ENTRY Entry;

    while (!IsListEmpty(&Attribute->ValueList))
    {
        Entry = RemoveHeadList(&Attribute->ValueList);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE_VALUE, ListEntry), TAG_SE_ATTRIBUTES);
    }

    ExFreePoolWithTag(Attribute, TAG_SE_ATTRIBUTES);
}

static
PSEP_SECURITY_ATTRIBUTES
SepAllocateSecurityAttributes(VOID)
{
    PSEP_SECURITY_ATTRIBUTES Attributes;

    Attributes = ExAllocatePoolWithTag(PagedPool, sizeof(*Attributes), TAG_SE_ATTRIBUTES);
    if (Attributes == NULL)
        return NULL;

    Attributes->AttributeCount = 0;
    InitializeListHead(&Attributes->AttributeList);
    return Attributes;
}

static
PSEP_SECURITY_ATTRIBUTE
SepAllocateSecurityAttribute(
    _In_ PCUNICODE_STRING Name,
    _In_ USHORT ValueType,
    _In_ ULONG Flags)
{
    PSEP_SECURITY_ATTRIBUTE Attribute;

    Attribute = ExAllocatePoolWithTag(PagedPool, sizeof(*Attribute) + Name->Length, TAG_SE_ATTRIBUTES);
    if (Attribute == NULL)
        return NULL;

    Attribute->Name.Buffer = (PWCHAR)(Attribute + 1);
    Attribute->Name.Length = Name->Length;
    Attribute->Name.MaximumLength = Name->Length;
    RtlCopyMemory(Attribute->Name.Buffer, Name->Buffer, Name->Length);
    Attribute->ValueType = ValueType;
    Attribute->Flags = Flags;
    Attribute->ValueCount = 0;
    InitializeListHead(&Attribute->ValueList);
    return Attribute;
}

/* Copies one value, given in the TOKEN_SECURITY_ATTRIBUTE_V1 element layout of its type */
static
PSEP_SECURITY_ATTRIBUTE_VALUE
SepAllocateSecurityAttributeValue(
    _In_ USHORT ValueType,
    _In_ PVOID Element)
{
    PSEP_SECURITY_ATTRIBUTE_VALUE Value;
    PUNICODE_STRING String = NULL;
    PTOKEN_SECURITY_ATTRIBUTE_OCTET_STRING_VALUE Octet = NULL;
    PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE Fqbn = NULL;
    ULONG DataLength = 0;
    PVOID Data;

    switch (ValueType)
    {
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
            String = Element;
            DataLength = String->Length;
            break;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
            Fqbn = Element;
            DataLength = Fqbn->Name.Length;
            break;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
            Octet = Element;
            DataLength = Octet->ValueLength;
            break;
    }

    Value = ExAllocatePoolWithTag(PagedPool, sizeof(*Value) + DataLength, TAG_SE_ATTRIBUTES);
    if (Value == NULL)
        return NULL;

    RtlZeroMemory(Value, sizeof(*Value));
    Data = Value + 1;

    if (String)
    {
        Value->String.Length = String->Length;
        Value->String.MaximumLength = String->Length;
        Value->String.Buffer = Data;
        RtlCopyMemory(Data, String->Buffer, DataLength);
    }
    else if (Fqbn)
    {
        Value->Fqbn.Version = Fqbn->Version;
        Value->Fqbn.Name.Length = Fqbn->Name.Length;
        Value->Fqbn.Name.MaximumLength = Fqbn->Name.Length;
        Value->Fqbn.Name.Buffer = Data;
        RtlCopyMemory(Data, Fqbn->Name.Buffer, DataLength);
    }
    else if (Octet)
    {
        Value->Octet.ValueLength = Octet->ValueLength;
        Value->Octet.pValue = Data;
        RtlCopyMemory(Data, Octet->pValue, DataLength);
    }
    else
    {
        Value->Number = *(PULONG64)Element;
    }

    return Value;
}

/* The stored union member has the layout of the V1 element of its type */
static
PVOID
SepStoredValueElement(
    _In_ USHORT ValueType,
    _In_ PSEP_SECURITY_ATTRIBUTE_VALUE Value)
{
    switch (ValueType)
    {
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
            return &Value->String;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
            return &Value->Fqbn;

        case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
        case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
            return &Value->Octet;

        default:
            return &Value->Number;
    }
}

static
PSEP_SECURITY_ATTRIBUTE
SepFindSecurityAttribute(
    _In_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_ PCUNICODE_STRING Name)
{
    PLIST_ENTRY Entry;
    PSEP_SECURITY_ATTRIBUTE Attribute;

    for (Entry = Attributes->AttributeList.Flink;
         Entry != &Attributes->AttributeList;
         Entry = Entry->Flink)
    {
        Attribute = CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE, ListEntry);
        if (RtlEqualUnicodeString(&Attribute->Name, Name, TRUE))
            return Attribute;
    }

    return NULL;
}

static
PSEP_SECURITY_ATTRIBUTE_VALUE
SepFindSecurityAttributeValue(
    _In_ PSEP_SECURITY_ATTRIBUTE Attribute,
    _In_ PVOID Element)
{
    PLIST_ENTRY Entry;
    PSEP_SECURITY_ATTRIBUTE_VALUE Value;
    PUNICODE_STRING String = Element;
    PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE Fqbn = Element;
    PTOKEN_SECURITY_ATTRIBUTE_OCTET_STRING_VALUE Octet = Element;

    for (Entry = Attribute->ValueList.Flink;
         Entry != &Attribute->ValueList;
         Entry = Entry->Flink)
    {
        Value = CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE_VALUE, ListEntry);

        switch (Attribute->ValueType)
        {
            case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
                if (RtlEqualUnicodeString(&Value->String, String, FALSE))
                    return Value;
                break;

            case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
                if (Value->Fqbn.Version == Fqbn->Version &&
                    RtlEqualUnicodeString(&Value->Fqbn.Name, &Fqbn->Name, FALSE))
                {
                    return Value;
                }
                break;

            case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
            case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
                if (Value->Octet.ValueLength == Octet->ValueLength &&
                    RtlEqualMemory(Value->Octet.pValue, Octet->pValue, Octet->ValueLength))
                {
                    return Value;
                }
                break;

            default:
                if (Value->Number == *(PULONG64)Element)
                    return Value;
                break;
        }
    }

    return NULL;
}

static
PVOID
SepInputValueElement(
    _In_ PTOKEN_SECURITY_ATTRIBUTE_V1 Input,
    _In_ ULONG Index)
{
    return (PUCHAR)Input->Values.pInt64 + Index * SepValueElementSize(Input->ValueType);
}

static
VOID
SepRemoveSecurityAttribute(
    _Inout_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_ _Post_invalid_ PSEP_SECURITY_ATTRIBUTE Attribute)
{
    RemoveEntryList(&Attribute->ListEntry);
    Attributes->AttributeCount--;
    SepFreeSecurityAttribute(Attribute);
}

static
NTSTATUS
SepAddSecurityAttribute(
    _Inout_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_ PTOKEN_SECURITY_ATTRIBUTE_V1 Input)
{
    PSEP_SECURITY_ATTRIBUTE Attribute;
    PSEP_SECURITY_ATTRIBUTE_VALUE Value;
    PVOID Element;
    ULONG Index;

    if (Input->ValueCount == 0)
        return STATUS_INVALID_PARAMETER;

    /* An existing attribute keeps its type and flags, new values join it */
    Attribute = SepFindSecurityAttribute(Attributes, &Input->Name);
    if (Attribute == NULL)
    {
        Attribute = SepAllocateSecurityAttribute(&Input->Name, Input->ValueType, Input->Flags);
        if (Attribute == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        InsertTailList(&Attributes->AttributeList, &Attribute->ListEntry);
        Attributes->AttributeCount++;
    }

    if (Attribute->ValueType != Input->ValueType)
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < Input->ValueCount; Index++)
    {
        Element = SepInputValueElement(Input, Index);
        if (SepFindSecurityAttributeValue(Attribute, Element))
            return STATUS_OBJECT_NAME_COLLISION;

        Value = SepAllocateSecurityAttributeValue(Attribute->ValueType, Element);
        if (Value == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        InsertTailList(&Attribute->ValueList, &Value->ListEntry);
        Attribute->ValueCount++;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
SepDeleteSecurityAttribute(
    _Inout_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_ PTOKEN_SECURITY_ATTRIBUTE_V1 Input)
{
    PSEP_SECURITY_ATTRIBUTE Attribute;
    PSEP_SECURITY_ATTRIBUTE_VALUE Value;
    ULONG Index;

    Attribute = SepFindSecurityAttribute(Attributes, &Input->Name);
    if (Attribute == NULL)
        return STATUS_NOT_FOUND;

    /* No values given means the whole attribute goes */
    if (Input->ValueCount == 0)
    {
        SepRemoveSecurityAttribute(Attributes, Attribute);
        return STATUS_SUCCESS;
    }

    if (Attribute->ValueType != Input->ValueType)
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < Input->ValueCount; Index++)
    {
        Value = SepFindSecurityAttributeValue(Attribute, SepInputValueElement(Input, Index));
        if (Value == NULL)
            return STATUS_NOT_FOUND;

        RemoveEntryList(&Value->ListEntry);
        Attribute->ValueCount--;
        ExFreePoolWithTag(Value, TAG_SE_ATTRIBUTES);
    }

    if (Attribute->ValueCount == 0)
        SepRemoveSecurityAttribute(Attributes, Attribute);

    return STATUS_SUCCESS;
}

static
NTSTATUS
SepReplaceSecurityAttribute(
    _Inout_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_ PTOKEN_SECURITY_ATTRIBUTE_V1 Input)
{
    PSEP_SECURITY_ATTRIBUTE Attribute;

    Attribute = SepFindSecurityAttribute(Attributes, &Input->Name);
    if (Attribute)
        SepRemoveSecurityAttribute(Attributes, Attribute);

    if (Input->ValueCount == 0)
        return STATUS_SUCCESS;

    return SepAddSecurityAttribute(Attributes, Input);
}

static
BOOLEAN
SepIsValidCountedString(
    _In_ PCUNICODE_STRING String)
{
    return String->Length != 0 &&
           String->MaximumLength != 0 &&
           String->Length <= String->MaximumLength &&
           String->Buffer != NULL;
}

static
NTSTATUS
SepValidateSecurityAttributes(
    _In_ PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Information,
    _In_ PTOKEN_SECURITY_ATTRIBUTE_OPERATION Operations)
{
    PTOKEN_SECURITY_ATTRIBUTE_V1 Attribute;
    BOOLEAN ReplaceAll;
    ULONG Index, Value;

    if (Information->Version != TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1 ||
        Information->Reserved != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ReplaceAll = (Operations[0] == TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE_ALL);

    if ((Information->AttributeCount != 0) != (Information->Attribute.pAttributeV1 != NULL))
        return STATUS_INVALID_PARAMETER;

    /* Only replacing everything may leave the token without attributes */
    if (!ReplaceAll && Information->AttributeCount == 0)
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < Information->AttributeCount; Index++)
    {
        Attribute = &Information->Attribute.pAttributeV1[Index];

        if (!SepIsValidCountedString(&Attribute->Name) ||
            Attribute->Reserved != 0 ||
            (Attribute->Flags & ~(TOKEN_SECURITY_ATTRIBUTE_VALID_FLAGS | TOKEN_SECURITY_ATTRIBUTE_CUSTOM_FLAGS)) != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (Index != 0 && !ReplaceAll &&
            Operations[Index] == TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE_ALL)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (((Index == 0 && ReplaceAll) ||
             (Index != 0 && !ReplaceAll && Operations[Index] == TOKEN_SECURITY_ATTRIBUTE_OPERATION_ADD)) &&
            Attribute->ValueCount == 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (SepValueElementSize(Attribute->ValueType) == 0)
            return STATUS_INVALID_PARAMETER;

        if (Attribute->ValueCount != 0 && Attribute->Values.pInt64 == NULL)
            return STATUS_INVALID_PARAMETER;

        for (Value = 0; Value < Attribute->ValueCount; Value++)
        {
            switch (Attribute->ValueType)
            {
                case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
                    if (!SepIsValidCountedString(&Attribute->Values.pString[Value]))
                        return STATUS_INVALID_PARAMETER;
                    break;

                case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
                    if (!SepIsValidCountedString(&Attribute->Values.pFqbn[Value].Name))
                        return STATUS_INVALID_PARAMETER;
                    break;

                case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
                case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
                    if (Attribute->Values.pOctetString[Value].pValue == NULL ||
                        Attribute->Values.pOctetString[Value].ValueLength == 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    break;

                case TOKEN_SECURITY_ATTRIBUTE_TYPE_BOOLEAN:
                    if (Attribute->Values.pUint64[Value] > 1)
                        return STATUS_INVALID_PARAMETER;
                    break;
            }
        }
    }

    return STATUS_SUCCESS;
}

/*
 * Lays out a TOKEN_SECURITY_ATTRIBUTES_INFORMATION: the header, the V1 array, then
 * for each attribute its name followed by its aligned value array and value data.
 * Without a buffer it only measures.
 */
static
NTSTATUS
SepLayoutSecurityAttributes(
    _In_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_reads_opt_(NameCount) PCUNICODE_STRING Names,
    _In_ ULONG NameCount,
    _Out_writes_bytes_opt_(*Needed) PUCHAR Buffer,
    _Out_ PULONG Needed)
{
    PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Information = (PVOID)Buffer;
    PTOKEN_SECURITY_ATTRIBUTE_V1 Out = NULL;
    PSEP_SECURITY_ATTRIBUTE Attribute;
    PSEP_SECURITY_ATTRIBUTE_VALUE Value;
    PLIST_ENTRY Entry, ValueEntry;
    ULONG64 Offset;
    ULONG Count, Index, ElementSize, DataLength, ValueIndex;
    PVOID Data;
    PUCHAR Element;

    Count = Names ? NameCount : Attributes->AttributeCount;
    Offset = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION) + (ULONG64)Count * sizeof(TOKEN_SECURITY_ATTRIBUTE_V1);

    if (Buffer)
    {
        Information->Version = TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1;
        Information->Reserved = 0;
        Information->AttributeCount = Count;
        Information->Attribute.pAttributeV1 = Count ? (PVOID)(Information + 1) : NULL;
        Out = Information->Attribute.pAttributeV1;
    }

    Entry = Attributes->AttributeList.Flink;
    for (Index = 0; Index < Count; Index++)
    {
        if (Names)
        {
            Attribute = SepFindSecurityAttribute(Attributes, &Names[Index]);
            if (Attribute == NULL)
                return STATUS_NOT_FOUND;
        }
        else
        {
            Attribute = CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE, ListEntry);
            Entry = Entry->Flink;
        }

        Offset = ALIGN_UP_BY(Offset, sizeof(WCHAR));
        if (Out)
        {
            Out[Index].Name.Length = Attribute->Name.Length;
            Out[Index].Name.MaximumLength = Attribute->Name.Length;
            Out[Index].Name.Buffer = (PWCHAR)(Buffer + Offset);
            Out[Index].ValueType = Attribute->ValueType;
            Out[Index].Reserved = 0;
            Out[Index].Flags = Attribute->Flags;
            Out[Index].ValueCount = Attribute->ValueCount;
            RtlCopyMemory(Buffer + Offset, Attribute->Name.Buffer, Attribute->Name.Length);
        }
        Offset += Attribute->Name.Length;

        ElementSize = SepValueElementSize(Attribute->ValueType);
        if (ElementSize == 0)
            return STATUS_INVALID_PARAMETER;

        Offset = ALIGN_UP_BY(Offset, SepValueArrayAlignment(Attribute->ValueType));
        Element = Buffer ? Buffer + Offset : NULL;
        if (Out)
            Out[Index].Values.pInt64 = (PLONG64)Element;
        Offset += (ULONG64)Attribute->ValueCount * ElementSize;

        ValueIndex = 0;
        for (ValueEntry = Attribute->ValueList.Flink;
             ValueEntry != &Attribute->ValueList;
             ValueEntry = ValueEntry->Flink, ValueIndex++)
        {
            Value = CONTAINING_RECORD(ValueEntry, SEP_SECURITY_ATTRIBUTE_VALUE, ListEntry);
            Data = SepValueData(Attribute->ValueType, Value, &DataLength);

            if (Element)
            {
                PUCHAR Target = Element + ValueIndex * ElementSize;

                switch (Attribute->ValueType)
                {
                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
                        ((PUNICODE_STRING)Target)->Length = (USHORT)DataLength;
                        ((PUNICODE_STRING)Target)->MaximumLength = (USHORT)DataLength;
                        ((PUNICODE_STRING)Target)->Buffer = (PWCHAR)(Buffer + Offset);
                        break;

                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
                        ((PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE)Target)->Version = Value->Fqbn.Version;
                        ((PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE)Target)->Name.Length = (USHORT)DataLength;
                        ((PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE)Target)->Name.MaximumLength = (USHORT)DataLength;
                        ((PTOKEN_SECURITY_ATTRIBUTE_FQBN_VALUE)Target)->Name.Buffer = (PWCHAR)(Buffer + Offset);
                        break;

                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
                        ((PTOKEN_SECURITY_ATTRIBUTE_OCTET_STRING_VALUE)Target)->pValue = Buffer + Offset;
                        ((PTOKEN_SECURITY_ATTRIBUTE_OCTET_STRING_VALUE)Target)->ValueLength = DataLength;
                        break;

                    default:
                        *(PULONG64)Target = Value->Number;
                        break;
                }

                if (DataLength)
                    RtlCopyMemory(Buffer + Offset, Data, DataLength);
            }

            Offset += DataLength;
        }
    }

    if (Offset > MAXULONG)
        return STATUS_INTEGER_OVERFLOW;

    *Needed = (ULONG)Offset;
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Frees a token's security attribute list and every value it holds.
 */
VOID
SepFreeSecurityAttributes(
    _In_opt_ _Post_invalid_ PSEP_SECURITY_ATTRIBUTES Attributes)
{
    PLIST_ENTRY Entry;

    if (Attributes == NULL)
        return;

    while (!IsListEmpty(&Attributes->AttributeList))
    {
        Entry = RemoveHeadList(&Attributes->AttributeList);
        SepFreeSecurityAttribute(CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE, ListEntry));
    }

    ExFreePoolWithTag(Attributes, TAG_SE_ATTRIBUTES);
}

/**
 * @brief
 * Deep copies a security attribute list. Process tokens drop the attributes
 * marked TOKEN_SECURITY_ATTRIBUTE_NON_INHERITABLE.
 */
NTSTATUS
SepDuplicateSecurityAttributes(
    _In_opt_ PSEP_SECURITY_ATTRIBUTES Source,
    _In_ BOOLEAN SkipNonInheritable,
    _Out_ PSEP_SECURITY_ATTRIBUTES *Copy)
{
    PSEP_SECURITY_ATTRIBUTES Attributes;
    PSEP_SECURITY_ATTRIBUTE Attribute, NewAttribute;
    PSEP_SECURITY_ATTRIBUTE_VALUE Value, NewValue;
    PLIST_ENTRY Entry, ValueEntry;

    *Copy = NULL;
    if (Source == NULL || Source->AttributeCount == 0)
        return STATUS_SUCCESS;

    Attributes = SepAllocateSecurityAttributes();
    if (Attributes == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Entry = Source->AttributeList.Flink;
         Entry != &Source->AttributeList;
         Entry = Entry->Flink)
    {
        Attribute = CONTAINING_RECORD(Entry, SEP_SECURITY_ATTRIBUTE, ListEntry);
        if (SkipNonInheritable && (Attribute->Flags & TOKEN_SECURITY_ATTRIBUTE_NON_INHERITABLE))
            continue;

        NewAttribute = SepAllocateSecurityAttribute(&Attribute->Name, Attribute->ValueType, Attribute->Flags);
        if (NewAttribute == NULL)
            goto Failure;

        InsertTailList(&Attributes->AttributeList, &NewAttribute->ListEntry);
        Attributes->AttributeCount++;

        for (ValueEntry = Attribute->ValueList.Flink;
             ValueEntry != &Attribute->ValueList;
             ValueEntry = ValueEntry->Flink)
        {
            Value = CONTAINING_RECORD(ValueEntry, SEP_SECURITY_ATTRIBUTE_VALUE, ListEntry);
            NewValue = SepAllocateSecurityAttributeValue(Attribute->ValueType,
                                                         SepStoredValueElement(Attribute->ValueType, Value));
            if (NewValue == NULL)
                goto Failure;

            InsertTailList(&NewAttribute->ValueList, &NewValue->ListEntry);
            NewAttribute->ValueCount++;
        }
    }

    if (Attributes->AttributeCount == 0)
    {
        SepFreeSecurityAttributes(Attributes);
        return STATUS_SUCCESS;
    }

    *Copy = Attributes;
    return STATUS_SUCCESS;

Failure:
    SepFreeSecurityAttributes(Attributes);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/**
 * @brief
 * Writes a token's security attributes, all of them or the named ones in the
 * order asked, as a self-relative TOKEN_SECURITY_ATTRIBUTES_INFORMATION.
 *
 * @remarks
 * The caller holds the token lock and guards a user mode buffer with SEH.
 */
NTSTATUS
SepQuerySecurityAttributes(
    _In_opt_ PSEP_SECURITY_ATTRIBUTES Attributes,
    _In_reads_opt_(NameCount) PCUNICODE_STRING Names,
    _In_ ULONG NameCount,
    _Out_writes_bytes_to_opt_(Length, *ReturnLength) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength)
{
    PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Information = Buffer;
    NTSTATUS Status;
    ULONG Needed, Index;

    *ReturnLength = 0;

    /* A token without attributes answers with an empty header, whatever was asked */
    if (Attributes == NULL || Attributes->AttributeCount == 0)
    {
        *ReturnLength = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
        if (Length < sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;

        RtlZeroMemory(Information, sizeof(*Information));
        Information->Version = TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1;
        return STATUS_SUCCESS;
    }

    /* A name longer than its maximum is not rejected, it just matches nothing */
    for (Index = 0; Names && Index < NameCount; Index++)
    {
        if (Names[Index].Length == 0 || Names[Index].Buffer == NULL)
            return STATUS_INVALID_PARAMETER;
    }

    Status = SepLayoutSecurityAttributes(Attributes, Names, NameCount, NULL, &Needed);
    if (!NT_SUCCESS(Status))
        return Status;

    *ReturnLength = Needed;
    if (Length < Needed)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Buffer, Length);
    return SepLayoutSecurityAttributes(Attributes, Names, NameCount, Buffer, &Needed);
}

/**
 * @brief
 * Releases what SepCaptureSecurityAttributesAndOperations captured.
 */
VOID
SepReleaseSecurityAttributesAndOperations(
    _In_opt_ _Post_invalid_ PTOKEN_SECURITY_ATTRIBUTES_INFORMATION CapturedAttributes,
    _In_opt_ _Post_invalid_ PTOKEN_SECURITY_ATTRIBUTE_OPERATION CapturedOperations)
{
    PTOKEN_SECURITY_ATTRIBUTE_V1 Attribute;
    ULONG Index, Value;
    PVOID Data;

    if (CapturedOperations)
        ExFreePoolWithTag(CapturedOperations, TAG_SE_ATTRIBUTES);

    if (CapturedAttributes == NULL)
        return;

    for (Index = 0; Index < CapturedAttributes->AttributeCount; Index++)
    {
        Attribute = &CapturedAttributes->Attribute.pAttributeV1[Index];
        if (Attribute->Name.Buffer)
            ExFreePoolWithTag(Attribute->Name.Buffer, TAG_SE_ATTRIBUTES);

        if (Attribute->Values.pInt64 == NULL)
            continue;

        for (Value = 0; Value < Attribute->ValueCount; Value++)
        {
            switch (Attribute->ValueType)
            {
                case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
                    Data = Attribute->Values.pString[Value].Buffer;
                    break;

                case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
                    Data = Attribute->Values.pFqbn[Value].Name.Buffer;
                    break;

                case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
                case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
                    Data = Attribute->Values.pOctetString[Value].pValue;
                    break;

                default:
                    Data = NULL;
                    break;
            }

            if (Data)
                ExFreePoolWithTag(Data, TAG_SE_ATTRIBUTES);
        }

        ExFreePoolWithTag(Attribute->Values.pInt64, TAG_SE_ATTRIBUTES);
    }

    if (CapturedAttributes->Attribute.pAttributeV1)
        ExFreePoolWithTag(CapturedAttributes->Attribute.pAttributeV1, TAG_SE_ATTRIBUTES);

    ExFreePoolWithTag(CapturedAttributes, TAG_SE_ATTRIBUTES);
}

static
PVOID
SepCaptureBuffer(
    _In_reads_bytes_opt_(Length) PVOID Source,
    _In_ ULONG Length,
    _In_ ULONG Alignment,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    PVOID Copy;

    if (Source == NULL || Length == 0)
        return NULL;

    if (PreviousMode != KernelMode)
        ProbeForRead(Source, Length, Alignment);

    Copy = ExAllocatePoolWithTag(PagedPool, Length, TAG_SE_ATTRIBUTES);
    if (Copy == NULL)
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);

    RtlCopyMemory(Copy, Source, Length);
    return Copy;
}

/**
 * @brief
 * Captures the TOKEN_SECURITY_ATTRIBUTES_AND_OPERATION_INFORMATION given to
 * NtSetInformationToken(TokenSecurityAttributes) into pool, values and all.
 * Validation happens when the operations are applied.
 */
NTSTATUS
SepCaptureSecurityAttributesAndOperations(
    _In_ PTOKEN_SECURITY_ATTRIBUTES_AND_OPERATION_INFORMATION Input,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PTOKEN_SECURITY_ATTRIBUTES_INFORMATION *CapturedAttributes,
    _Out_ PTOKEN_SECURITY_ATTRIBUTE_OPERATION *CapturedOperations)
{
    PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Attributes = NULL;
    PTOKEN_SECURITY_ATTRIBUTE_OPERATION Operations = NULL;
    TOKEN_SECURITY_ATTRIBUTES_AND_OPERATION_INFORMATION Local;
    TOKEN_SECURITY_ATTRIBUTES_INFORMATION LocalInformation;
    PTOKEN_SECURITY_ATTRIBUTE_V1 Attribute;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index, Value, Count, ValueCount, ElementSize, DataLength;
    PVOID UserName, UserValues, UserData;
    PVOID *Inner;

    *CapturedAttributes = NULL;
    *CapturedOperations = NULL;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(Input, sizeof(*Input), sizeof(ULONG));
        Local = *Input;

        if (Local.Operations == NULL)
        {
            Status = STATUS_INVALID_PARAMETER;
            _SEH2_LEAVE;
        }

        Count = 0;
        if (Local.Attributes)
        {
            if (PreviousMode != KernelMode)
                ProbeForRead(Local.Attributes, sizeof(*Local.Attributes), sizeof(ULONG));
            LocalInformation = *Local.Attributes;
            Count = LocalInformation.AttributeCount;
        }

        if (Count > MAXUSHORT)
        {
            Status = STATUS_INVALID_PARAMETER;
            _SEH2_LEAVE;
        }

        /* Every attribute has its operation, and there is always a first one */
        Operations = SepCaptureBuffer(Local.Operations,
                                      max(Count, 1) * sizeof(*Operations),
                                      sizeof(ULONG),
                                      PreviousMode);

        if (Local.Attributes == NULL)
        {
            if (Operations[0] != TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE_ALL)
                Status = STATUS_INVALID_PARAMETER;
            _SEH2_LEAVE;
        }

        Attributes = ExAllocatePoolWithTag(PagedPool, sizeof(*Attributes), TAG_SE_ATTRIBUTES);
        if (Attributes == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            _SEH2_LEAVE;
        }

        /* The release routine frees only what the counts cover, so they grow as capture goes */
        *Attributes = LocalInformation;
        Attributes->AttributeCount = 0;
        Attributes->Attribute.pAttributeV1 = SepCaptureBuffer(LocalInformation.Attribute.pAttributeV1,
                                                              Count * sizeof(TOKEN_SECURITY_ATTRIBUTE_V1),
                                                              sizeof(ULONG),
                                                              PreviousMode);
        if (Attributes->Attribute.pAttributeV1 == NULL)
            _SEH2_LEAVE;

        for (Index = 0; Index < Count; Index++)
        {
            Attribute = &Attributes->Attribute.pAttributeV1[Index];
            UserName = Attribute->Name.Buffer;
            UserValues = Attribute->Values.pInt64;
            ValueCount = Attribute->ValueCount;
            ElementSize = SepValueElementSize(Attribute->ValueType);

            Attribute->Name.Buffer = NULL;
            Attribute->Values.pInt64 = NULL;
            Attribute->ValueCount = 0;
            Attributes->AttributeCount = Index + 1;

            if (ElementSize && ValueCount > MAXUSHORT)
            {
                Status = STATUS_INVALID_PARAMETER;
                _SEH2_LEAVE;
            }

            Attribute->Name.Buffer = SepCaptureBuffer(UserName, Attribute->Name.Length, sizeof(WCHAR), PreviousMode);
            if (ElementSize)
                Attribute->Values.pInt64 = SepCaptureBuffer(UserValues, ValueCount * ElementSize, sizeof(ULONG), PreviousMode);

            /* Values the validation must still see as missing keep their count */
            if (Attribute->Values.pInt64 == NULL)
            {
                Attribute->ValueCount = ValueCount;
                continue;
            }

            for (Value = 0; Value < ValueCount; Value++)
            {
                switch (Attribute->ValueType)
                {
                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING:
                        Inner = (PVOID *)&Attribute->Values.pString[Value].Buffer;
                        DataLength = Attribute->Values.pString[Value].Length;
                        break;

                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_FQBN:
                        Inner = (PVOID *)&Attribute->Values.pFqbn[Value].Name.Buffer;
                        DataLength = Attribute->Values.pFqbn[Value].Name.Length;
                        break;

                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_SID:
                    case TOKEN_SECURITY_ATTRIBUTE_TYPE_OCTET_STRING:
                        Inner = &Attribute->Values.pOctetString[Value].pValue;
                        DataLength = Attribute->Values.pOctetString[Value].ValueLength;
                        break;

                    default:
                        Inner = NULL;
                        DataLength = 0;
                        break;
                }

                if (Inner)
                {
                    UserData = *Inner;
                    *Inner = NULL;
                    Attribute->ValueCount = Value + 1;
                    *Inner = SepCaptureBuffer(UserData, DataLength, sizeof(UCHAR), PreviousMode);
                }
            }

            Attribute->ValueCount = ValueCount;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        SepReleaseSecurityAttributesAndOperations(Attributes, Operations);
        return Status;
    }

    *CapturedAttributes = Attributes;
    *CapturedOperations = Operations;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Applies captured attribute operations to a token. The operations run on a
 * copy that only replaces the token's list when all of them succeed.
 *
 * @remarks
 * The caller holds the token lock exclusively.
 */
NTSTATUS
SepSetSecurityAttributesToken(
    _Inout_ PTOKEN Token,
    _In_ PTOKEN_SECURITY_ATTRIBUTE_OPERATION Operations,
    _In_opt_ PTOKEN_SECURITY_ATTRIBUTES_INFORMATION Attributes)
{
    PSEP_SECURITY_ATTRIBUTES Working;
    PTOKEN_SECURITY_ATTRIBUTE_V1 Attribute;
    BOOLEAN ReplaceAll;
    NTSTATUS Status;
    ULONG Index;

    ReplaceAll = (Operations[0] == TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE_ALL);

    if (Attributes == NULL)
    {
        if (!ReplaceAll)
            return STATUS_INVALID_PARAMETER;

        SepFreeSecurityAttributes(Token->pSecurityAttributes);
        Token->pSecurityAttributes = NULL;
        return STATUS_SUCCESS;
    }

    Status = SepValidateSecurityAttributes(Attributes, Operations);
    if (!NT_SUCCESS(Status))
        return Status;

    if (ReplaceAll)
    {
        Working = NULL;
    }
    else
    {
        Status = SepDuplicateSecurityAttributes(Token->pSecurityAttributes, FALSE, &Working);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (Working == NULL)
    {
        Working = SepAllocateSecurityAttributes();
        if (Working == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (Index = 0; Index < Attributes->AttributeCount && NT_SUCCESS(Status); Index++)
    {
        Attribute = &Attributes->Attribute.pAttributeV1[Index];

        if (ReplaceAll)
        {
            Status = SepReplaceSecurityAttribute(Working, Attribute);
            continue;
        }

        switch (Operations[Index])
        {
            case TOKEN_SECURITY_ATTRIBUTE_OPERATION_NONE:
                break;

            case TOKEN_SECURITY_ATTRIBUTE_OPERATION_ADD:
                Status = SepAddSecurityAttribute(Working, Attribute);
                break;

            case TOKEN_SECURITY_ATTRIBUTE_OPERATION_DELETE:
                Status = SepDeleteSecurityAttribute(Working, Attribute);
                break;

            case TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE:
                Status = SepReplaceSecurityAttribute(Working, Attribute);
                break;

            default:
                Status = STATUS_INVALID_PARAMETER;
                break;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        SepFreeSecurityAttributes(Working);
        return Status;
    }

    SepFreeSecurityAttributes(Token->pSecurityAttributes);
    if (Working->AttributeCount == 0)
    {
        SepFreeSecurityAttributes(Working);
        Working = NULL;
    }

    Token->pSecurityAttributes = Working;
    return STATUS_SUCCESS;
}

/* SYSTEM CALLS ***************************************************************/

/**
 * @brief
 * Queries the security attributes of a token, all of them or the named ones.
 *
 * @param[in] TokenHandle
 * Token handle with TOKEN_QUERY access.
 *
 * @param[in] Attributes
 * Names of the attributes to return, in the order to return them. NULL returns all.
 *
 * @param[in] NumberOfAttributes
 * Number of names.
 *
 * @param[out] Buffer
 * Receives a self-relative TOKEN_SECURITY_ATTRIBUTES_INFORMATION.
 *
 * @param[in] Length
 * Size of the buffer, in bytes.
 *
 * @param[out] ReturnLength
 * Receives the size the result needs.
 *
 * @return
 * STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, STATUS_NOT_FOUND when a named
 * attribute is not in the token, or another failure code.
 */
NTSTATUS
NTAPI
NtQuerySecurityAttributesToken(
    _In_ HANDLE TokenHandle,
    _In_reads_opt_(NumberOfAttributes) PUNICODE_STRING Attributes,
    _In_ ULONG NumberOfAttributes,
    _Out_writes_bytes_to_opt_(Length, *ReturnLength) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PUNICODE_STRING Names = NULL;
    PTOKEN Token;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Needed = 0;
    ULONG Index;

    PAGED_CODE();

    if ((Length != 0) != (Buffer != NULL))
        return STATUS_INVALID_PARAMETER;

    /* Names come with a count and a count with names */
    if ((Attributes != NULL) != (NumberOfAttributes != 0) || NumberOfAttributes > MAXUSHORT)
        return STATUS_INVALID_PARAMETER;

    /* Capture the names, each into its own buffer */
    if (Attributes)
    {
        Names = ExAllocatePoolZero(PagedPool, NumberOfAttributes * sizeof(UNICODE_STRING), TAG_SE_ATTRIBUTES);
        if (Names == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            if (Length)
                ProbeForWrite(Buffer, Length, sizeof(ULONG));
            ProbeForWriteUlong(ReturnLength);
            if (Names)
                ProbeForRead(Attributes, NumberOfAttributes * sizeof(UNICODE_STRING), sizeof(ULONG));
        }

        for (Index = 0; Names && Index < NumberOfAttributes; Index++)
        {
            UNICODE_STRING Name = Attributes[Index];

            Names[Index].Length = Name.Length;
            Names[Index].MaximumLength = Name.MaximumLength;
            Names[Index].Buffer = SepCaptureBuffer(Name.Buffer, Name.Length, sizeof(WCHAR), PreviousMode);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(TokenHandle,
                                           TOKEN_QUERY,
                                           SeTokenObjectType,
                                           PreviousMode,
                                           (PVOID*)&Token,
                                           NULL);
    }

    if (NT_SUCCESS(Status))
    {
        SepAcquireTokenLockShared(Token);

        _SEH2_TRY
        {
            Status = SepQuerySecurityAttributes(Token->pSecurityAttributes,
                                                Names,
                                                NumberOfAttributes,
                                                Buffer,
                                                Length,
                                                &Needed);
            *ReturnLength = Needed;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        SepReleaseTokenLock(Token);
        ObDereferenceObject(Token);
    }

    for (Index = 0; Names && Index < NumberOfAttributes; Index++)
    {
        if (Names[Index].Buffer)
            ExFreePoolWithTag(Names[Index].Buffer, TAG_SE_ATTRIBUTES);
    }

    if (Names)
        ExFreePoolWithTag(Names, TAG_SE_ATTRIBUTES);

    return Status;
}

/* EOF */
