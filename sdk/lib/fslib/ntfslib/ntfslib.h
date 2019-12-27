/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.h
 */

#ifndef NTFSLIB_H
#define NTFSLIB_H

#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include <ndk/umtypes.h>
#include <fmifs/fmifs.h>

#define BOOT_SECTOR_END  0x55AA

#define BPB_BYTES_PER_SECTOR     512

#define BPB_WINXP_HIDDEN_SECTORS 0x3F
#define BPB_WIN7_HIDDEN_SECTORS  0x800

#define BPB_HEADS  0xFF

#define EBPB_HEADER 0x80008000

#define MFT_LOCATION  0x0C0000
#define CLUSTER_PER_MFT_RECORD     0xF6
#define CLUSTERS_PER_INDEX_RECORD  0x01  //check this

#include <pshpack1.h>

typedef struct _BIOS_PARAMETERS_BLOCK
{
    USHORT    BytesPerSector;      // 0x0B
    UCHAR     SectorsPerCluster;   // 0x0D
    UCHAR     Unused0[7];          // 0x0E, checked when volume is mounted
    UCHAR     MediaId;             // 0x15
    USHORT    Unused1;             // 0x16
    USHORT    SectorsPerTrack;     // 0x18
    USHORT    Heads;               // 0x1A
    DWORD32   HiddenSectorsCount;  // 0x1C
    DWORD32   Unused2;             // 0x20, checked when volume is mounted
} BIOS_PARAMETERS_BLOCK, *PBIOS_PARAMETERS_BLOCK;

typedef struct _EXTENDED_BIOS_PARAMETERS_BLOCK
{
    DWORD32    Header;                  // 0x24, always 80 00 80 00
    ULONGLONG  SectorCount;             // 0x28
    ULONGLONG  MftLocation;             // 0x30
    ULONGLONG  MftMirrLocation;         // 0x38
    CHAR       ClustersPerMftRecord;    // 0x40
    UCHAR      Unused0[3];              // 0x41
    CHAR       ClustersPerIndexRecord;  // 0x44
    UCHAR      Unused1[3];              // 0x45
    ULONGLONG  SerialNumber;            // 0x48
    DWORD32    Checksum;                // 0x50, unused
} EXTENDED_BIOS_PARAMETERS_BLOCK, *PEXTENDED_BIOS_PARAMETERS_BLOCK;

typedef struct _BOOT_SECTOR
{
    UCHAR                           Jump[3];         // 0x00
    ULARGE_INTEGER                  OEMID;           // 0x03
    BIOS_PARAMETERS_BLOCK           BPB;             // 0x0B
    EXTENDED_BIOS_PARAMETERS_BLOCK  EBPB;            // 0x24
    UCHAR                           BootStrap[426];  // 0x54
    USHORT                          EndSector;       // 0x1FE
} BOOT_SECTOR, *PBOOT_SECTOR;

#include <poppack.h>

ULONG NTAPI NtGetTickCount(VOID);

#endif

