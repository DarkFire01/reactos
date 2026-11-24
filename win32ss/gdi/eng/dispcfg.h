/*
 * PROJECT:     ReactOS Win32k
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Display configuration (CCD) types shared by NtUser and the dxgkrnl glue
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * win32k builds for Windows Server 2003, where wingdi.h has none of the display configuration
 * types, so the public SDK layouts are spelled out here.
 */
#if (WINVER < 0x0601)

#define DISPLAYCONFIG_PATH_ACTIVE                       0x00000001
#define DISPLAYCONFIG_PATH_PREFERRED_UNSCALED           0x00000004
#define DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE         0x00000008

#define DISPLAYCONFIG_PATH_MODE_IDX_INVALID             0xFFFFFFFF
#define DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID          0x0000FFFF
#define DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID    0x0000FFFF
#define DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID      0x0000FFFF
#define DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID      0x0000FFFF

#define SDC_TOPOLOGY_INTERNAL                           0x00000001
#define SDC_TOPOLOGY_CLONE                              0x00000002
#define SDC_TOPOLOGY_EXTEND                             0x00000004
#define SDC_TOPOLOGY_EXTERNAL                           0x00000008
#define SDC_TOPOLOGY_SUPPLIED                           0x00000010
#define SDC_USE_DATABASE_CURRENT                        0x0000000F
#define SDC_USE_SUPPLIED_DISPLAY_CONFIG                 0x00000020
#define SDC_VALIDATE                                    0x00000040
#define SDC_APPLY                                       0x00000080
#define SDC_NO_OPTIMIZATION                             0x00000100
#define SDC_SAVE_TO_DATABASE                            0x00000200
#define SDC_ALLOW_CHANGES                               0x00000400
#define SDC_PATH_PERSIST_IF_REQUIRED                    0x00000800
#define SDC_FORCE_MODE_ENUMERATION                      0x00001000
#define SDC_ALLOW_PATH_ORDER_CHANGES                    0x00002000
#define SDC_VIRTUAL_MODE_AWARE                          0x00008000

#define DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE             1
#define DISPLAYCONFIG_MODE_INFO_TYPE_TARGET             2
#define DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE      3

#define DISPLAYCONFIG_PIXELFORMAT_8BPP                  1
#define DISPLAYCONFIG_PIXELFORMAT_16BPP                 2
#define DISPLAYCONFIG_PIXELFORMAT_24BPP                 3
#define DISPLAYCONFIG_PIXELFORMAT_32BPP                 4
#define DISPLAYCONFIG_PIXELFORMAT_NONGDI                5

#define DISPLAYCONFIG_ROTATION_IDENTITY                 1
#define DISPLAYCONFIG_ROTATION_ROTATE90                 2
#define DISPLAYCONFIG_ROTATION_ROTATE180                3
#define DISPLAYCONFIG_ROTATION_ROTATE270                4

#define DISPLAYCONFIG_SCALING_IDENTITY                  1
#define DISPLAYCONFIG_SCALING_CENTERED                  2
#define DISPLAYCONFIG_SCALING_STRETCHED                 3
#define DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX    4
#define DISPLAYCONFIG_SCALING_CUSTOM                    5
#define DISPLAYCONFIG_SCALING_PREFERRED                 128

#define DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED     0
#define DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED_LOWERFIELDFIRST 3

/* The enum typed fields of the SDK are 32 bit wide, UINT32 keeps that with every compiler */
typedef struct DISPLAYCONFIG_RATIONAL
{
    UINT32 Numerator;
    UINT32 Denominator;
} DISPLAYCONFIG_RATIONAL;

typedef struct DISPLAYCONFIG_2DREGION
{
    UINT32 cx;
    UINT32 cy;
} DISPLAYCONFIG_2DREGION;

typedef struct DISPLAYCONFIG_VIDEO_SIGNAL_INFO
{
    UINT64 pixelRate;
    DISPLAYCONFIG_RATIONAL hSyncFreq;
    DISPLAYCONFIG_RATIONAL vSyncFreq;
    DISPLAYCONFIG_2DREGION activeSize;
    DISPLAYCONFIG_2DREGION totalSize;
    union
    {
        struct
        {
            UINT32 videoStandard : 16;
            UINT32 vSyncFreqDivider : 6;
            UINT32 reserved : 10;
        } AdditionalSignalInfo;
        UINT32 videoStandard;
    } DUMMYUNIONNAME;
    UINT32 scanLineOrdering;
} DISPLAYCONFIG_VIDEO_SIGNAL_INFO;

typedef struct DISPLAYCONFIG_TARGET_MODE
{
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO targetVideoSignalInfo;
} DISPLAYCONFIG_TARGET_MODE;

typedef struct DISPLAYCONFIG_SOURCE_MODE
{
    UINT32 width;
    UINT32 height;
    UINT32 pixelFormat;
    POINTL position;
} DISPLAYCONFIG_SOURCE_MODE;

typedef struct DISPLAYCONFIG_DESKTOP_IMAGE_INFO
{
    POINTL PathSourceSize;
    RECTL DesktopImageRegion;
    RECTL DesktopImageClip;
} DISPLAYCONFIG_DESKTOP_IMAGE_INFO;

typedef struct DISPLAYCONFIG_MODE_INFO
{
    UINT32 infoType;
    UINT32 id;
    LUID adapterId;
    union
    {
        DISPLAYCONFIG_TARGET_MODE targetMode;
        DISPLAYCONFIG_SOURCE_MODE sourceMode;
        DISPLAYCONFIG_DESKTOP_IMAGE_INFO desktopImageInfo;
    } DUMMYUNIONNAME;
} DISPLAYCONFIG_MODE_INFO;

typedef struct DISPLAYCONFIG_PATH_SOURCE_INFO
{
    LUID adapterId;
    UINT32 id;
    union
    {
        UINT32 modeInfoIdx;
        struct
        {
            UINT32 cloneGroupId : 16;
            UINT32 sourceModeInfoIdx : 16;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    UINT32 statusFlags;
} DISPLAYCONFIG_PATH_SOURCE_INFO;

typedef struct DISPLAYCONFIG_PATH_TARGET_INFO
{
    LUID adapterId;
    UINT32 id;
    union
    {
        UINT32 modeInfoIdx;
        struct
        {
            UINT32 desktopModeInfoIdx : 16;
            UINT32 targetModeInfoIdx : 16;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    UINT32 outputTechnology;
    UINT32 rotation;
    UINT32 scaling;
    DISPLAYCONFIG_RATIONAL refreshRate;
    UINT32 scanLineOrdering;
    BOOL targetAvailable;
    UINT32 statusFlags;
} DISPLAYCONFIG_PATH_TARGET_INFO;

typedef struct DISPLAYCONFIG_PATH_INFO
{
    DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo;
    DISPLAYCONFIG_PATH_TARGET_INFO targetInfo;
    UINT32 flags;
} DISPLAYCONFIG_PATH_INFO;

#endif /* WINVER < 0x0601 */

C_ASSERT(sizeof(DISPLAYCONFIG_PATH_INFO) == 72);
C_ASSERT(sizeof(DISPLAYCONFIG_MODE_INFO) == 64);
C_ASSERT(FIELD_OFFSET(DISPLAYCONFIG_MODE_INFO, sourceMode) == 0x10);

/*
 * Answers whether dxgkrnl knows the target, the way Windows asks dxgkrnl whether the target runs
 * virtual modes or virtual topologies. Neither is supported here, both always come back TRUE.
 */
NTSTATUS
NTAPI
DlGetTargetVirtualization(
    _In_ const LUID *pAdapterLuid,
    _In_ ULONG TargetId,
    _Out_ PBOOLEAN pbVirtualModesDisabled,
    _Out_ PBOOLEAN pbVirtualTopologiesDisabled);

/* Validates or applies a SetDisplayConfig request that already passed the parameter checks */
NTSTATUS
NTAPI
DlSetDisplayConfigPaths(
    _In_ ULONG cPaths,
    _Inout_updates_opt_(cPaths) DISPLAYCONFIG_PATH_INFO *pPaths,
    _In_ ULONG cModes,
    _In_reads_opt_(cModes) const DISPLAYCONFIG_MODE_INFO *pModes,
    _In_ ULONG Flags,
    _In_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew,
    _Out_ PBOOLEAN pbApplied);
