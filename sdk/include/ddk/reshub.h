/*
 * PROJECT:     ReactOS DDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub consumer definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#ifndef _RESHUB_H_
#define _RESHUB_H_

#ifdef RESHUB_USE_HELPER_ROUTINES
#include <ntstrsafe.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Resource Hub maps an ACPI 5.0 Connection() descriptor to a 64 bit
 * connection id. A device whose _CRS carries such a descriptor gets a
 * CmResourceTypeConnection resource holding that id. The driver turns the id
 * into a path with RESOURCE_HUB_CREATE_PATH_FROM_ID, opens it, and lands on the
 * bus controller that owns the connection. The controller then asks the hub for
 * the raw descriptor with IOCTL_RH_QUERY_CONNECTION_PROPERTIES.
 */

#define RESOURCE_HUB_DEVICE_NAME        L"\\Device\\RESOURCE_HUB"
#define RESOURCE_HUB_SYMBOLIC_NAME      L"\\DosDevices\\RESOURCE_HUB"
#define RESOURCE_HUB_DEVICE_NAME_PREFIX RESOURCE_HUB_DEVICE_NAME L"\\"

/* A connection file name is the id as a zero padded 16 digit hex string */
#define RESOURCE_HUB_CONNECTION_FILE_SIZE \
    ((sizeof(LARGE_INTEGER) * 2 * sizeof(WCHAR)) + sizeof(UNICODE_NULL))

#define RESOURCE_HUB_CONNECTION_PATH_SIZE      \
    (sizeof(RESOURCE_HUB_DEVICE_NAME_PREFIX) + \
     RESOURCE_HUB_CONNECTION_FILE_SIZE -       \
     sizeof(UNICODE_NULL))

#define RESOURCE_HUB_CONNECTION_FILE_CHARS \
    ((RESOURCE_HUB_CONNECTION_FILE_SIZE + sizeof(WCHAR) - 1) / sizeof(WCHAR))

#define RESOURCE_HUB_CONNECTION_PATH_CHARS \
    ((RESOURCE_HUB_CONNECTION_PATH_SIZE + sizeof(WCHAR) - 1) / sizeof(WCHAR))

#define RESOURCE_HUB_FILE_SIZE  RESOURCE_HUB_CONNECTION_FILE_SIZE
#define RESOURCE_HUB_PATH_SIZE  RESOURCE_HUB_CONNECTION_PATH_SIZE
#define RESOURCE_HUB_FILE_CHARS RESOURCE_HUB_CONNECTION_FILE_CHARS
#define RESOURCE_HUB_PATH_CHARS RESOURCE_HUB_CONNECTION_PATH_CHARS

#define FILE_DEVICE_RESOURCE_HUB (FILE_DEVICE_BUS_EXTENDER)

#define IOCTL_RH_QUERY_CONNECTION_PROPERTIES \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB,       \
             0x0,                            \
             METHOD_BUFFERED,                \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_RH_ALLOCATE_CONNECTION   \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, \
             0x1,                      \
             METHOD_BUFFERED,          \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_RH_FREE_CONNECTION       \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB, \
             0x2,                      \
             METHOD_BUFFERED,          \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_RH_UPDATE_CONNECTION_PROPERTIES \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB,        \
             0x3,                             \
             METHOD_BUFFERED,                 \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB,              \
             0x4,                                   \
             METHOD_BUFFERED,                       \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY \
    CTL_CODE(FILE_DEVICE_RESOURCE_HUB,               \
             0x5,                                    \
             METHOD_BUFFERED,                        \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION            1
#define RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION           1
#define RH_ALLOCATE_CONNECTION_INPUT_VERSION                    1
#define RH_ALLOCATE_CONNECTION_OUTPUT_VERSION                   1
#define RH_FREE_CONNECTION_INPUT_VERSION                        1
#define RH_FREE_CONNECTION_OUTPUT_VERSION                       1
#define RH_UPDATE_CONNECTION_PROPERTIES_INPUT_VERSION           1
#define RH_UPDATE_CONNECTION_PROPERTIES_OUTPUT_VERSION          1
#define RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_VERSION     1
#define RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_VERSION    1
#define RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_VERSION    1
#define RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_VERSION   1

#define RH_UPDATE_CONNECTIONAL_MASK_ALL 0xFFFF

/* ACPI large resource tags the hub stores and hands back verbatim */
#define FUNCTION_CONFIG_DESCRIPTOR                      0x8d
#define PNP_FUNCTION_CONFIG_DESCRIPTOR_REVISION         0x1
#define PNP_FUNCTION_CONFIG_DESCRIPTOR_REVISION_MINIMUM 0x1

#define GPIO_INTERRUPT_IO_DESCRIPTOR 0x8c

#ifndef INVALID_PIN_NUMBER
#define INVALID_PIN_NUMBER 0xFFFF
#endif

#define PNP_GPIO_IRQ_DESCRIPTOR_REVISION            0x1
#define PNP_GPIO_IRQ_DESCRIPTOR_TYPE_INTERRUPT      0x0
#define PNP_GPIO_IRQ_DESCRIPTOR_TYPE_IO             0x1
#define PNP_GPIO_IRQ_DESCRIPTOR_REVISION_MINIMUM    0x1

#define PNP_GPIO_IRQ_RESOURCE_CONSUMER_ONLY 0x01
#define PNP_GPIO_IRQ_MODE                   0x01
#define PNP_GPIO_IRQ_POLARITY               0x06
#define PNP_GPIO_IRQ_SHARED                 0x08
#define PNP_GPIO_IRQ_WAKE_HINT              0x10

#ifndef PNP_GPIO_IRQ_MODE_EDGE
#define PNP_GPIO_IRQ_MODE_EDGE      0x01
#define PNP_GPIO_IRQ_MODE_LEVEL     0x00
#define PNP_GPIO_IRQ_POLARITY_LOW   0x02
#define PNP_GPIO_IRQ_POLARITY_HIGH  0x00
#define PNP_GPIO_IRQ_POLARITY_BOTH  0x04
#endif

#define SERIAL_BUS_DESCRIPTOR             0x8e
#define SERIAL_BUS_DESCRIPTOR_REVISION    0x01
#define SERIAL_BUS_DESCRIPTOR_REVISION_V2 0x02
#define SERIAL_BUS_I2C_DESCRIPTOR_TYPE    1
#define SERIAL_BUS_SPI_DESCRIPTOR_TYPE    2
#define SERIAL_BUS_UART_DESCRIPTOR_TYPE   3
#define SERIAL_BUS_FLAG_SHARED_DESCRIPTOR 0x4

/* IOCTL_RH_QUERY_CONNECTION_PROPERTIES parameters */

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

/*
 * ConnectionProperties is the raw descriptor the firmware supplied, a
 * PNP_SERIAL_BUS_DESCRIPTOR or PNP_GPIO_INTERRUPT_IO_DESCRIPTOR followed by its
 * type specific data and resource source string.
 */
typedef struct _RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER
{
    ULONG Version;
    ULONG PropertiesLength;
    _Field_size_bytes_(PropertiesLength) UCHAR ConnectionProperties[ANYSIZE_ARRAY];
} RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, *PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER;

/* IOCTL_RH_ALLOCATE_CONNECTION and IOCTL_RH_UPDATE_CONNECTION_PROPERTIES */

typedef enum _RH_CONNECTION_PROPERTIES_INPUT_TYPE
{
    GpioIoType,
    GpioInterruptType,
    SpiConnectionType,
    I2cConnectionType,
    UartConnectionType
} RH_CONNECTION_PROPERTIES_INPUT_TYPE, *PRH_CONNECTION_PROPERTIES_INPUT_TYPE;

typedef struct _RH_GPIO_IO_CONNECTION_PARAMETERS
{
    LARGE_INTEGER ConnectionId;
    USHORT UpdateMask;
    UCHAR PinConfiguration;
    USHORT DebounceTimeout;
    USHORT DriveStrength;
} RH_GPIO_IO_CONNECTION_PARAMETERS, *PRH_GPIO_IO_CONNECTION_PARAMETERS;

typedef struct _RH_GPIO_INTERRUPT_CONNECTION_PARAMETERS
{
    ULONG InterruptVector;
    USHORT UpdateMask;
    UCHAR InterruptMode;
    UCHAR InterruptPolarity;
    UCHAR PinConfiguration;
    USHORT DebounceTimeout;
} RH_GPIO_INTERRUPT_CONNECTION_PARAMETERS, *PRH_GPIO_INTERRUPT_CONNECTION_PARAMETERS;

typedef struct _RH_SPI_CONNECTION_PARAMETERS
{
    LARGE_INTEGER ConnectionId;
    USHORT UpdateMask;
    USHORT DeviceSelection;
    ULONG ConnectionSpeed;
    UCHAR ClockPolarity;
    UCHAR ClockPhase;
    UCHAR DevicePolarity;
    UCHAR DataBitLength;
} RH_SPI_CONNECTION_PARAMETERS, *PRH_SPI_CONNECTION_PARAMETERS;

typedef struct _RH_I2C_CONNECTION_PARAMETERS
{
    LARGE_INTEGER ConnectionId;
    USHORT UpdateMask;
    USHORT TypeSpecificFlags;
    ULONG ConnectionSpeed;
    USHORT SlaveAddress;
} RH_I2C_CONNECTION_PARAMETERS, *PRH_I2C_CONNECTION_PARAMETERS;

typedef struct _RH_UART_CONNECTION_PARAMETERS
{
    LARGE_INTEGER ConnectionId;
    USHORT UpdateMask;
    ULONG BaudRate;
    USHORT TypeSpecificFlags;
    USHORT RxBufferSize;
    USHORT TxBufferSize;
    UCHAR Parity;
} RH_UART_CONNECTION_PARAMETERS, *PRH_UART_CONNECTION_PARAMETERS;

typedef struct _RH_ALLOCATE_UPDATE_CONNECTION_INPUT_BUFFER
{
    ULONG Version;
    RH_CONNECTION_PROPERTIES_INPUT_TYPE ConnectionType;
    union
    {
        RH_GPIO_IO_CONNECTION_PARAMETERS IoConnection;
        RH_GPIO_INTERRUPT_CONNECTION_PARAMETERS InterruptConnection;
        RH_SPI_CONNECTION_PARAMETERS SpiConnection;
        RH_I2C_CONNECTION_PARAMETERS I2cConnection;
        RH_UART_CONNECTION_PARAMETERS UartConnection;
    } u;
} RH_ALLOCATE_UPDATE_CONNECTION_INPUT_BUFFER, *PRH_ALLOCATE_UPDATE_CONNECTION_INPUT_BUFFER;

typedef struct _RH_ALLOCATE_CONNECTION_OUTPUT_BUFFER
{
    ULONG Version;
    LARGE_INTEGER ConnectionId;
} RH_ALLOCATE_CONNECTION_OUTPUT_BUFFER, *PRH_ALLOCATE_CONNECTION_OUTPUT_BUFFER;

typedef struct _RH_UPDATE_CONNECTION_PROPERTIES_OUTPUT_BUFFER
{
    ULONG Version;
} RH_UPDATE_CONNECTION_PROPERTIES_OUTPUT_BUFFER, *PRH_UPDATE_CONNECTION_PROPERTIES_OUTPUT_BUFFER;

/* IOCTL_RH_FREE_CONNECTION parameters */

typedef struct _RH_FREE_CONNECTION_INPUT_BUFFER
{
    ULONG Version;
    LARGE_INTEGER ConnectionId;
} RH_FREE_CONNECTION_INPUT_BUFFER, *PRH_FREE_CONNECTION_INPUT_BUFFER;

typedef struct _RH_FREE_CONNECTION_OUTPUT_BUFFER
{
    ULONG Version;
} RH_FREE_CONNECTION_OUTPUT_BUFFER, *PRH_FREE_CONNECTION_OUTPUT_BUFFER;

/* The ACPI descriptors, byte packed as the firmware emits them */

#include <pshpack1.h>

#define PNP_GPIO_INTERRUPT_IO_DESCRIPTOR_LENGTH sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR)

typedef struct _PNP_GPIO_INTERRUPT_IO_DESCRIPTOR
{
    UCHAR Tag;                      /* Large item name 0xC */
    USHORT Length;                  /* Length of the descriptor, 12 minimum */
    UCHAR Revision;                 /* Descriptor format revision */
    UCHAR DescriptorType;           /* Interrupt or IO descriptor */
    USHORT GeneralFlags;
    USHORT InterruptIoFlags;        /* Flags depending on the descriptor type */
    UCHAR PinConfiguration;         /* Pull configuration for the pin */
    USHORT DriveStrength;
    USHORT DebounceTimeout;
    USHORT PinTableOffset;          /* Offset to the start of the pin table */
    UCHAR ResourceSourceIndex;      /* Index of the resource producer */
    USHORT ResourceSourceOffset;    /* Offset to the resource name string */
    USHORT VendorDataOffset;        /* Offset to the start of the vendor data */
    USHORT VendorDataLength;
} PNP_GPIO_INTERRUPT_IO_DESCRIPTOR, *PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR;

#define PNP_LARGE_VENDOR_DESCRIPTOR_LENGTH sizeof(PNP_LARGE_VENDOR_DESCRIPTOR)

typedef struct _PNP_LARGE_VENDOR_DESCRIPTOR
{
    UCHAR Tag;                      /* Large item name 0x4 */
    USHORT Length;
    UCHAR SubType;                  /* UUID specific subtype */
    GUID UUID;
    UCHAR VendorDescriptor[ANYSIZE_ARRAY];
} PNP_LARGE_VENDOR_DESCRIPTOR, *PPNP_LARGE_VENDOR_DESCRIPTOR;

#define PNP_FUNCTION_CONFIG_DESCRIPTOR_LENGTH sizeof(PNP_FUNCTION_CONFIG_DESCRIPTOR)

typedef struct _PNP_FUNCTION_CONFIG_DESCRIPTOR
{
    UCHAR Tag;                      /* Large item name 0xD */
    USHORT Length;                  /* Length of the descriptor, 12 minimum */
    UCHAR Revision;                 /* Descriptor format revision */
    USHORT Flags;
    UCHAR PinConfiguration;         /* Pull configuration for the pin */
    USHORT FunctionNumber;          /* Function config number for the pin */
    USHORT PinTableOffset;          /* Offset to the start of the pin table */
    UCHAR ResourceSourceIndex;      /* Index of the resource producer */
    USHORT ResourceSourceOffset;    /* Offset to the resource name string */
    USHORT VendorDataOffset;        /* Offset to the start of the vendor data */
    USHORT VendorDataLength;
} PNP_FUNCTION_CONFIG_DESCRIPTOR, *PPNP_FUNCTION_CONFIG_DESCRIPTOR;

#define PNP_SERIAL_BUS_DESCRIPTOR_LENGTH sizeof(PNP_SERIAL_BUS_DESCRIPTOR)

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

#ifdef RESHUB_USE_HELPER_ROUTINES

#define RESOURCE_HUB_ASSERT(exp) NT_ASSERT(exp)

/* Variadic, so this cannot be FORCEINLINE */
static __inline NTSTATUS
RESOURCE_HUB_STRING_PRINTF(
    _Inout_ NTSTRSAFE_PWSTR DestinationString,
    _In_ size_t DestinationSizeInBytes,
    _In_ NTSTRSAFE_PWSTR Format,
    ...)
{
    va_list ArgList;
    NTSTATUS Status;

    va_start(ArgList, Format);
    Status = RtlStringCbVPrintfExW(DestinationString,
                                   DestinationSizeInBytes,
                                   NULL,
                                   NULL,
                                   0,
                                   Format,
                                   ArgList);
    va_end(ArgList);

    return Status;
}

static __inline NTSTATUS
RESOURCE_HUB_UNICODE_STRING_PRINTF(
    _Inout_ PUNICODE_STRING DestinationString,
    _In_ NTSTRSAFE_PWSTR Format,
    ...)
{
    va_list ArgList;
    NTSTRSAFE_PWSTR DestinationEnd;
    NTSTATUS Status;

    va_start(ArgList, Format);
    Status = RtlStringCbVPrintfExW(DestinationString->Buffer,
                                   DestinationString->MaximumLength,
                                   &DestinationEnd,
                                   NULL,
                                   0,
                                   Format,
                                   ArgList);
    va_end(ArgList);

    if (NT_SUCCESS(Status))
        DestinationString->Length = (USHORT)((DestinationEnd - DestinationString->Buffer) * sizeof(WCHAR));

    return Status;
}

/*
 * A bounded init that reports STATUS_NAME_TOO_LONG instead of truncating.
 * RtlInitUnicodeStringEx would do, but it is declared only in <ntifs.h>.
 */
static __inline NTSTATUS
RESOURCE_HUB_UNICODE_STRING_INIT(
    _Out_ PUNICODE_STRING DestinationString,
    _In_opt_z_ PCWSTR SourceString)
{
    SIZE_T Length;

    if (SourceString == NULL)
    {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
        return STATUS_SUCCESS;
    }

    for (Length = 0; SourceString[Length] != UNICODE_NULL; ++Length)
    {
        if (Length >= ((MAXUSHORT / sizeof(WCHAR)) - 1))
            return STATUS_NAME_TOO_LONG;
    }

    DestinationString->Length = (USHORT)(Length * sizeof(WCHAR));
    DestinationString->MaximumLength = (USHORT)((Length * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
    DestinationString->Buffer = (PWSTR)SourceString;

    return STATUS_SUCCESS;
}

static __inline NTSTATUS
RESOURCE_HUB_ID_TO_FILE_NAME(
    _In_ ULONG IdLowPart,
    _In_ ULONG IdHighPart,
    _Out_writes_bytes_(RESOURCE_HUB_CONNECTION_FILE_SIZE) PWCHAR FileName)
{
    LARGE_INTEGER Id;

    Id.LowPart = IdLowPart;
    Id.HighPart = IdHighPart;

    return RESOURCE_HUB_STRING_PRINTF(FileName,
                                      RESOURCE_HUB_FILE_SIZE,
                                      L"%0*I64x",
                                      (ULONG)(sizeof(Id) * 2),
                                      Id.QuadPart);
}

static __inline NTSTATUS
RESOURCE_HUB_CREATE_PATH_FROM_ID(
    _Inout_ PUNICODE_STRING FileName,
    _In_ ULONG IdLowPart,
    _In_ ULONG IdHighPart)
{
    WCHAR FileNameSuffix[RESOURCE_HUB_CONNECTION_FILE_CHARS];
    NTSTATUS Status;

    RESOURCE_HUB_ASSERT(FileName->MaximumLength >= RESOURCE_HUB_CONNECTION_PATH_SIZE);

    Status = RESOURCE_HUB_ID_TO_FILE_NAME(IdLowPart, IdHighPart, FileNameSuffix);
    if (NT_SUCCESS(Status))
    {
        Status = RESOURCE_HUB_UNICODE_STRING_PRINTF(FileName,
                                                    L"%s%s",
                                                    RESOURCE_HUB_DEVICE_NAME_PREFIX,
                                                    FileNameSuffix);
    }

    return Status;
}

static __inline NTSTATUS
RESOURCE_HUB_ID_FROM_FILE_NAME_WITH_SUBPATH(
    _In_z_ LPCWSTR FileName,
    _Out_ PLARGE_INTEGER Id,
    _Out_ LPWSTR *NextPathElement)
{
    UNICODE_STRING HighPart;
    UNICODE_STRING LowPart;
    USHORT Index;
    NTSTATUS Status;

    Id->QuadPart = 0;
    *NextPathElement = (LPWSTR)FileName;

    Status = RESOURCE_HUB_UNICODE_STRING_INIT(&HighPart, FileName);
    if (!NT_SUCCESS(Status))
        return Status;

    if (HighPart.Length < (RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(UNICODE_NULL)))
        return STATUS_INVALID_PARAMETER;

    HighPart.Length = RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(UNICODE_NULL);

    for (Index = 0; Index < (HighPart.Length / sizeof(WCHAR)); ++Index)
    {
        WCHAR Digit = HighPart.Buffer[Index];

        if ((Digit < L'0' || Digit > L'9') &&
            (Digit < L'a' || Digit > L'f') &&
            (Digit < L'A' || Digit > L'F'))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    *NextPathElement = &HighPart.Buffer[Index];
    if ((*NextPathElement)[0] != L'\\' && (*NextPathElement)[0] != UNICODE_NULL)
        return STATUS_INVALID_PARAMETER;

    /* Eight characters per 32 bit half of the id */
    HighPart.Length = 2 * sizeof(Id->HighPart) * sizeof(WCHAR);
    LowPart.Buffer = (PWSTR)((ULONG_PTR)HighPart.Buffer + HighPart.Length);
    LowPart.MaximumLength = HighPart.MaximumLength - HighPart.Length;
    LowPart.Length = 2 * sizeof(Id->LowPart) * sizeof(WCHAR);

    Status = RtlUnicodeStringToInteger(&HighPart, 16, (PULONG)&Id->HighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlUnicodeStringToInteger(&LowPart, 16, (PULONG)&Id->LowPart);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((*NextPathElement)[0] == L'\\')
        ++(*NextPathElement);

    return Status;
}

static __inline NTSTATUS
RESOURCE_HUB_ID_FROM_FILE_NAME(
    _In_z_ LPCWSTR FileName,
    _Out_ PLARGE_INTEGER Id)
{
    LPWSTR NextPathElement;

    return RESOURCE_HUB_ID_FROM_FILE_NAME_WITH_SUBPATH(FileName, Id, &NextPathElement);
}

#endif /* RESHUB_USE_HELPER_ROUTINES */

#ifdef __cplusplus
}
#endif

#endif /* _RESHUB_H_ */
