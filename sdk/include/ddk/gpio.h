/*
 * PROJECT:     ReactOS DDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controls a GPIO I/O connection answers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Bit n of the buffer is entry n of the connection's pin table */
#define IOCTL_GPIO_READ_PINS                    CTL_CODE(FILE_DEVICE_GPIO, 0x0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_WRITE_PINS                   CTL_CODE(FILE_DEVICE_GPIO, 0x1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_CONTROLLER_SPECIFIC_FUNCTION CTL_CODE(FILE_DEVICE_GPIO, 0x2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_COMMIT_FUNCTION_CONFIG_PINS  CTL_CODE(FILE_DEVICE_GPIO, 0x4, METHOD_BUFFERED, FILE_ANY_ACCESS)
