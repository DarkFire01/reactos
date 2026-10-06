/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The contract a GPIO controller driver binds to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * GpioClx client interface.
 *
 * A GPIO controller driver owns the silicon and nothing else. Everything above
 * it - the pins a peripheral opens, interrupt demultiplexing, the connection
 * ids the resource hub mints - belongs to the class extension, which reaches
 * the hardware only through the callbacks a client registers here.
 *
 * Where a layout is stated below it was read out of the binaries rather than
 * assumed; the evidence is in the comment next to it.
 */

#pragma once

#include <wdf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque to a client. GpioClx allocates one per controller, sized by
 * ControllerContextSize in the registration packet, and hands the client its
 * own storage back through every callback.
 */
typedef struct _GPIO_DEVICE_CONTEXT *PGPIO_DEVICE_CONTEXT;

/*
 * A bank is a fixed run of pins within the controller; a pin number is relative
 * to its bank, not to the controller.
 */
typedef USHORT BANK_ID, *PBANK_ID;
typedef USHORT PIN_NUMBER, *PPIN_NUMBER;

/* What a consumer asked a pin to become */
typedef enum _GPIO_CONNECT_MODE
{
    ConnectModeInvalid = 0,
    ConnectModeInput,
    ConnectModeOutput
} GPIO_CONNECT_MODE, *PGPIO_CONNECT_MODE;

/*
 * Parameter blocks. GpioClx fills these in and the client reads them.
 *
 * Their field offsets were recovered from the Intel client's own reads, which
 * name them explicitly; the citation on each is the function whose accesses
 * pin the layout down. What is not cited is padding, present so the following
 * field lands where the reference reads it.
 */

/* The parameters GpioConnectIoPins reads */
typedef struct _GPIO_CONNECT_IO_PINS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    PPIN_NUMBER PinNumberTable;
    USHORT PinCount;
    USHORT Reserved3;
    ULONG ConnectFlags;
    UCHAR ConnectMode;
    UCHAR PullConfiguration;
    USHORT DebounceTimeout;
} GPIO_CONNECT_IO_PINS_PARAMETERS, *PGPIO_CONNECT_IO_PINS_PARAMETERS;

/* Disconnect carries the same pin list as connect */
typedef struct _GPIO_DISCONNECT_IO_PINS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    PPIN_NUMBER PinNumberTable;
    USHORT PinCount;
    USHORT Reserved3;
    ULONG DisconnectFlags;
    UCHAR DisconnectMode;
    UCHAR Reserved4[3];
} GPIO_DISCONNECT_IO_PINS_PARAMETERS, *PGPIO_DISCONNECT_IO_PINS_PARAMETERS;

/* GpioReadGpioPins / GpioWriteGpioPins: a3+8, a3+16, a3+24 */
typedef struct _GPIO_READ_PINS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    PPIN_NUMBER PinNumberTable;
    ULONG PinCount;
    ULONG Reserved3;
    PULONG64 Buffer;
} GPIO_READ_PINS_PARAMETERS, *PGPIO_READ_PINS_PARAMETERS;

typedef GPIO_READ_PINS_PARAMETERS GPIO_WRITE_PINS_PARAMETERS;
typedef GPIO_WRITE_PINS_PARAMETERS *PGPIO_WRITE_PINS_PARAMETERS;

/*
 * GpioEnableInterrupt: a3+2, a3+8, a3+12, a3+16, and the block GpioClx fills
 * before the call for the rest.
 */
typedef struct _GPIO_ENABLE_INTERRUPT_PARAMETERS
{
    BANK_ID BankId;
    PIN_NUMBER PinNumber;
    ULONG Flags;
    KINTERRUPT_MODE InterruptMode;
    KINTERRUPT_POLARITY Polarity;
    UCHAR PullConfiguration;
    UCHAR Reserved;
    USHORT DebounceTimeout;
    ULONG Reserved2;
    PVOID VendorData;
    ULONG VendorDataLength;
} GPIO_ENABLE_INTERRUPT_PARAMETERS, *PGPIO_ENABLE_INTERRUPT_PARAMETERS;

/* GpioDisableInterrupt: a3+0, a3+2 and nothing else */
typedef struct _GPIO_DISABLE_INTERRUPT_PARAMETERS
{
    BANK_ID BankId;
    PIN_NUMBER PinNumber;
} GPIO_DISABLE_INTERRUPT_PARAMETERS, *PGPIO_DISABLE_INTERRUPT_PARAMETERS;

/* GpioMaskInterrupts: a3+0, a3+8, a3+16 */
typedef struct _GPIO_MASK_INTERRUPT_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    ULONG64 PinMask;

    /* Which pins the client could not mask, written back on failure */
    ULONG64 FailedMask;
} GPIO_MASK_INTERRUPT_PARAMETERS, *PGPIO_MASK_INTERRUPT_PARAMETERS;

/* GpioQueryActiveInterrupts: a3+0, a3+8, a3+16 */
typedef struct _GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    ULONG64 EnabledMask;

    /* Written back: which of those are asserted */
    ULONG64 ActiveMask;
} GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS, *PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS;

/* GpioClearActiveInterrupts, the same shape as mask */
typedef struct _GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    ULONG64 ClearActiveMask;
    ULONG64 FailedClearMask;
} GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS, *PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS;

typedef struct _GPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Reserved2;
    ULONG64 EnabledMask;
} GPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS, *PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS;

/*
 * The vendor function block stays opaque by definition: what is in it is
 * whatever the controller driver and its caller agreed on.
 */

/* GpioReconfigureInterrupt: a3+0, a3+2, a3+4, a3+8 */
typedef struct _GPIO_RECONFIGURE_INTERRUPTS_PARAMETERS
{
    BANK_ID BankId;
    PIN_NUMBER PinNumber;
    KINTERRUPT_MODE InterruptMode;
    KINTERRUPT_POLARITY Polarity;
} GPIO_RECONFIGURE_INTERRUPTS_PARAMETERS, *PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS;

/*
 * GpioSaveBankHardwareContext and its restore counterpart read only the bank
 * id (:5639). Where the saved registers go is the client's business: the
 * reference keeps them in a per-pin table of its own rather than in this block.
 */
typedef struct _GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS
{
    BANK_ID BankId;
    USHORT Reserved;
    ULONG Flags;
} GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS, *PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS;
typedef struct _GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION_PARAMETERS GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION_PARAMETERS, *PGPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION_PARAMETERS;

/*
 * What the client tells GpioClx about its silicon, answered from
 * CLIENT_QueryControllerBasicInformation.
 *
 * Layout confirmed against the Intel driver, which writes Version/Size as the
 * single DWORD 0x00100001 (version 1, size 16), then a USHORT at +4, a UCHAR at
 * +6, and a flags DWORD at +12.
 */
typedef struct _CLIENT_CONTROLLER_BASIC_INFORMATION
{
    USHORT Version;
    USHORT Size;

    /* How many pins the controller has in total */
    USHORT TotalPins;

    /* How many of those sit in one bank; banks are uniform */
    UCHAR NumberOfPinsPerBank;
    UCHAR Reserved;

    ULONG Reserved2;

    union
    {
        struct
        {
            /* The client reaches its registers with MmMapIoSpace, not ports */
            ULONG MemoryMappedController : 1;

            /* Reading the active-interrupt register clears it */
            ULONG ActiveInterruptsAutoClearOnRead : 1;

            /* Bank registers survive a D3 without being saved and restored */
            ULONG BankIdlePowerMgmtSupported : 1;

            /* The controller can stay in D3 while a pin is in use */
            ULONG DeviceIdlePowerMgmtSupported : 1;

            /* Pin masks arrive as bitmaps rather than pin numbers */
            ULONG FormatIoRequestsAsMasks : 1;

            /* The controller has an interrupt line of its own */
            ULONG DeviceInterruptSupported : 1;

            /* Emulate a level-triggered interrupt on edge-only hardware */
            ULONG EmulateDebouncing : 1;

            ULONG Reserved : 25;
        };

        ULONG AsULONG;
    } Flags;
} CLIENT_CONTROLLER_BASIC_INFORMATION, *PCLIENT_CONTROLLER_BASIC_INFORMATION;

/* CALLBACKS ******************************************************************/

typedef
_Function_class_(GPIO_CLIENT_PREPARE_CONTROLLER)
NTSTATUS
(NTAPI GPIO_CLIENT_PREPARE_CONTROLLER)(
    _In_ WDFDEVICE Device,
    _In_ PVOID Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated);
typedef GPIO_CLIENT_PREPARE_CONTROLLER *PGPIO_CLIENT_PREPARE_CONTROLLER;

typedef
_Function_class_(GPIO_CLIENT_RELEASE_CONTROLLER)
NTSTATUS
(NTAPI GPIO_CLIENT_RELEASE_CONTROLLER)(
    _In_ WDFDEVICE Device,
    _In_ PVOID Context);
typedef GPIO_CLIENT_RELEASE_CONTROLLER *PGPIO_CLIENT_RELEASE_CONTROLLER;

/*
 * RestoreContext says whether the controller is coming back from a state that
 * lost its registers; WdfPowerDeviceState is the state it is leaving.
 */
typedef
_Function_class_(GPIO_CLIENT_START_CONTROLLER)
NTSTATUS
(NTAPI GPIO_CLIENT_START_CONTROLLER)(
    _In_ PVOID Context,
    _In_ BOOLEAN RestoreContext,
    _In_ WDF_POWER_DEVICE_STATE PreviousPowerState);
typedef GPIO_CLIENT_START_CONTROLLER *PGPIO_CLIENT_START_CONTROLLER;

typedef
_Function_class_(GPIO_CLIENT_STOP_CONTROLLER)
NTSTATUS
(NTAPI GPIO_CLIENT_STOP_CONTROLLER)(
    _In_ PVOID Context,
    _In_ BOOLEAN SaveContext,
    _In_ WDF_POWER_DEVICE_STATE TargetPowerState);
typedef GPIO_CLIENT_STOP_CONTROLLER *PGPIO_CLIENT_STOP_CONTROLLER;

typedef
_Function_class_(GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION)
NTSTATUS
(NTAPI GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION)(
    _In_ PVOID Context,
    _Out_ PCLIENT_CONTROLLER_BASIC_INFORMATION ControllerInformation);
typedef GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION *PGPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION;

typedef
_Function_class_(GPIO_CLIENT_CONNECT_IO_PINS)
NTSTATUS
(NTAPI GPIO_CLIENT_CONNECT_IO_PINS)(
    _In_ PVOID Context,
    _In_ PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParameters);
typedef GPIO_CLIENT_CONNECT_IO_PINS *PGPIO_CLIENT_CONNECT_IO_PINS;

typedef
_Function_class_(GPIO_CLIENT_DISCONNECT_IO_PINS)
NTSTATUS
(NTAPI GPIO_CLIENT_DISCONNECT_IO_PINS)(
    _In_ PVOID Context,
    _In_ PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParameters);
typedef GPIO_CLIENT_DISCONNECT_IO_PINS *PGPIO_CLIENT_DISCONNECT_IO_PINS;

typedef
_Function_class_(GPIO_CLIENT_READ_PINS)
NTSTATUS
(NTAPI GPIO_CLIENT_READ_PINS)(
    _In_ PVOID Context,
    _In_ PGPIO_READ_PINS_PARAMETERS ReadParameters);
typedef GPIO_CLIENT_READ_PINS *PGPIO_CLIENT_READ_PINS;

typedef
_Function_class_(GPIO_CLIENT_WRITE_PINS)
NTSTATUS
(NTAPI GPIO_CLIENT_WRITE_PINS)(
    _In_ PVOID Context,
    _In_ PGPIO_WRITE_PINS_PARAMETERS WriteParameters);
typedef GPIO_CLIENT_WRITE_PINS *PGPIO_CLIENT_WRITE_PINS;

typedef
_Function_class_(GPIO_CLIENT_ENABLE_INTERRUPT)
NTSTATUS
(NTAPI GPIO_CLIENT_ENABLE_INTERRUPT)(
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParameters);
typedef GPIO_CLIENT_ENABLE_INTERRUPT *PGPIO_CLIENT_ENABLE_INTERRUPT;

typedef
_Function_class_(GPIO_CLIENT_DISABLE_INTERRUPT)
NTSTATUS
(NTAPI GPIO_CLIENT_DISABLE_INTERRUPT)(
    _In_ PVOID Context,
    _In_ PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParameters);
typedef GPIO_CLIENT_DISABLE_INTERRUPT *PGPIO_CLIENT_DISABLE_INTERRUPT;

/* Unmask takes the enable block, not a block of its own */
typedef
_Function_class_(GPIO_CLIENT_UNMASK_INTERRUPT)
NTSTATUS
(NTAPI GPIO_CLIENT_UNMASK_INTERRUPT)(
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS UnmaskParameters);
typedef GPIO_CLIENT_UNMASK_INTERRUPT *PGPIO_CLIENT_UNMASK_INTERRUPT;

typedef
_Function_class_(GPIO_CLIENT_MASK_INTERRUPTS)
NTSTATUS
(NTAPI GPIO_CLIENT_MASK_INTERRUPTS)(
    _In_ PVOID Context,
    _In_ PGPIO_MASK_INTERRUPT_PARAMETERS MaskParameters);
typedef GPIO_CLIENT_MASK_INTERRUPTS *PGPIO_CLIENT_MASK_INTERRUPTS;

typedef
_Function_class_(GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS)
NTSTATUS
(NTAPI GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS)(
    _In_ PVOID Context,
    _Inout_ PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParameters);
typedef GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS *PGPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS;

typedef
_Function_class_(GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS)
NTSTATUS
(NTAPI GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS)(
    _In_ PVOID Context,
    _In_ PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParameters);
typedef GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS *PGPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS;

typedef
_Function_class_(GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS)
NTSTATUS
(NTAPI GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS)(
    _In_ PVOID Context,
    _Inout_ PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS QueryEnabledParameters);
typedef GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS *PGPIO_CLIENT_QUERY_ENABLED_INTERRUPTS;

typedef
_Function_class_(GPIO_CLIENT_RECONFIGURE_INTERRUPT)
NTSTATUS
(NTAPI GPIO_CLIENT_RECONFIGURE_INTERRUPT)(
    _In_ PVOID Context,
    _In_ PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParameters);
typedef GPIO_CLIENT_RECONFIGURE_INTERRUPT *PGPIO_CLIENT_RECONFIGURE_INTERRUPT;

typedef
_Function_class_(GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT)
NTSTATUS
(NTAPI GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT)(
    _In_ PVOID Context,
    _In_ PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS SaveParameters);
typedef GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT *PGPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT;

typedef
_Function_class_(GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT)
NTSTATUS
(NTAPI GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT)(
    _In_ PVOID Context,
    _In_ PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS RestoreParameters);
typedef GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT *PGPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT;

typedef
_Function_class_(GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION)
NTSTATUS
(NTAPI GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION)(
    _In_ PVOID Context,
    _Inout_ PGPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION_PARAMETERS Parameters);
typedef GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION *PGPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION;

/* REGISTRATION ***************************************************************/

/**
 * @brief
 * What a controller driver hands GpioClx to become a client.
 *
 * Layout confirmed against the Intel driver, which zeroes 0xD8 bytes, writes the
 * leading DWORD as 0x00D80003 (version 3, size 216) and the next as 56 (the
 * controller context size), then fills callbacks starting at offset 24. The two
 * gaps below are slots that driver leaves NULL; they are named from the callback
 * set the class extension looks for.
 */
typedef struct _GPIO_CLIENT_REGISTRATION_PACKET
{
    USHORT Version;
    USHORT Size;

    ULONG Flags;

    /* How much per-controller storage GpioClx allocates for the client */
    ULONG ControllerContextSize;

    ULONG Reserved;
    PVOID Reserved1;

    PGPIO_CLIENT_PREPARE_CONTROLLER CLIENT_PrepareController;
    PGPIO_CLIENT_RELEASE_CONTROLLER CLIENT_ReleaseController;
    PGPIO_CLIENT_START_CONTROLLER CLIENT_StartController;
    PGPIO_CLIENT_STOP_CONTROLLER CLIENT_StopController;
    PGPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION CLIENT_QueryControllerBasicInformation;

    /* Left NULL by the Intel drivers */
    PVOID CLIENT_QuerySetControllerInformation;

    PGPIO_CLIENT_ENABLE_INTERRUPT CLIENT_EnableInterrupt;
    PGPIO_CLIENT_DISABLE_INTERRUPT CLIENT_DisableInterrupt;
    PGPIO_CLIENT_UNMASK_INTERRUPT CLIENT_UnmaskInterrupt;
    PGPIO_CLIENT_MASK_INTERRUPTS CLIENT_MaskInterrupts;
    PGPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS CLIENT_QueryActiveInterrupts;
    PGPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS CLIENT_ClearActiveInterrupts;
    PGPIO_CLIENT_CONNECT_IO_PINS CLIENT_ConnectIoPins;
    PGPIO_CLIENT_DISCONNECT_IO_PINS CLIENT_DisconnectIoPins;
    PGPIO_CLIENT_READ_PINS CLIENT_ReadGpioPins;
    PGPIO_CLIENT_WRITE_PINS CLIENT_WriteGpioPins;
    PGPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT CLIENT_SaveBankHardwareContext;
    PGPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT CLIENT_RestoreBankHardwareContext;

    /* Left NULL by the Intel drivers */
    PVOID CLIENT_PreProcessControllerInterrupt;

    PGPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION CLIENT_ControllerSpecificFunction;
    PGPIO_CLIENT_RECONFIGURE_INTERRUPT CLIENT_ReconfigureInterrupt;
    PGPIO_CLIENT_QUERY_ENABLED_INTERRUPTS CLIENT_QueryEnabledInterrupts;

    PVOID Reserved2[2];
} GPIO_CLIENT_REGISTRATION_PACKET, *PGPIO_CLIENT_REGISTRATION_PACKET;

#define GPIO_CLIENT_REGISTRATION_PACKET_VERSION 3

/* CLASS EXTENSION ENTRY POINTS ***********************************************/

/*
 * A controller driver links no GpioClx import library. It carries a
 * .kmdfclassbind descriptor naming the class, and the class extension fills in
 * the table below when wdfldr binds it, so every call here is indirect.
 *
 * The table has exactly four entries, in this order: RegisterClient,
 * UnregisterClient, ProcessAddDevicePreDeviceCreate and
 * ProcessAddDevicePostDeviceCreate, all four copied into the client's table at
 * bind time.
 */

typedef VOID (NTAPI *GPIOCLXFUNC)(VOID);

typedef enum _GPIOCLXFUNCENUM
{
    GpioClxRegisterClientTableIndex = 0,
    GpioClxUnregisterClientTableIndex,
    GpioClxProcessAddDevicePreDeviceCreateTableIndex,
    GpioClxProcessAddDevicePostDeviceCreateTableIndex,
    GpioClxFunctionTableNumEntries
} GPIOCLXFUNCENUM;

typedef struct _WDF_DRIVER_GLOBALS GPIO_DRIVER_GLOBALS, *PGPIO_DRIVER_GLOBALS;

/* Both filled in by the bind; see drivers/gpio/inc/gpioclient.c */
extern GPIOCLXFUNC GpioClxFunctions[];
extern PGPIO_DRIVER_GLOBALS GpioClxDriverGlobals;

typedef NTSTATUS (NTAPI *PFN_GPIOCLXREGISTERCLIENT)(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    _In_opt_ PCUNICODE_STRING RegistryPath);

typedef NTSTATUS (NTAPI *PFN_GPIOCLXUNREGISTERCLIENT)(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver);

typedef NTSTATUS (NTAPI *PFN_GPIOCLXPROCESSADDDEVICEPREDEVICECREATE)(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _Out_ PWDF_OBJECT_ATTRIBUTES DeviceAttributes);

typedef NTSTATUS (NTAPI *PFN_GPIOCLXPROCESSADDDEVICEPOSTDEVICECREATE)(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ WDFDEVICE Device);

/**
 * @brief
 * Announces this driver as a GpioClx client.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in] RegistrationPacket
 * The callbacks this client answers. Copied, so a stack block will do.
 *
 * @param[in] RegistryPath
 * The client's service key, for GpioClx to read its own settings from.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the registration.
 */
FORCEINLINE NTSTATUS
GPIO_CLX_RegisterClient(
    _In_ WDFDRIVER Driver,
    _In_ PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    _In_opt_ PCUNICODE_STRING RegistryPath)
{
    return ((PFN_GPIOCLXREGISTERCLIENT)
                GpioClxFunctions[GpioClxRegisterClientTableIndex])(
        GpioClxDriverGlobals, Driver, RegistrationPacket, RegistryPath);
}

/**
 * @brief
 * Withdraws this driver's client registration.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @return
 * STATUS_SUCCESS, or the failure reported by the class extension.
 */
FORCEINLINE NTSTATUS
GPIO_CLX_UnregisterClient(
    _In_ WDFDRIVER Driver)
{
    return ((PFN_GPIOCLXUNREGISTERCLIENT)
                GpioClxFunctions[GpioClxUnregisterClientTableIndex])(
        GpioClxDriverGlobals, Driver);
}

/**
 * @brief
 * Lets GpioClx prepare the device before the client creates it.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in,out] DeviceInit
 * The initialization block the device will be created from.
 *
 * @param[out] DeviceAttributes
 * Receives the attributes to pass to WdfDeviceCreate.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the device coming up.
 */
FORCEINLINE NTSTATUS
GPIO_CLX_ProcessAddDevicePreDeviceCreate(
    _In_ WDFDRIVER Driver,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _Out_ PWDF_OBJECT_ATTRIBUTES DeviceAttributes)
{
    return ((PFN_GPIOCLXPROCESSADDDEVICEPREDEVICECREATE)
                GpioClxFunctions[GpioClxProcessAddDevicePreDeviceCreateTableIndex])(
        GpioClxDriverGlobals, Driver, DeviceInit, DeviceAttributes);
}

/**
 * @brief
 * Lets GpioClx attach itself once the client has created the device.
 *
 * @param[in] Driver
 * The calling driver.
 *
 * @param[in] Device
 * The device just created.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the device coming up.
 */
FORCEINLINE NTSTATUS
GPIO_CLX_ProcessAddDevicePostDeviceCreate(
    _In_ WDFDRIVER Driver,
    _In_ WDFDEVICE Device)
{
    return ((PFN_GPIOCLXPROCESSADDDEVICEPOSTDEVICECREATE)
                GpioClxFunctions[GpioClxProcessAddDevicePostDeviceCreateTableIndex])(
        GpioClxDriverGlobals, Driver, Device);
}

#ifdef __cplusplus
}
#endif
