/*
 * PROJECT:     ReactOS KMDF: driver initialization static library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Main file
 * COPYRIGHT:   Copyright 2021 Max Korostil <mrmks04@yandex.ru>
 */

#include <ntddk.h>
#include <windef.h>
#include <fxldr.h>
#include "wdf.h"


#define WDFENTRY_TAG 'EFDW'

// supplied by the driver this library is linked into
extern
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath);

const WDFFUNC *WdfFunctions;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
WDF_BIND_INFO BindInfo =
{
    .Size = sizeof(BindInfo),
    .Component = L"KmdfLibrary", 
    .Version.Major = __WDF_MAJOR_VERSION,
    .Version.Minor = __WDF_MINOR_VERSION,
    .Version.Build = __WDF_BUILD_NUMBER,
    .FuncCount = WdfFunctionTableNumEntries,
    .FuncTable = (WDFFUNC *)&WdfFunctions
};
PDRIVER_UNLOAD pOriginalUnload = NULL;
UNICODE_STRING gRegistryPath;


/*
 * Class-library binding.
 *
 * A KMDF class extension (ucx01000, for one) is reached through a function
 * table that stays empty until the framework binds the class to this client.
 * Each client places a WDF_CLASS_BIND_INFO in ".kmdfclassbind$b"; the two
 * markers below sit in "$a" and "$c", so the linker's sorted merge of
 * ".kmdfclassbind$*" leaves every descriptor between them.
 *
 * Entries are validated by Size and ClassName rather than trusted blindly,
 * because the merge can pad between contributions.
 */
/*
 * The whole section name is passed rather than just the suffix: MSVC does not
 * concatenate adjacent string literals inside __declspec(allocate()), so a
 * split name reaches the linker as ".kmdfclassbind$" with the suffix dropped.
 */
#if defined(__GNUC__)
#define FX_CLASS_BIND_SECTION(_section_)    __attribute__((section(_section_), used, aligned(4)))
#else
#pragma section(".kmdfclassbind$a", read, write)
#pragma section(".kmdfclassbind$c", read, write)
#define FX_CLASS_BIND_SECTION(_section_)    __declspec(allocate(_section_))
#endif

FX_CLASS_BIND_SECTION(".kmdfclassbind$a") WDF_CLASS_BIND_INFO FxClassBindStart = { 0 };
FX_CLASS_BIND_SECTION(".kmdfclassbind$c") WDF_CLASS_BIND_INFO FxClassBindEnd = { 0 };

/**
 * @brief
 * Finds the next class-bind descriptor at or after a point in the merged
 * section.
 *
 * The linker pads between the merged contributions, so the descriptors are not
 * evenly spaced and a fixed stride would walk into the padding. Scan forward a
 * word at a time and recognize an entry by its header instead. Padding is
 * zeroed, so a Size of sizeof(WDF_CLASS_BIND_INFO) together with a class name
 * and a function table identifies a real descriptor.
 *
 * @param[in] Cursor
 * Where to start looking.
 *
 * @param[in] End
 * One past the last byte that may be read.
 *
 * @return
 * The descriptor found, or NULL once the section is exhausted.
 */
static
PWDF_CLASS_BIND_INFO
FxNextClassBindEntry(
    _In_ PUCHAR Cursor,
    _In_ PUCHAR End)
{
    PWDF_CLASS_BIND_INFO entry;

    while ((Cursor + sizeof(WDF_CLASS_BIND_INFO)) <= End)
    {
        entry = (PWDF_CLASS_BIND_INFO)Cursor;

        if ((entry->Size == sizeof(WDF_CLASS_BIND_INFO)) &&
            (entry->ClassName != NULL) &&
            (entry->FunctionTable != NULL))
        {
            return entry;
        }

        Cursor += sizeof(ULONG);
    }

    return NULL;
}

/**
 * @brief
 * Unbinds the class libraries this driver bound, stopping before a given
 * descriptor.
 *
 * @param[in] Last
 * The descriptor to stop at, which is left unbound. NULL unbinds all of them.
 *
 * @return
 * Nothing.
 */
static
VOID
FxUnbindClassesUpTo(
    _In_opt_ PWDF_CLASS_BIND_INFO Last)
{
    PWDF_CLASS_BIND_INFO entry;
    PUCHAR cursor;
    PUCHAR end;

    cursor = (PUCHAR)&FxClassBindStart + sizeof(WDF_CLASS_BIND_INFO);
    end = (PUCHAR)&FxClassBindEnd;

    while ((entry = FxNextClassBindEntry(cursor, end)) != NULL)
    {
        if (entry == Last)
        {
            break;
        }

        WdfVersionUnbindClass(&BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals, entry);
        cursor = (PUCHAR)entry + sizeof(WDF_CLASS_BIND_INFO);
    }
}

/**
 * @brief
 * Unbinds every class library this driver bound.
 *
 * @return
 * Nothing.
 */
static
VOID
FxUnbindClasses(VOID)
{
    FxUnbindClassesUpTo(NULL);
}

/**
 * @brief
 * Binds every class library this driver declared a descriptor for.
 *
 * Until this runs the client's class function table is all zeroes, so the
 * first class API it calls is a call through a NULL pointer.
 *
 * @return
 * STATUS_SUCCESS on success, or the NTSTATUS the failing bind produced.
 */
static
NTSTATUS
FxBindClasses(VOID)
{
    PWDF_CLASS_BIND_INFO entry;
    PUCHAR cursor;
    PUCHAR end;
    NTSTATUS status;

    cursor = (PUCHAR)&FxClassBindStart + sizeof(WDF_CLASS_BIND_INFO);
    end = (PUCHAR)&FxClassBindEnd;

    while ((entry = FxNextClassBindEntry(cursor, end)) != NULL)
    {
        status = WdfVersionBindClass(&BindInfo,
                                     (PWDF_COMPONENT_GLOBALS*)&WdfDriverGlobals,
                                     entry);
        if (!NT_SUCCESS(status))
        {
            /* Leave the client no half-bound classes to trip over. */
            FxUnbindClassesUpTo(entry);
            return status;
        }

        cursor = (PUCHAR)entry + sizeof(WDF_CLASS_BIND_INFO);
    }

    return STATUS_SUCCESS;
}

static
VOID
FxDriverUnloadCommon(VOID)
{
    FxUnbindClasses();
    WdfVersionUnbind(&gRegistryPath, &BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals);
}

VOID
NTAPI
FxDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    if (pOriginalUnload != NULL)
    {
        pOriginalUnload(DriverObject);
    }
    FxDriverUnloadCommon();
}

NTSTATUS
NTAPI
FxDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    if (DriverObject == NULL)
    {
        return DriverEntry(DriverObject, RegistryPath);
    }

    // Copy registry path
    gRegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    gRegistryPath.Buffer = ExAllocatePoolWithTag(PagedPool,
                                                 gRegistryPath.MaximumLength,
                                                 WDFENTRY_TAG);

    if (gRegistryPath.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&gRegistryPath, RegistryPath);

    // Bind wdf driver to framework
    status = WdfVersionBind(DriverObject,
                            RegistryPath,
                            &BindInfo,
                            (PWDF_COMPONENT_GLOBALS*)(&WdfDriverGlobals));

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // Bind any class libraries this driver declared
    status = FxBindClasses();
    if (!NT_SUCCESS(status))
    {
        WdfVersionUnbind(&gRegistryPath, &BindInfo, (PWDF_COMPONENT_GLOBALS)WdfDriverGlobals);
        return status;
    }

    // Call original entry point
    status = DriverEntry(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status))
    {
        FxDriverUnloadCommon();
        return status;
    }

    if (WdfDriverGlobals->DisplaceDriverUnload)
    {
        pOriginalUnload = DriverObject->DriverUnload;
        DriverObject->DriverUnload = FxDriverUnload;
    }
    return STATUS_SUCCESS;
}
