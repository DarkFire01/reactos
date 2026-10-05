/*
 * PROJECT:     ReactOS SuperSpeed USB Hub Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GUIDs the public headers do not provide
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Power settings the hub follows */
DEFINE_GUID(GUID_POWER_HUB_SELECTIVE_SUSPEND_TIMEOUT,
            0x0853A681, 0x27C8, 0x4100, 0xA2, 0xFD, 0x82, 0x01, 0x3E, 0x97, 0x06, 0x83);
DEFINE_GUID(GUID_USB_SETTING_SELECTIVE_SUSPEND,
            0x48E6B7A6, 0x50F5, 0x4782, 0xA5, 0xD4, 0x53, 0xBB, 0x8F, 0x07, 0xE2, 0x26);
