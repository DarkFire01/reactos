/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     TrustZone secure monitor service requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Clients find the monitor through either interface, both carry the same requests */

/* {DFE3D9B8-BE02-4D27-818E-213D481C5ECA} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_TZ,
            0xdfe3d9b8, 0xbe02, 0x4d27, 0x81, 0x8e, 0x21, 0x3d, 0x48, 0x1c, 0x5e, 0xca);

/* {AE865C08-4A07-404D-BE51-D9A0465E23E5} */
DEFINE_GUID(GUID_DEVINTERFACE_QCOM_TZ_PASSTHROUGH,
            0xae865c08, 0x4a07, 0x404d, 0xbe, 0x51, 0xd9, 0xa0, 0x46, 0x5e, 0x23, 0xe5);

/* Kernel mode only, the request carries raw caller pointers */
#define IOCTL_QCOM_TZ_SERVICE_REQUEST \
    CTL_CODE(FILE_DEVICE_TRUST_ENV, 0x001, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Low bits pick the call, the top bit runs it on processor 0 */
#define QCOM_TZ_FUNCTION_SIP_CALL   0x0000
#define QCOM_TZ_FUNCTION_OS_CALL    0x0001
#define QCOM_TZ_FUNCTION_INDEX_MASK 0x7FFF
#define QCOM_TZ_FUNCTION_ON_CPU0    0x8000

#define QCOM_TZ_MAX_ARGS            10

/* Owner field of an SMC function id, and its 64-bit calling convention bit */
#define QCOM_TZ_OWNER_SIP           0x02000000
#define QCOM_TZ_OWNER_TRUSTED_OS    0x32000000
#define QCOM_TZ_SMC64               0x40000000

/* Service and command packed into an SMC function id */
#define QCOM_TZ_SIP_ID(Service, Command) \
    (QCOM_TZ_OWNER_SIP | ((ULONG)(Service) << 8) | (ULONG)(Command))

/* Argument count in the low nibble of the parameter id */
#define QCOM_TZ_PARAM_COUNT(ParamId) ((ParamId) & 0xF)

typedef struct _QCOM_TZ_SERVICE_REQUEST
{
    ULONG Reserved[3];
    ULONG FunctionCode;
    PVOID InputBuffer;
    ULONG_PTR InputBufferSize;
    PVOID OutputBuffer;
    ULONG_PTR OutputBufferSize;
} QCOM_TZ_SERVICE_REQUEST, *PQCOM_TZ_SERVICE_REQUEST;

typedef struct _QCOM_TZ_SERVICE_RESPONSE
{
    ULONG Reserved;
    ULONG_PTR BytesWritten;
} QCOM_TZ_SERVICE_RESPONSE, *PQCOM_TZ_SERVICE_RESPONSE;

/* Input of both calls */
typedef struct _QCOM_TZ_CALL
{
    ULONG SmcId;
    ULONG ParamId;
    ULONG64 Args[QCOM_TZ_MAX_ARGS];
} QCOM_TZ_CALL, *PQCOM_TZ_CALL;

/* SIP call output, Value[0] is 1 on success and the rest are x1 to x3 */
typedef struct _QCOM_TZ_SIP_RESULT
{
    ULONG64 Value[4];
} QCOM_TZ_SIP_RESULT, *PQCOM_TZ_SIP_RESULT;

/* Trusted OS call output, x1 to x3 of the final return */
typedef struct _QCOM_TZ_OS_RESULT
{
    ULONG64 Value[3];
} QCOM_TZ_OS_RESULT, *PQCOM_TZ_OS_RESULT;

#ifdef _WIN64
C_ASSERT(FIELD_OFFSET(QCOM_TZ_SERVICE_REQUEST, FunctionCode) == 0x0C);
C_ASSERT(FIELD_OFFSET(QCOM_TZ_SERVICE_REQUEST, InputBuffer) == 0x10);
C_ASSERT(sizeof(QCOM_TZ_SERVICE_REQUEST) == 0x30);
C_ASSERT(FIELD_OFFSET(QCOM_TZ_SERVICE_RESPONSE, BytesWritten) == 0x08);
C_ASSERT(sizeof(QCOM_TZ_SERVICE_RESPONSE) == 0x10);
#endif
C_ASSERT(sizeof(QCOM_TZ_CALL) == 0x58);
C_ASSERT(sizeof(QCOM_TZ_SIP_RESULT) == 0x20);
C_ASSERT(sizeof(QCOM_TZ_OS_RESULT) == 0x18);
