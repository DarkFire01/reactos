/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hardware button events for the shell, from the generic buttons device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define QCOM_BUTTONS_DEVICE         L"\\\\.\\QcomButtons"

/* Completes with one QCOM_BUTTON_EVENT once a button changes */
#define IOCTL_QCOM_BUTTONS_WAIT     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

#define QCOM_BUTTON_RELEASED        0
#define QCOM_BUTTON_PRESSED         1
#define QCOM_BUTTON_REPEAT          2

/* HID usages the firmware assigns to the buttons */
#define QCOM_BUTTON_PAGE_CONSUMER   0x0C
#define QCOM_BUTTON_VOLUME_UP       0xE9
#define QCOM_BUTTON_VOLUME_DOWN     0xEA
#define QCOM_BUTTON_PAGE_CAMERA     0x90
#define QCOM_BUTTON_CAMERA_FOCUS    0x20
#define QCOM_BUTTON_CAMERA_SHUTTER  0x21

typedef struct _QCOM_BUTTON_EVENT
{
    USHORT UsagePage;
    USHORT Usage;
    ULONG State;
} QCOM_BUTTON_EVENT, *PQCOM_BUTTON_EVENT;
