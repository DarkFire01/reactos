
#pragma once

/*
 * ReactOS ships two d3dhal.h: the kernel-mode display-driver one in
 * sdk/include/ddk, and the DirectX 7 user-mode one in sdk/include/psdk.
 * sdk/include/ddk precedes sdk/include/psdk on the include path, so a plain
 * #include "d3dhal.h" from user-mode code lands on the driver header, which
 * has no D3DDEVICEDESC_V1/V2/V3.
 *
 * Wine's ddraw wants the user-mode one; select it explicitly here. Only
 * modules built with set_wine_module() see this file.
 */
#include <psdk/d3dhal.h>
