#pragma once
#pragma pack(1)
///
/// MBR Partition Entry
///
typedef struct {
UINT8  BootIndicator;
UINT8  StartHead;
UINT8  StartSector;
UINT8  StartTrack;
UINT8  OSIndicator;
UINT8  EndHead;
UINT8  EndSector;
UINT8  EndTrack;
UINT32 StartingLBA;
UINT32 SizeInLBA;
} MBR_PARTITION_RECORD;
///
/// MBR Partition Table
///
typedef struct {
UINT8 BootStrapCode[440];
UINT8 UniqueMbrSignature[4];
UINT8 Unknown[2];
MBR_PARTITION_RECORD Partition[4];
UINT16 Signature;
} MASTER_BOOT_RECORD;
#pragma pack()
