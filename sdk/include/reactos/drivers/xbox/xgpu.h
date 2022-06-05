/*
 * PROJECT:     Original Xbox onboard hardware
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nVidia NV2A (XGPU) header file
 * COPYRIGHT:   Copyright 2020 Stanislav Motylkov (x86corez@gmail.com)
 */

#ifndef _XGPU_H_
#define _XGPU_H_

#pragma once

/*
 * Registers and definitions
 */
#define NV2A_VIDEO_MEMORY_SIZE  (4 * 1024 * 1024) /* FIXME: obtain fb size from firmware somehow (Cromwell reserves high 4 MB of RAM) */

#define NV2A_FB_OFFSET                 0x100000
#define   NV2A_FB_CFG0                   (0x200 + NV2A_FB_OFFSET)
#define NV2A_CRTC_OFFSET               0x600000
#define   NV2A_CRTC_FRAMEBUFFER_START    (0x800 + NV2A_CRTC_OFFSET)
#define   NV2A_CRTC_REGISTER_INDEX      (0x13D4 + NV2A_CRTC_OFFSET)
#define   NV2A_CRTC_REGISTER_VALUE      (0x13D5 + NV2A_CRTC_OFFSET)
#define NV2A_RAMDAC_OFFSET             0x680000
#define   NV2A_RAMDAC_FP_HVALID_END      (0x838 + NV2A_RAMDAC_OFFSET)
#define   NV2A_RAMDAC_FP_VVALID_END      (0x818 + NV2A_RAMDAC_OFFSET)



/* NV2A registers */
#define NV_REGS 0xFD000000

#define NV_DMA_ADDRESS                                        0xFFFFF000


#define NV_PMC_BOOT_0                                    0x00000000
#define NV_PMC_INTR_0                                    0x00000100
#define NV_PMC_INTR_EN_0                                 0x00000140
#define NV_PMC_ENABLE                                    0x00000200


/* TODO: let's do some research on the specifics */
typedef struct _PMC_INTR_0
{
    struct
    {
        ULONG Reserved                             : 8;
        ULONG PMC_INTR_0_PFIFO                     : 4;
        ULONG PMC_INTR_0_PGRAPH                    : 12;
        ULONG PMC_INTR_0_PCRTC                     : 4;
        ULONG PMC_INTR_0_PBUS                      : 2;
        ULONG PMC_INTR_0_SOFTWARE                  : 2;
    };
} PMC_INTR_0, PPMC_INTR_0;

typedef struct _PMC_ENABLE
{
    struct
    {
        ULONG Reserved                             : 8;
        ULONG PMC_ENABLE_PFIFO                     : 4;
        ULONG PMC_ENABLE_PGRAPH                    : 1;
        ULONG Reserved2                            : 19; /* or maybe an extention to the enable? */
    };
} PMC_ENABLE, PPMC_ENABLE;

#define NV_PGRAPH_CLEARRECTX                             0x00001864
#       define NV_PGRAPH_CLEARRECTX_XMIN                          0x00000FFF
#       define NV_PGRAPH_CLEARRECTX_XMAX                          0x0FFF0000
#define NV_PGRAPH_CLEARRECTY                             0x00001868
#       define NV_PGRAPH_CLEARRECTY_YMIN                          0x00000FFF
#       define NV_PGRAPH_CLEARRECTY_YMAX                          0x0FFF0000
#define NV_PGRAPH_COLORCLEARVALUE                        0x0000186C


struct s_CtxDma
{
  DWORD32              ChannelID;
  DWORD32              Inst;   //Addr in PRAMIN area, unit=16 bytes blocks, baseaddr=VIDEO_BASE+NV_PRAMIN
  DWORD32              Class;
  DWORD32              isGr;
};

#endif /* _XGPU_H_ */
