#pragma once

/* Large FAT32 */
#pragma pack(push, 1)
typedef struct tagFAT_BOOTSECTOR32
{
	// Common fields.
	BYTE sJmpBoot[3];
	BYTE sOEMName[8];
	WORD wBytsPerSec;
	BYTE bSecPerClus;
	WORD wRsvdSecCnt;
	BYTE bNumFATs;
	WORD wRootEntCnt;
	WORD wTotSec16;           // if zero, use dTotSec32 instead
	BYTE bMedia;
	WORD wFATSz16;
	WORD wSecPerTrk;
	WORD wNumHeads;
	DWORD dHiddSec;
	DWORD dTotSec32;
	DWORD dFATSz32;
	WORD wExtFlags;
	WORD wFSVer;
	DWORD dRootClus;
	WORD wFSInfo;
	WORD wBkBootSec;
	BYTE Reserved[12];
	BYTE bDrvNum;
	BYTE Reserved1;
	BYTE bBootSig;           // == 0x29 if next three fields are ok
	DWORD dBS_VolID;
	BYTE sVolLab[11];
	BYTE sBS_FilSysType[8];
} FAT_BOOTSECTOR32;

typedef struct {
	DWORD dLeadSig;         // 0x41615252
	BYTE sReserved1[480];   // zeros
	DWORD dStrucSig;        // 0x61417272
	DWORD dFree_Count;      // 0xFFFFFFFF
	DWORD dNxt_Free;        // 0xFFFFFFFF
	BYTE sReserved2[12];    // zeros
	DWORD dTrailSig;        // 0xAA550000
} FAT_FSINFO;
#pragma pack(pop)