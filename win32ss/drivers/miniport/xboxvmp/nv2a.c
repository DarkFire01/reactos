#include "xboxvmp.h"
#include <drivers/xbox/xgpu.h>
#include <debug.h>

#define NV_PMC_ENABLE_PGRAPH_DISABLED			0xFFFFEFFF
#define NV_PMC_ENABLE_PGRAPH_ENABLED			0x00001000

/* INITALIZE BASIC GPU STUFF */
VOID
NV2A_InitGPU(PXBOXVMP_DEVICE_EXTENSION XboxVmpDeviceExtension)
{
    WRITE_REGISTER_ULONG((ULONG_PTR)XboxVmpDeviceExtension->VirtControlStart + NV_PMC_ENABLE, NV_PMC_ENABLE_PGRAPH_ENABLED);
}