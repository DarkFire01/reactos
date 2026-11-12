/*
 * PROJECT:     ReactOS Display Drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Callback a generic display miniport raises to power the panel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * A miniport that only knows the firmware framebuffer notifies this callback
 * object when its output is powered on or off. The driver that owns the real
 * panel registers on it. Argument1 is TRUE to power the panel on and FALSE to
 * power it off, Argument2 is unused. The callback runs at PASSIVE_LEVEL.
 */
#define DISPLAY_PANEL_POWER_CALLBACK    L"\\Callback\\DisplayPanelPower"
