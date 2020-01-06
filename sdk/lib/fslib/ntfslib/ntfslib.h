/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.h
 * PURPOSE:     NTFS lib definitions
 * PROGRAMMERS: Klachkov Valery
 */

#ifndef NTFSLIB_H
#define NTFSLIB_H


/* INCLUDES ******************************************************************/

#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/umtypes.h>

#include <fmifs/fmifs.h>


/* MACROSES ******************************************************************/

#define KeQuerySystemTime(t)  GetSystemTimeAsFileTime((LPFILETIME)(t));

#define FREE(p) if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p);


/* BOOT SECTOR DEFINES *******************************************************/

#define BPB_BYTES_PER_SECTOR     512
#define BPB_WINXP_HIDDEN_SECTORS 0x3F
#define BPB_WIN7_HIDDEN_SECTORS  0x0800
#define BPB_HEADS  0xFF

#define EBPB_HEADER 0x80008000

#define MFT_LOCATION  0x0C0000
#define CLUSTER_PER_MFT_RECORD     0xF6
#define CLUSTERS_PER_INDEX_RECORD  0x01  // TODO: Recheck the correctness of the value

#define BOOT_SECTOR_END  0xAA55


/* FILES DEFINES *************************************************************/

// NRH - NTFS_RECORD_HEADER
#define NRH_FILE_TYPE         0x454C4946  // 'FILE'
#define NRH_USA_OFFSET_WINXP  0x002D

#define MFT_RECORD_SIZE  0x400  // 1KB

// The beginning and length of an attribute record are always aligned to an 8-byte boundary,
// relative to the beginning of the file record.
#define ATTR_RECORD_ALIGNMENT  8

// FILE_RECORD_END seems to follow AttributeEnd in every file record starting with $Quota.
// No clue what data is being represented here.
#define FILE_RECORD_END  0x11477982

#define NTFS_FILE_NAME_POSIX          0
#define NTFS_FILE_NAME_WIN32          1
#define NTFS_FILE_NAME_DOS            2
#define NTFS_FILE_NAME_WIN32_AND_DOS  3

#define NTFS_FILE_TYPE_READ_ONLY  0x1
#define NTFS_FILE_TYPE_HIDDEN     0x2
#define NTFS_FILE_TYPE_SYSTEM     0x4
#define NTFS_FILE_TYPE_ARCHIVE    0x20
#define NTFS_FILE_TYPE_REPARSE    0x400
#define NTFS_FILE_TYPE_COMPRESSED 0x800
#define NTFS_FILE_TYPE_DIRECTORY  0x10000000

#define NTFS_FILE_CASE_SENSITIVE  FALSE

// Indexed Flag in Resident attributes - still somewhat speculative
#define RA_INDEXED  0x01

#define RA_HEADER_LENGTH (FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR))

// MFT Metafiles
#define NTFS_FILE_MFT              0
#define NTFS_FILE_MFTMIRR          1
#define NTFS_FILE_LOGFILE          2
#define NTFS_FILE_VOLUME           3
#define NTFS_FILE_ATTRDEF          4
#define NTFS_FILE_ROOT             5
#define NTFS_FILE_BITMAP           6
#define NTFS_FILE_BOOT             7
#define NTFS_FILE_BADCLUS          8
#define NTFS_FILE_QUOTA            9
#define NTFS_FILE_UPCASE           10
#define NTFS_FILE_EXTEND           11
#define NTFS_FILE_FIRST_USER_FILE  16


/* BOOT SECTOR STRUCTURES ****************************************************/

#include <pshpack1.h>

typedef struct _BIOS_PARAMETERS_BLOCK
{
    USHORT    BytesPerSector;      // 0x0B
    UCHAR     SectorsPerCluster;   // 0x0D
    UCHAR     Unused0[7];          // 0x0E
    UCHAR     MediaId;             // 0x15
    USHORT    Unused1;             // 0x16
    USHORT    SectorsPerTrack;     // 0x18
    USHORT    Heads;               // 0x1A
    DWORD32   HiddenSectorsCount;  // 0x1C
    DWORD32   Unused2;             // 0x20
} BIOS_PARAMETERS_BLOCK, * PBIOS_PARAMETERS_BLOCK;

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
} EXTENDED_BIOS_PARAMETERS_BLOCK, * PEXTENDED_BIOS_PARAMETERS_BLOCK;

typedef struct _BOOT_SECTOR
{
    UCHAR                           Jump[3];         // 0x00
    ULARGE_INTEGER                  OEMID;           // 0x03
    BIOS_PARAMETERS_BLOCK           BPB;             // 0x0B
    EXTENDED_BIOS_PARAMETERS_BLOCK  EBPB;            // 0x24
    UCHAR                           BootStrap[426];  // 0x54
    USHORT                          EndSector;       // 0x1FE
} BOOT_SECTOR, * PBOOT_SECTOR;

#include <poppack.h>


/* FILES DATA ****************************************************************/

typedef struct _NTFS_RECORD_HEADER
{
    ULONG     Magic;        // 0x00, magic 'FILE'
    USHORT    UsaOffset;    // 0x04, offset to the update sequence
    USHORT    UsaCount;     // 0x06, size in words of Update Sequence Number & Array (S)
    ULONGLONG Lsn;          // 0x08, $LogFile Sequence Number
} NTFS_RECORD_HEADER, *PNTFS_RECORD_HEADER;

typedef enum _MFT_RECORD_FLAGS
{
    MFT_RECORD_NOT_USED = 0x0000,
    MFT_RECORD_IN_USE = 0x0001,
    MFT_RECORD_IS_DIRECTORY = 0x0002
} MFT_RECORD_FLAGS, *PMFT_RECORD_FLAGS;

typedef struct _FILE_RECORD_HEADER
{
    NTFS_RECORD_HEADER Header;       // 0x00
    USHORT     SequenceNumber;       // 0x10, sequence number
    USHORT     LinkCount;            // 0x12, hard link count
    USHORT     AttributeOffset;      // 0x14, offset to the first Attribute
    USHORT     Flags;                // 0x16, flags (see MFT_RECORD_FLAGS)
    ULONG      BytesInUse;           // 0x18, real size of the FILE record
    ULONG      BytesAllocated;       // 0x1C, allocated size of the FILE record
    ULONGLONG  BaseFileRecord;       // 0x20, file reference to the base FILE record
    USHORT     NextAttributeNumber;  // 0x28, Next Attribute Id
    USHORT     Padding;              // 0x2A, align to 4 UCHAR boundary (XP)
    ULONG      MFTRecordNumber;      // 0x2C, number of this MFT Record (XP)
} FILE_RECORD_HEADER, * PFILE_RECORD_HEADER;


/* ATTRIBUTES COMMON *********************************************************/

typedef enum _ATTR_FLAGS
{
    ATTR_IS_COMPRESSED = 0x1,
    ATTR_IS_ENCRYPTED  = 0x4000,
    ATTR_IS_SPARSE     = 0x8000
} ATTR_FLAGS, *PATTR_FLAGS;

typedef enum _ATTRIBUTE_TYPE
{
    AttributeStandardInformation = 0x10,
    AttributeAttributeList       = 0x20,
    AttributeFileName            = 0x30,
    AttributeObjectId            = 0x40,
    AttributeSecurityDescriptor  = 0x50,
    AttributeVolumeName          = 0x60,
    AttributeVolumeInformation   = 0x70,
    AttributeData                = 0x80,
    AttributeIndexRoot           = 0x90,
    AttributeIndexAllocation     = 0xA0,
    AttributeBitmap              = 0xB0,
    AttributeReparsePoint        = 0xC0,
    AttributeEAInformation       = 0xD0,
    AttributeEA                  = 0xE0,
    AttributePropertySet         = 0xF0,
    AttributeLoggedUtilityStream = 0x100,
    AttributeEnd                 = 0xFFFFFFFF
} ATTRIBUTE_TYPE, *PATTRIBUTE_TYPE;

typedef struct _NTFS_ATTR_RECORD
{
    ULONG   Type;
    ULONG   Length;
    UCHAR   IsNonResident;
    UCHAR   NameLength;
    USHORT  NameOffset;
    USHORT  Flags;
    USHORT  Instance;
    union
    {
        struct
        {
            ULONG   ValueLength;
            USHORT  ValueOffset;
            UCHAR   Flags;
            UCHAR   Reserved;
        } Resident;

        struct
        {
            ULONGLONG  LowestVCN;
            ULONGLONG  HighestVCN;
            USHORT     MappingPairsOffset;
            USHORT     CompressionUnit;
            UCHAR      Reserved[4];
            LONGLONG   AllocatedSize;
            LONGLONG   DataSize;
            LONGLONG   InitializedSize;
            LONGLONG   CompressedSize;
        } NonResident;
    };
} NTFS_ATTR_RECORD, *PNTFS_ATTR_RECORD;


/* ATTRIBUTES STRUCTURES *****************************************************/

typedef struct _STANDARD_INFORMATION
{
    ULONGLONG CreationTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONG     FileAttribute;
    ULONG     AlignmentOrReserved[3];

    // UNIMPLEMENTED
#if 0
    ULONG QuotaId;
    ULONG SecurityId;
    ULONGLONG QuotaCharge;
    USN Usn;
#endif
} STANDARD_INFORMATION, * PSTANDARD_INFORMATION;

typedef struct _FILENAME_ATTRIBUTE
{
    ULONGLONG DirectoryFileReferenceNumber;
    ULONGLONG CreationTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    ULONG     FileAttributes;
    union
    {
        struct
        {
            USHORT PackedEaSize;
            USHORT AlignmentOrReserved;
        } EaInfo;

        ULONG ReparseTag;
    } Extended;
    UCHAR NameLength;
    UCHAR NameType;
    WCHAR Name[1];
} FILENAME_ATTRIBUTE, *PFILENAME_ATTRIBUTE;

typedef struct _VOLUME_INFORMATION_ATTRIBUTE
{
    BYTE   Unused[8];
    BYTE   MajorVersion;
    BYTE   MinorVersion;
    USHORT Flags;
} VOLUME_INFORMATION_ATTRIBUTE, *PVOLUME_INFORMATION_ATTRIBUTE;


/* PROTOTYPES ****************************************************************/

ULONG
NTAPI NtGetTickCount(VOID); 

VOID
GetSystemTimeAsFileTime(OUT PFILETIME lpFileTime);

// bootsect.c

NTSTATUS
WriteBootSector(IN HANDLE h,
                IN GET_LENGTH_INFORMATION* gli,
                IN PDISK_GEOMETRY dg);

// attrib.c

VOID
AddStandardInformationAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                                OUT PNTFS_ATTR_RECORD   AttributeAddress);

VOID
AddFileNameAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                     OUT PNTFS_ATTR_RECORD   AttributeAddress,
                     IN  LPCWSTR             FileName,
                     IN  DWORD32             MftRecordNumber);

VOID
AddEmptyDataAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                      OUT PNTFS_ATTR_RECORD   AttributeAddress);

VOID
AddEmptyVolumeNameAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                            OUT PNTFS_ATTR_RECORD   AttributeAddress);

VOID
AddVolumeInformationAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                              OUT PNTFS_ATTR_RECORD   AttributeAddress,
                              IN  BYTE                MajorVersion,
                              IN  BYTE                MinorVersion);

// files.c

NTSTATUS
WriteMetafiles(IN HANDLE h, IN  GET_LENGTH_INFORMATION* gli);

#endif