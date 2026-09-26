/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Feature configuration and usage reporting
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

typedef struct _RTL_FEATURE_CONFIGURATION
{
    ULONG FeatureId;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG Priority:4;
            ULONG EnabledState:2;
            ULONG IsWexpConfiguration:1;
            ULONG HasSubscriptions:1;
            ULONG Variant:6;
            ULONG VariantPayloadKind:2;
            ULONG Reserved:16;
        };
    };
    ULONG VariantPayload;
} RTL_FEATURE_CONFIGURATION, *PRTL_FEATURE_CONFIGURATION;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Reads the configuration of a staged feature.
 *
 * @return
 * STATUS_NOT_FOUND. Nothing stages features here, so every feature is left at
 * the state its own code compiled in.
 */
NTSTATUS
NTAPI
RtlQueryFeatureConfiguration(
    _In_ ULONG FeatureId,
    _In_ ULONG FeatureType,
    _Inout_ PULONG64 ChangeStamp,
    _Out_ PRTL_FEATURE_CONFIGURATION Configuration)
{
    UNREFERENCED_PARAMETER(FeatureType);

    if (ChangeStamp != NULL)
        *ChangeStamp = 0;

    if (Configuration != NULL)
    {
        RtlZeroMemory(Configuration, sizeof(*Configuration));
        Configuration->FeatureId = FeatureId;
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Returns the stamp that changes when any feature configuration does.
 *
 * @return
 * Zero, which never changes because no configuration ever does.
 */
ULONG64
NTAPI
RtlQueryFeatureConfigurationChangeStamp(VOID)
{
    return 0;
}

NTSTATUS
NTAPI
RtlRegisterFeatureConfigurationChangeNotification(
    _In_ PVOID Callback,
    _In_opt_ PVOID Context,
    _Inout_opt_ PULONG64 ChangeStamp,
    _Out_ PVOID *NotificationHandle)
{
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Context);

    if (ChangeStamp != NULL)
        *ChangeStamp = 0;

    /* Nothing will ever change, so there is nothing to hold on to */
    if (NotificationHandle != NULL)
        *NotificationHandle = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnregisterFeatureConfigurationChangeNotification(
    _In_ PVOID NotificationHandle)
{
    UNREFERENCED_PARAMETER(NotificationHandle);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlRegisterFeatureUsageProvider(
    _In_ PVOID Callback,
    _In_opt_ PVOID Context,
    _Out_opt_ PVOID *ProviderHandle)
{
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Context);

    if (ProviderHandle != NULL)
        *ProviderHandle = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlUnregisterFeatureUsageProvider(
    _In_ PVOID ProviderHandle)
{
    UNREFERENCED_PARAMETER(ProviderHandle);
    return STATUS_SUCCESS;
}

VOID
NTAPI
RtlArmFeatureUsageProviderFlushNotification(
    _In_ PVOID ProviderHandle,
    _In_ ULONG64 Flags)
{
    UNREFERENCED_PARAMETER(ProviderHandle);
    UNREFERENCED_PARAMETER(Flags);
}

/**
 * @brief
 * Counts a use of a staged feature, for the telemetry nobody is collecting.
 */
VOID
NTAPI
RtlRecordFeatureUsage(
    _In_ PVOID FeatureUsage,
    _In_ ULONG64 Count)
{
    UNREFERENCED_PARAMETER(FeatureUsage);
    UNREFERENCED_PARAMETER(Count);
}

VOID
NTAPI
RtlNotifyFeatureUsage(
    _In_ PVOID FeatureUsage)
{
    UNREFERENCED_PARAMETER(FeatureUsage);
}

/* EOF */
