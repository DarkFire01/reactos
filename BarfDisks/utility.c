#include "utility.h"

void
TranslateDriveName(char *DriveName,
                    unsigned int DiskIndex)
{
#ifdef __unix__
    sprintf(DriveName, "/dev/sd%c", 'a' + DiskIndex);
#else
    sprintf(DriveName, "\\\\.\\PhysicalDrive%d", DiskIndex);
#endif
}

/* num is sector number, it starts with 0 */
bool
WriteSect_InC(const char *dsk,
             char *buf,
             unsigned long long num)
{
    if (strlen(dsk) == 0) {
        printf("dsk is null\r\n");
        return false;
    }

    FILE *f = fopen(dsk, "rb");
    if (!f) {
        printf("cant open DISK: %s\r\n", dsk);
        return false;
    }

    unsigned long long n = num * SECTOR_SIZE;

    fseeko(f, n, SEEK_SET);
    fwrite(buf, SECTOR_SIZE, 1, f);
     if (! fwrite(buf, SECTOR_SIZE, 1, f)) {
        printf("couldnt write: %s\r\n", dsk);
        return false;
    }

    fclose(f);

    return true;
}

/* num is sector number, it starts with 0 */
bool
ReadSect_InC(const char *dsk,
             char *buf,
             unsigned long long num)
{
    if (strlen(dsk) == 0) {
        printf("dsk is null\r\n");
        return false;
    }

    FILE *f = fopen(dsk, "rb");
    if (!f) {
        printf("cant open DISK: %s\r\n", dsk);
        return false;
    }

    unsigned long long n = num * SECTOR_SIZE;

    fseeko(f, n, SEEK_SET);
    fread(buf, SECTOR_SIZE, 1, f);

    fclose(f);

    return true;
}

void printG(char *format, unsigned char *buf)
{
    printf("%s", format);

    printf(
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-",
        buf[3], buf[2], buf[1], buf[0],
        buf[5], buf[4],
        buf[7], buf[6]
    );

    for (int i = 8; i <= 15; i++) {
        printf("%02X", buf[i]);

        if (i == 9) {
            printf("-");
        }
    }

    printf(" \r\n");
}

void
printS(const char *str, unsigned char *buf, int size, int isHex)
{
    printf("%s", str);

    for (int i = 0; i < size; i++) {
        if (isHex) {
            printf("%02X", buf[i]);
        } else {
            printf("%c", buf[i]);
        }
    }
    printf(" \r\n");
}


void printGPTHeader(GPT_HEADER *p)
{
    printf("Le GPT Header \r\n");
    printf("------------------ \r\n");
    printS("Signature              - ", p->signature, 8, 0);
    printf("Fixed                  - 0x%08X \r\n", p->fixed);
    printf("GPT Header Size        - %d \r\n", p->headerSize);
    printS("Header CRC32           - ", p->headerCRC32, 4, 1);
    printf("Reserved               - %d \r\n", p->reserved);
    printf("1st Header LBA         - 0x%llX (%llu) \r\n", p->firstHeaderLBA, p->firstHeaderLBA);
    printf("2nd Header LBA         - 0x%llX (%llu) \r\n", p->secondHeaderLBA, p->secondHeaderLBA);
    printf("First Partition LBA    - 0x%llX \r\n", p->firstPartLBA);
    printf("Last Partition LBA     - 0x%llX \r\n", p->lastPartLBA);
    printS("GUID                   - ", p->guid, 16, 1);
    printf("Start Partition Header - 0x%llX (%llu) \r\n", p->startPartHeaderLBA, p->startPartHeaderLBA);
    printf("Partition Count        - %d \r\n", p->partCount);
    printf("Partition Header Size  - %d \r\n", p->partHeaderSize);
    printS("Partition Seq CRC32    - ", p->partSeqCRC32, 4, 1);
}


void printMBRHeader(MASTER_BOOT_RECORD *p)
{
    
    printf("Le MBR Header \r\n");
    printf("--------------------- \r\n");
    printf("Boot strap code first instruction - %X\r\n", p->BootStrapCode[0]);
    printS("UniqueMbrSig: - ", p->UniqueMbrSignature, 8, 1);
    printf("Signature - %X\r\n", p->Signature);
}
void
PrintFat32Header(FAT_BOOTSECTOR32 *p)
{
    printf("FAT32 Header \r\n");
    printf("-------------__----- \r\n");


    printf("sJmpBoot[0]      : 0x%X\r\n", p->sJmpBoot[0]);
    printf("sJmpBoot[1]      : 0x%X\r\n", p->sJmpBoot[1]);
    printf("sJmpBoot[2]      : 0x%X\r\n", p->sJmpBoot[2]);
    printf("sJmpBoot[3]      : 0x%X\r\n", p->sJmpBoot[3]);
    printf("sOEMName         : 0x%s\r\n", p->sOEMName);
#if 1
    printf("wBytsPerSec      : 0x%X\r\n", p->wBytsPerSec);
    printf("bSecPerClus      : 0x%X\r\n", p->bSecPerClus);
    printf("wRsvdSecCnt      : 0x%X\r\n", p->wRsvdSecCnt);
    printf("bNumFATs         : 0x%X\r\n", p->bNumFATs);
    printf("wRootEntCnt      : 0x%X\r\n", p->wRootEntCnt);
    printf("wTotSec16        : 0x%X\r\n", p->wTotSec16);
    printf("bMedia           : 0x%X\r\n", p->bMedia);
    printf("wFATSz16         : 0x%X\r\n", p->wFATSz16);
    printf("wSecPerTrk       : 0x%X\r\n", p->wSecPerTrk);
    printf("wNumHeads        : 0x%X\r\n", p->wNumHeads);
    printf("dHiddSec         : 0x%X\r\n", p->dHiddSec);
    printf("dTotSec32        : 0x%X\r\n", p->dTotSec32);
    printf("dFATSz32         : 0x%X\r\n", p->dFATSz32);
    printf("wExtFlags        : 0x%X\r\n", p->wExtFlags);
    printf("wFSVer           : 0x%X\r\n", p->wFSVer);
    printf("dRootClus        : 0x%X\r\n", p->dRootClus);
    printf("wFSInfo          : 0x%X\r\n", p->wFSInfo);
    printf("wBkBootSec       : 0x%X\r\n", p->wBkBootSec);
    printf("Reserved[12]     : 0x%X\r\n", p->Reserved[12]);
    printf("bDrvNum          : 0x%X\r\n", p->bDrvNum);
    printf("Reserved1        : 0x%X\r\n", p->Reserved1);
    printf("bBootSig         : 0x%X\r\n", p->bBootSig);
    printf("dBS_VolID        : 0x%X\r\n", p->dBS_VolID);
    printf("sVolLab      : 0x%X\r\n", p->sVolLab);
    printf("FilSysTyp: ");
    printf(p->sBS_FilSysType);
#endif
    printf("\r\n");
}
