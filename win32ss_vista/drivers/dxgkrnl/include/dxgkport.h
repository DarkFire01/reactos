#pragma once

/* Routines used inside C components */
#ifdef __cplusplus
extern "C"
{
#endif
NTSTATUS
NTAPI
DxgkPortInitializeMiniport(_In_ PDRIVER_OBJECT DriverObject,
                           _In_ PUNICODE_STRING SourceString,
                           _In_ PDRIVER_INITIALIZATION_DATA DriverInitData);
NTSTATUS
NTAPI
DxgkPortInitializeDodMiniport(_In_ PDRIVER_OBJECT DriverObject,
                              _In_ PUNICODE_STRING SourceString,
                              _In_ PKMDDOD_INITIALIZATION_DATA DriverInitData);
#ifdef __cplusplus
}
#endif

VOID
DxgkSetupTimedOperationServices();
