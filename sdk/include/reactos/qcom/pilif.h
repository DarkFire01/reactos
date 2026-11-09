/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Peripheral image loader requests and subsystem identities
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Subsystem images, also the key names under services\qcSubsystemLoad */

/* {BA58766D-ABF2-4402-88D7-90AB243F6C77} */
DEFINE_GUID(GUID_QCOM_IMAGE_MBA,
            0xba58766d, 0xabf2, 0x4402, 0x88, 0xd7, 0x90, 0xab, 0x24, 0x3f, 0x6c, 0x77);

/* {936DC601-5530-4B82-9D2A-72A488BEC7C1} */
DEFINE_GUID(GUID_QCOM_IMAGE_MODEM,
            0x936dc601, 0x5530, 0x4b82, 0x9d, 0x2a, 0x72, 0xa4, 0x88, 0xbe, 0xc7, 0xc1);

/* {D58464D3-5B28-4EA6-A2E2-E8E57C5C69B8} */
DEFINE_GUID(GUID_QCOM_IMAGE_ADSP,
            0xd58464d3, 0x5b28, 0x4ea6, 0xa2, 0xe2, 0xe8, 0xe5, 0x7c, 0x5c, 0x69, 0xb8);

/* {93367D0F-35CE-4EC4-8E38-BB33030C58B2} */
DEFINE_GUID(GUID_QCOM_IMAGE_WCNSS,
            0x93367d0f, 0x35ce, 0x4ec4, 0x8e, 0x38, 0xbb, 0x33, 0x03, 0x0c, 0x58, 0xb2);

#define QCOM_PIL_DEVICE_NAME        L"\\Device\\PIL"
#define QCOM_PIL_DEVICE_TYPE        0x9C41

/* Drops a loaded image, input is its 16 byte GUID */
#define IOCTL_QCOM_PIL_UNLOAD \
    CTL_CODE(QCOM_PIL_DEVICE_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Modem self authentication steps, input is one QCOM_PIL_MSA_* record */
#define IOCTL_QCOM_PIL_MSA \
    CTL_CODE(QCOM_PIL_DEVICE_TYPE, 0x90A, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define QCOM_PIL_MSA_VERSION        2

/* Steps in the order the modem side sends them */
#define QCOM_PIL_MSA_SET_RMB        1
#define QCOM_PIL_MSA_LOAD_MBA       2
#define QCOM_PIL_MSA_LOAD_MODEM     3

#define QCOM_PIL_PATH_LENGTH        260

typedef struct _QCOM_PIL_MSA_HEADER
{
    USHORT Size;
    USHORT Version;
    ULONG Type;
} QCOM_PIL_MSA_HEADER, *PQCOM_PIL_MSA_HEADER;

/* Kernel address of the mapped modem boot handshake registers */
typedef struct _QCOM_PIL_MSA_RMB
{
    QCOM_PIL_MSA_HEADER Header;
    PVOID Registers;
} QCOM_PIL_MSA_RMB, *PQCOM_PIL_MSA_RMB;

typedef struct _QCOM_PIL_MSA_IMAGE
{
    QCOM_PIL_MSA_HEADER Header;
    GUID Image;
    WCHAR Path[QCOM_PIL_PATH_LENGTH];
} QCOM_PIL_MSA_IMAGE, *PQCOM_PIL_MSA_IMAGE;

/* Modem boot handshake register block, byte offsets */
#define QCOM_RMB_MBA_IMAGE          0x00
#define QCOM_RMB_PBL_STATUS         0x04
#define QCOM_RMB_MBA_COMMAND        0x08
#define QCOM_RMB_MBA_STATUS         0x0C
#define QCOM_RMB_PMI_META_DATA      0x10
#define QCOM_RMB_PMI_CODE_START     0x14
#define QCOM_RMB_PMI_CODE_LENGTH    0x18
#define QCOM_RMB_SIZE               0x24

#ifdef _WIN64
C_ASSERT(sizeof(QCOM_PIL_MSA_RMB) == 16);
#endif
C_ASSERT(sizeof(QCOM_PIL_MSA_IMAGE) == 544);
C_ASSERT((ULONG)IOCTL_QCOM_PIL_UNLOAD == 0x9C412408UL);
C_ASSERT((ULONG)IOCTL_QCOM_PIL_MSA == 0x9C412428UL);
