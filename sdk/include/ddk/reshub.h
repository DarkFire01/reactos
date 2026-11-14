/*
 * PROJECT:     ReactOS DDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource hub interface for GPIO and serial bus connections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RESOURCE_HUB_DEVICE_NAME          L"\\Device\\RESOURCE_HUB"
#define RESOURCE_HUB_SYMBOLIC_NAME        L"\\DosDevices\\RESOURCE_HUB"
#define RESOURCE_HUB_DEVICE_NAME_PREFIX   RESOURCE_HUB_DEVICE_NAME L"\\"

/* A connection file name is its 64-bit id as 16 hex digits */
#define RESOURCE_HUB_CONNECTION_FILE_SIZE  (sizeof(LARGE_INTEGER) * 2 * sizeof(WCHAR) + sizeof(UNICODE_NULL))
#define RESOURCE_HUB_CONNECTION_PATH_SIZE  (sizeof(RESOURCE_HUB_DEVICE_NAME_PREFIX) + \
                                            RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(UNICODE_NULL))
#define RESOURCE_HUB_CONNECTION_FILE_CHARS ((RESOURCE_HUB_CONNECTION_FILE_SIZE + sizeof(WCHAR) - 1) / sizeof(WCHAR))
#define RESOURCE_HUB_CONNECTION_PATH_CHARS ((RESOURCE_HUB_CONNECTION_PATH_SIZE + sizeof(WCHAR) - 1) / sizeof(WCHAR))
#define RESOURCE_HUB_FILE_SIZE  RESOURCE_HUB_CONNECTION_FILE_SIZE
#define RESOURCE_HUB_PATH_SIZE  RESOURCE_HUB_CONNECTION_PATH_SIZE
#define RESOURCE_HUB_FILE_CHARS RESOURCE_HUB_CONNECTION_FILE_CHARS
#define RESOURCE_HUB_PATH_CHARS RESOURCE_HUB_CONNECTION_PATH_CHARS

#define FILE_DEVICE_RESOURCE_HUB FILE_DEVICE_BUS_EXTENDER

#define IOCTL_RH_QUERY_CONNECTION_PROPERTIES \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x0, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_RH_ALLOCATE_CONNECTION \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x1, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_RH_FREE_CONNECTION \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x2, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_RH_UPDATE_CONNECTION_PROPERTIES \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x3, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x4, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, 0x5, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION         1
#define RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION        1
#define RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_VERSION  1
#define RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_VERSION 1

/* ACPI large resource descriptors the hub hands out */
#define GPIO_INTERRUPT_IO_DESCRIPTOR  0x8c
#define FUNCTION_CONFIG_DESCRIPTOR    0x8d
#define SERIAL_BUS_DESCRIPTOR         0x8e

#ifndef INVALID_PIN_NUMBER
#define INVALID_PIN_NUMBER 0xFFFF
#endif

#define PNP_GPIO_IRQ_DESCRIPTOR_REVISION        0x1
#define PNP_GPIO_IRQ_DESCRIPTOR_TYPE_INTERRUPT  0x0
#define PNP_GPIO_IRQ_DESCRIPTOR_TYPE_IO         0x1
#define PNP_GPIO_IRQ_RESOURCE_CONSUMER_ONLY     0x01
#define PNP_GPIO_IRQ_MODE                       0x01
#define PNP_GPIO_IRQ_POLARITY                   0x06
#define PNP_GPIO_IRQ_SHARED                     0x08
#define PNP_GPIO_IRQ_WAKE_HINT                  0x10
#define PNP_GPIO_IRQ_MODE_LEVEL                 0x00
#define PNP_GPIO_IRQ_MODE_EDGE                  0x01
#define PNP_GPIO_IRQ_POLARITY_HIGH              0x00
#define PNP_GPIO_IRQ_POLARITY_LOW               0x02
#define PNP_GPIO_IRQ_POLARITY_BOTH              0x04

#define SERIAL_BUS_I2C_DESCRIPTOR_TYPE  1
#define SERIAL_BUS_SPI_DESCRIPTOR_TYPE  2
#define SERIAL_BUS_UART_DESCRIPTOR_TYPE 3

typedef enum _RH_QUERY_CONNECTION_PROPERTIES_INPUT_TYPE
{
    ConnectionIdType,
    InterruptVectorType
} RH_QUERY_CONNECTION_PROPERTIES_INPUT_TYPE, *PRH_QUERY_CONNECTION_PROPERTIES_INPUT_TYPE;

typedef struct _RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER
{
    ULONG Version;
    RH_QUERY_CONNECTION_PROPERTIES_INPUT_TYPE QueryType;
    union
    {
        LARGE_INTEGER ConnectionId;
        ULONG InterruptVector;
    } u;
} RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER, *PRH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER;

typedef struct _RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER
{
    ULONG Version;
    ULONG PropertiesLength;
    UCHAR ConnectionProperties[ANYSIZE_ARRAY];
} RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, *PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER;

typedef struct _RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER
{
    ULONG Version;
    ULONG InterruptVector;
} RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER, *PRH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER;

typedef struct _RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER
{
    ULONG Version;
    UCHAR InitialPolarity;
} RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER, *PRH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER;

typedef struct _RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER
{
    ULONG Version;
    ULONG InterruptVector;
    BOOLEAN InitialPolarity;
} RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER, *PRH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER;

typedef struct _RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER
{
    ULONG Version;
} RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER, *PRH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER;

#include <pshpack1.h>

typedef struct _PNP_GPIO_INTERRUPT_IO_DESCRIPTOR
{
    UCHAR Tag;
    USHORT Length;
    UCHAR Revision;
    UCHAR DescriptorType;
    USHORT GeneralFlags;
    USHORT InterruptIoFlags;
    UCHAR PinConfiguration;
    USHORT DriveStrength;
    USHORT DebounceTimeout;
    USHORT PinTableOffset;
    UCHAR ResourceSourceIndex;
    USHORT ResourceSourceOffset;
    USHORT VendorDataOffset;
    USHORT VendorDataLength;
} PNP_GPIO_INTERRUPT_IO_DESCRIPTOR, *PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR;

typedef struct _PNP_FUNCTION_CONFIG_DESCRIPTOR
{
    UCHAR Tag;
    USHORT Length;
    UCHAR Revision;
    USHORT Flags;
    UCHAR PinConfiguration;
    USHORT FunctionNumber;
    USHORT PinTableOffset;
    UCHAR ResourceSourceIndex;
    USHORT ResourceSourceOffset;
    USHORT VendorDataOffset;
    USHORT VendorDataLength;
} PNP_FUNCTION_CONFIG_DESCRIPTOR, *PPNP_FUNCTION_CONFIG_DESCRIPTOR;

typedef struct _PNP_SERIAL_BUS_DESCRIPTOR
{
    UCHAR Tag;
    USHORT Length;
    UCHAR RevisionId;
    UCHAR ResourceSourceIndex;
    UCHAR SerialBusType;
    UCHAR GeneralFlags;
    USHORT TypeSpecificFlags;
    UCHAR TypeSpecificRevisionId;
    USHORT TypeDataLength;
} PNP_SERIAL_BUS_DESCRIPTOR, *PPNP_SERIAL_BUS_DESCRIPTOR;

#include <poppack.h>

#define PNP_GPIO_INTERRUPT_IO_DESCRIPTOR_LENGTH sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR)
#define PNP_FUNCTION_CONFIG_DESCRIPTOR_LENGTH   sizeof(PNP_FUNCTION_CONFIG_DESCRIPTOR)
#define PNP_SERIAL_BUS_DESCRIPTOR_LENGTH        sizeof(PNP_SERIAL_BUS_DESCRIPTOR)

#ifdef RESHUB_USE_HELPER_ROUTINES

#include <ntstrsafe.h>

/**
 * @brief Builds the hub path a connection id opens as.
 *
 * @param[in,out] FileName
 * Needs room for RESOURCE_HUB_CONNECTION_PATH_SIZE bytes.
 */
FORCEINLINE
NTSTATUS
RESOURCE_HUB_CREATE_PATH_FROM_ID(
    _Inout_ PUNICODE_STRING FileName,
    _In_ ULONG IdLowPart,
    _In_ ULONG IdHighPart)
{
    return RtlUnicodeStringPrintf(FileName, L"%s%08lx%08lx",
                                  RESOURCE_HUB_DEVICE_NAME_PREFIX, IdHighPart, IdLowPart);
}

/**
 * @brief Reads a connection id back from the 16 hex digits that start a hub file name.
 *
 * @return STATUS_INVALID_PARAMETER unless the digits are followed by the end or a backslash.
 */
FORCEINLINE
NTSTATUS
RESOURCE_HUB_ID_FROM_FILE_NAME(
    _In_z_ PCWSTR FileName,
    _Out_ PLARGE_INTEGER Id)
{
    ULONGLONG Value = 0;
    ULONG Index;
    WCHAR Digit;

    Id->QuadPart = 0;
    for (Index = 0; Index < sizeof(LARGE_INTEGER) * 2; Index++)
    {
        Digit = FileName[Index];
        if (Digit >= L'0' && Digit <= L'9')
            Value = (Value << 4) | (ULONG)(Digit - L'0');
        else if (Digit >= L'a' && Digit <= L'f')
            Value = (Value << 4) | (ULONG)(Digit - L'a' + 10);
        else if (Digit >= L'A' && Digit <= L'F')
            Value = (Value << 4) | (ULONG)(Digit - L'A' + 10);
        else
            return STATUS_INVALID_PARAMETER;
    }

    if (FileName[Index] != UNICODE_NULL && FileName[Index] != L'\\')
        return STATUS_INVALID_PARAMETER;

    Id->QuadPart = (LONGLONG)Value;
    return STATUS_SUCCESS;
}

#endif /* RESHUB_USE_HELPER_ROUTINES */

#ifdef __cplusplus
}
#endif
