#pragma once 

/* sysinfo.c */
NTSTATUS
NTAPI
HaliQuerySystemInformation(
    _In_ HAL_QUERY_INFORMATION_CLASS InformationClass,
    _In_ ULONG BufferSize,
    _Inout_ PVOID Buffer,
    _Out_ PULONG ReturnedLength
);

NTSTATUS
NTAPI
HaliSetSystemInformation(
    _In_ HAL_SET_INFORMATION_CLASS InformationClass,
    _In_ ULONG BufferSize,
    _Inout_ PVOID Buffer
);

/* usage.c */
CODE_SEG("INIT")
VOID
NTAPI
HalpRegisterVector(
    _In_ UCHAR Flags,
    _In_ ULONG BusVector,
    _In_ ULONG SystemVector,
    _In_ KIRQL Irql
);

CODE_SEG("INIT")
VOID
NTAPI
HalpReportResourceUsage(
    _In_ PUNICODE_STRING HalName,
    _In_ INTERFACE_TYPE InterfaceType
);

/* INTRODUCED IN VISTA+ *************************************************************************/