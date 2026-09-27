/*
 * PROJECT:     ReactOS Hyper-V socket provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The socket entry points the virtual machine bus root imports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A Hyper-V socket is an ordinary socket whose address is a virtual machine
 * rather than a host, carried over the machine bus. The transport behind it is
 * the network stack's, which is not here, so the provider starts with nothing
 * behind it: the bus comes up, its channels work, and a socket of this family
 * is never offered to anyone.
 */

#include <ntifs.h>

/* The interface the bus hands to whoever asks it for socket support */
typedef struct _HVSOCKET_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PVOID Reference;
    PVOID Dereference;
} HVSOCKET_INTERFACE, *PHVSOCKET_INTERFACE;

/**
 * @brief
 * Fills in the interface the bus offers on behalf of this provider.
 *
 * @remarks
 * Nothing is written. The bus keeps the interface in memory it has already
 * cleared, and an interface of no size is one nobody can call through, which is
 * what a provider with no transport behind it has to say.
 */
VOID
NTAPI
HvSocketFillInterface(
    _Out_ PHVSOCKET_INTERFACE Interface)
{
    UNREFERENCED_PARAMETER(Interface);
}

/**
 * @brief
 * Starts the provider for one machine bus.
 *
 * @param[in] Device
 * The bus the provider is being started for.
 *
 * @param[in] Callbacks
 * What the bus offers the provider, which is how a socket address is turned
 * into a machine.
 *
 * @param[out] Provider
 * Receives what is to be handed back when the provider is stopped, which is
 * nothing at all here.
 *
 * @remarks
 * Answering with success is what lets the bus finish coming up. A provider that
 * refused would take the bus down with it, and the bus has plenty to do that has
 * nothing to do with sockets.
 */
NTSTATUS
NTAPI
HvSocketProviderStart(
    _In_ PVOID Device,
    _In_opt_ PVOID Reserved,
    _In_ PVOID Callbacks,
    _Out_ PVOID *Provider)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(Callbacks);

    *Provider = NULL;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops the provider for one machine bus.
 */
VOID
NTAPI
HvSocketProviderStop(
    _In_ PVOID Device,
    _In_opt_ PVOID Reserved,
    _In_opt_ PVOID Provider)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Reserved);
    UNREFERENCED_PARAMETER(Provider);
}

/*
 * Nothing loads this on its own. It is here because the machine bus root names
 * it in its imports, so the loader brings it in and nothing more happens.
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}

/* EOF */
