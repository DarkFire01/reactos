/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Memory collections, driver buffers handed out as prebuilt MDLs
 */

#include "Nx.hpp"

#include <NxApi.hpp>

#include "NxPrivateGlobals.hpp"
#include "verifier.hpp"

#define NX_MEMORY_TAG 'mndm'

/* A collection only counts what it has handed out, so teardown can spot a leak. */
struct NxMemory
{
    NX_PRIVATE_GLOBALS const * m_driverGlobals;
    LONG64 volatile m_buffersOutstanding;

    NxMemory(
        _In_ NX_PRIVATE_GLOBALS const * DriverGlobals) :
        m_driverGlobals(DriverGlobals),
        m_buffersOutstanding(0)
    {
    }

    ~NxMemory()
    {
        if (!m_driverGlobals->CxVerifierOn || m_buffersOutstanding == 0)
            return;

        Verifier_ReportViolation(
            m_driverGlobals,
            VerifierAction_DbgBreakIfDebuggerPresent,
            FailureCode_LeakedNetMemoryNotAllMemoryReleased,
            reinterpret_cast<ULONG_PTR>(this),
            static_cast<ULONG_PTR>(m_buffersOutstanding));
    }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NxMemory, GetNxMemoryFromHandle);

/* What a memory id points at. Kind is always zero for a block this file made. */
struct NxMemoryBlock
{
    ULONG Kind;
    MDL Mdl;
};

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetMemoryCollectionCreate)(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES * Attributes,
    _In_ NET_MEMORY_COLLECTION_CONFIG const * Config,
    _Out_ NETMEMORYCOLLECTION * MemoryCollection)
{
    auto const privateGlobals = GetPrivateGlobals(DriverGlobals);

    /* The configuration only sizes a hint, nothing reads it yet. */
    UNREFERENCED_PARAMETER(Config);

    Verifier_VerifyPrivateGlobals(privateGlobals);
    Verifier_VerifyIrqlPassive(privateGlobals);

    *MemoryCollection = nullptr;

    Verifier_VerifyObjectAttributesParentIsNull(privateGlobals, Attributes);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NxMemory);
    attributes.ParentObject = Device;
    attributes.EvtDestroyCallback = [](WDFOBJECT Object)
    {
        GetNxMemoryFromHandle(Object)->~NxMemory();
    };

    wil::unique_wdf_object object;

    CX_RETURN_IF_NOT_NT_SUCCESS(
        WdfObjectCreate(
            &attributes,
            &object));

    new (GetNxMemoryFromHandle(object.get())) NxMemory(privateGlobals);

    if (Attributes != nullptr)
    {
        CX_RETURN_IF_NOT_NT_SUCCESS(
            WdfObjectAllocateContext(
                object.get(),
                Attributes,
                nullptr));
    }

    *MemoryCollection = reinterpret_cast<NETMEMORYCOLLECTION>(object.release());

    return STATUS_SUCCESS;
}

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetMemoryCreate)(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETMEMORYCOLLECTION MemoryCollection,
    _In_ NET_MEMORY_CONFIG const * Config,
    _Out_ void ** Memory)
{
    auto const privateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(privateGlobals);
    Verifier_VerifyIrqlPassive(privateGlobals);
    Verifier_VerifyNotNull(privateGlobals, Config);

    if (Config->BufferAddress == nullptr)
    {
        Verifier_ReportViolation(
            privateGlobals,
            VerifierAction_BugcheckAlways,
            FailureCode_ParameterCantBeNull,
            reinterpret_cast<ULONG_PTR>(Config),
            0);
    }

    if (Config->BufferLength == 0)
    {
        Verifier_ReportViolation(
            privateGlobals,
            VerifierAction_BugcheckAlways,
            FailureCode_InvalidNetMemoryConfigStructure,
            reinterpret_cast<ULONG_PTR>(Config),
            0);
    }

    auto const collection = GetNxMemoryFromHandle(MemoryCollection);
    SIZE_T const mdlSize = MmSizeOfMdl(Config->BufferAddress, Config->BufferLength);

    auto const block = static_cast<NxMemoryBlock *>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(NxMemoryBlock, Mdl) + mdlSize, NX_MEMORY_TAG));

    CX_RETURN_NTSTATUS_IF_MSG(
        STATUS_INSUFFICIENT_RESOURCES,
        block == nullptr,
        "Could not allocate the MDL for a %Iu byte buffer",
        Config->BufferLength);

    /* The buffer is the driver's own nonpaged memory, so the MDL can be built in place. */
    block->Kind = 0;
    MmInitializeMdl(&block->Mdl, Config->BufferAddress, Config->BufferLength);
    MmBuildMdlForNonPagedPool(&block->Mdl);

    *Memory = block;
    InterlockedIncrement64(&collection->m_buffersOutstanding);

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetMemoryDestroy)(
    _In_ NET_DRIVER_GLOBALS * DriverGlobals,
    _In_ NETMEMORYCOLLECTION MemoryCollection,
    _In_ void * Memory)
{
    auto const privateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(privateGlobals);
    Verifier_VerifyIrqlPassive(privateGlobals);

    auto const collection = GetNxMemoryFromHandle(MemoryCollection);
    auto const block = static_cast<NxMemoryBlock *>(Memory);
    bool const owned = block != nullptr && block->Kind == 0;

    if (owned && collection->m_buffersOutstanding > 0)
    {
        InterlockedDecrement64(&collection->m_buffersOutstanding);
        ExFreePoolWithTag(block, 0);
        return;
    }

    /* A bad release is only reported, the block is left alone either way. */
    if (collection->m_driverGlobals->CxVerifierOn)
    {
        Verifier_ReportViolation(
            collection->m_driverGlobals,
            VerifierAction_DbgBreakIfDebuggerPresent,
            FailureCode_InvalidNetMemoryRelease,
            reinterpret_cast<ULONG_PTR>(Memory),
            block != nullptr ? block->Kind : 0);
    }
}
