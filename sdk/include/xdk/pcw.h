/******************************************************************************
 *                     Performance Counter Library Types                      *
 ******************************************************************************/

#define PCW_VERSION_1           0x0100
#define PCW_CURRENT_VERSION     PCW_VERSION_1

typedef struct _PCW_INSTANCE *PPCW_INSTANCE;
typedef struct _PCW_REGISTRATION *PPCW_REGISTRATION;
typedef struct _PCW_BUFFER *PPCW_BUFFER;

typedef struct _PCW_COUNTER_DESCRIPTOR
{
  USHORT Id;
  USHORT StructIndex;
  USHORT Offset;
  USHORT Size;
} PCW_COUNTER_DESCRIPTOR, *PPCW_COUNTER_DESCRIPTOR;

typedef struct _PCW_DATA
{
  _In_reads_bytes_(Size) const VOID *Data;
  _In_ ULONG Size;
} PCW_DATA, *PPCW_DATA;

typedef struct _PCW_COUNTER_INFORMATION
{
  ULONG64 CounterMask;
  PCUNICODE_STRING InstanceMask;
} PCW_COUNTER_INFORMATION, *PPCW_COUNTER_INFORMATION;

/* InstanceId when the consumer asked for every instance rather than one. */
#define PCW_ANY_INSTANCE_ID     0xFFFFFFFF

typedef struct _PCW_MASK_INFORMATION
{
  ULONG64 CounterMask;
  PCUNICODE_STRING InstanceMask;
  ULONG InstanceId;
  BOOLEAN CollectMultiple;
  PPCW_BUFFER Buffer;
  PKEVENT CancelEvent;
} PCW_MASK_INFORMATION, *PPCW_MASK_INFORMATION;

typedef union _PCW_CALLBACK_INFORMATION
{
  PCW_COUNTER_INFORMATION AddCounter;
  PCW_COUNTER_INFORMATION RemoveCounter;
  PCW_MASK_INFORMATION EnumerateInstances;
  PCW_MASK_INFORMATION CollectData;
} PCW_CALLBACK_INFORMATION, *PPCW_CALLBACK_INFORMATION;

typedef enum _PCW_CALLBACK_TYPE
{
  PcwCallbackAddCounter = 0,
  PcwCallbackRemoveCounter,
  PcwCallbackEnumerateInstances,
  PcwCallbackCollectData,
} PCW_CALLBACK_TYPE, *PPCW_CALLBACK_TYPE;

typedef
NTSTATUS
NTAPI
PCW_CALLBACK(
  _In_ PCW_CALLBACK_TYPE Type,
  _In_ PPCW_CALLBACK_INFORMATION Info,
  _In_opt_ PVOID Context);
typedef PCW_CALLBACK *PPCW_CALLBACK;

typedef struct _PCW_REGISTRATION_INFORMATION
{
  _In_ ULONG Version;
  _In_ PCUNICODE_STRING Name;
  _In_ ULONG CounterCount;
  _In_reads_(CounterCount) PPCW_COUNTER_DESCRIPTOR Counters;
  _In_opt_ PPCW_CALLBACK Callback;
  _In_opt_ PVOID CallbackContext;
} PCW_REGISTRATION_INFORMATION, *PPCW_REGISTRATION_INFORMATION;

/******************************************************************************
 *                   Performance Counter Library Functions                    *
 ******************************************************************************/

#if (NTDDI_VERSION >= NTDDI_WIN7)

_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
PcwRegister(
  _Outptr_ PPCW_REGISTRATION *Registration,
  _In_ PPCW_REGISTRATION_INFORMATION Info);

_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
VOID
NTAPI
PcwUnregister(
  _In_ PPCW_REGISTRATION Registration);

_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
PcwCreateInstance(
  _Outptr_ PPCW_INSTANCE *Instance,
  _In_ PPCW_REGISTRATION Registration,
  _In_ PCUNICODE_STRING Name,
  _In_ ULONG Count,
  _In_reads_(Count) PPCW_DATA Data);

_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
VOID
NTAPI
PcwCloseInstance(
  _In_ PPCW_INSTANCE Instance);

_IRQL_requires_max_(APC_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
PcwAddInstance(
  _In_ PPCW_BUFFER Buffer,
  _In_ PCUNICODE_STRING Name,
  _In_ ULONG Id,
  _In_ ULONG Count,
  _In_reads_(Count) PPCW_DATA Data);

#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */
