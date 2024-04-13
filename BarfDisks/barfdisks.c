#include "utility.h"
bool AreWeWritingNtfs;


bool
DetectDiskScheme(char * DriveName)
{
    unsigned long long sector = 1;
    char secBuf[SECTOR_SIZE];

    printf("We're going going to first detect partiton data\r\n");
    printf("This utility makes the assumption you're not using protective MBR\r\n");
    bool res = ReadSect(DriveName, secBuf, sector);
    if (!res) {
        printf("Can't read sector - Are you sure you're an admin/sudo?\r\n");
        return -1;
    }

    GPT_HEADER *GptHeader;
    GptHeader = (GPT_HEADER*)secBuf;

    if (GptHeader->signature[0] == 'E' && GptHeader->signature[1] == 'F'
        && GptHeader->signature[2] == 'I')
    {
        printf("Congrats you're a GPT disk!\r\n");
        printGPTHeader(GptHeader);
        return TRUE;
    }

    sector = 0;
    res = ReadSect(DriveName, secBuf, sector);
    if (!res) {
        printf("Can't read sector - Are you sure you're an admin/sudo?\r\n");
        return -1;
    }
    MASTER_BOOT_RECORD *MbrHeader;
    MbrHeader = (MASTER_BOOT_RECORD*)secBuf;
    

    if (MbrHeader->Signature == 0xAA55) 
    {
        printf("Congrats! You're an MBR disk\n");
        printMBRHeader(MbrHeader);
        return FALSE;
    }
    else
    {
        printf("This drive seems unformatted\r\n");
    }
}

void
WriteNtfsWeirdness(unsigned long long firstLBAoffset, char *drv)
{
    char secBuf[SECTOR_SIZE];
    FAT_BOOTSECTOR32 *Fat32Part;

    ReadSect(drv, secBuf, firstLBAoffset);
    Fat32Part = (GPT_HEADER*)secBuf;

    Fat32Part->sOEMName[0] = "G";
    Fat32Part->sOEMName[1] = "A";
    Fat32Part->sOEMName[2] =  "Y";
    Fat32Part->sOEMName[3] =  0;

    WriteSect(drv, Fat32Part, firstLBAoffset);
    ReadSect(drv, secBuf, firstLBAoffset);
    Fat32Part = (GPT_HEADER*)secBuf;
    PrintFat32Header(Fat32Part);
    for(;;)
    {

    }
}

//-----------------------------------------------------------------------------
void printGPTPartHeader(int index, GPT_PARTITION_HEADER *p, char *drv)
{
    char* SSDMsg[2] = {"No", "Yes"};

    int is4KAlign = 0;
    if ((p->firstLBA * 512) % 4096 == 0) {
        is4KAlign = 1;
    }

    printf("GPT Partition Header #%02d \r\n", index);
    printf("--------------------------- \r\n");
    printG("Partition GUID       - ", p->partTypeGUID);
    printS("GUID                 - ", p->partGUID, 16, 1);
    printf("Partition Begin LBA  - 0x%llX (%llu) \r\n", p->firstLBA, p->firstLBA);
    printf("Partition End LBA    - 0x%llX (%llu) \r\n", p->lastLBA, p->lastLBA);
    printf("4K Alignment         - %s \r\n",SSDMsg[is4KAlign]);
    printf("ASD SSD Benchmark    - %llu \r\n", p->firstLBA * 512 / 1024);
    printf("\r\n");
    printf("\r\n");

    char secBuf[SECTOR_SIZE];
    FAT_BOOTSECTOR32 *Fat32Part;

    ReadSect(drv, secBuf, p->firstLBA);
    Fat32Part = (GPT_HEADER*)secBuf;
    if (strncmp(Fat32Part->sBS_FilSysType, "FAT", 3) == 0)
    {
        printf("This is a fat32 partition\r\n");
        PrintFat32Header(Fat32Part);
    }
    else if (strncmp(Fat32Part->sOEMName, "NTFS", 4) == 0)
    {
        printf("This is a NTFS partition\r\n");

        if (AreWeWritingNtfs == TRUE)
        {
            printf("prepping to modify ntfs partition\r\n");
            PrintFat32Header(Fat32Part);

            WriteNtfsWeirdness(p->firstLBA, drv);
        }
    
    }
    else
    {
        printf("Unknown FS detection\r\n");
        PrintFat32Header(Fat32Part);
        printf("\r\n");
    }
    printf("\r\n");

         
}

//-----------------------------------------------------------------------------
void processGPTPartitionHeader(char *drv, GPT_HEADER *gptHeader)
{
    unsigned long long startSector = gptHeader->startPartHeaderLBA;
    unsigned int partHeaderSize = gptHeader->partHeaderSize;

    if (partHeaderSize != 128) {
        printf("GPT Partition Header size is %d, not supported yet. \r\n", partHeaderSize);
        return;
    }

    unsigned partIndex = 1;
    char secBuf[SECTOR_SIZE];

    while (1) {
        ReadSect(drv, secBuf, startSector);

        for (int i = 0; i < 4; i++) {
            GPT_PARTITION_HEADER *p = (GPT_PARTITION_HEADER*)&secBuf[128 * i];

            if (p->partTypeGUID[0] == 0x00 && p->partTypeGUID[1] == 0x00) {
                unsigned int count = (startSector - gptHeader->startPartHeaderLBA) * 4 + i;
                printf("Total Partitions are %d. \r\n", count);
                return;
            }
            printGPTPartHeader(partIndex++, p, drv);
        }

        startSector++;
    }
}
int count = 0;
VOID
PrintMbrPartitions(MBR_PARTITION_RECORD MbrHeader, int index,char* DriveName)
{
    printf("Printing MBR index: %x\r\n", index);
    if (MbrHeader.StartHead)
    {
        printf("BootIndicator - %X\r\n"   ,               MbrHeader.BootIndicator);
        printf("StartHead     - %X\r\n"   ,       MbrHeader.StartHead);
        printf("StartSector   - %X\r\n"   ,           MbrHeader.StartSector);
        printf("StartTrack    - %X\r\n"   ,         MbrHeader.StartTrack);
        printf("OSIndicator   - %X\r\n"   ,           MbrHeader.OSIndicator);
        printf("EndHead       - %X\r\n"   ,   MbrHeader.EndHead);
        printf("EndSector     - %X\r\n"   ,       MbrHeader.EndSector);
        printf("EndTrack      - %X\r\n"   ,     MbrHeader.EndTrack);
        printf("StartingLBA   - %X\r\n"   ,           MbrHeader.StartingLBA);
        printf("SizeInLBA     - %X\r\n"   ,       MbrHeader.SizeInLBA);
        printf("\r\n");

         char secBuf[SECTOR_SIZE];
         FAT_BOOTSECTOR32 *Fat32Part;
        
         ReadSect(DriveName, secBuf, MbrHeader.StartingLBA);
         Fat32Part = (GPT_HEADER*)secBuf;

        if (strncmp(Fat32Part->sBS_FilSysType, "FAT", 3) == 0)
        {
            printf("This is a fat32 partition\r\n");
            PrintFat32Header(Fat32Part);
        }

        else if (strncmp(Fat32Part->sOEMName, "NTFS", 4) == 0)
        {
            printf("This is a NTFS partition\r\n");
            for(;;)
            {

            }
        }
         printf("\r\n");
         count++;
    }

}



VOID
DumpMbrPartitionTables(char* DriveName)
{
    unsigned long long sector = 0;
    char secBuf[SECTOR_SIZE];
    bool res = ReadSect(DriveName, secBuf, sector);
    if (!res) {
        printf("Can't read sector - Are you sure you're an admin/sudo?\r\n");
        return -1;
    }
    MASTER_BOOT_RECORD *MbrHeader;
    MbrHeader = (MASTER_BOOT_RECORD*)secBuf;
    for (int i = 0; i < 4; i++)
    {
        PrintMbrPartitions(MbrHeader->Partition[i], i, DriveName);
    }

    printf("total partition count - %d\r\n",count);

}

int main(int argc, char* argv[])
{
    printf("Welcome to the barf disk utility - input a disk to attack\r\n");
    char* string;
    unsigned int DiskIndex = 0;
    AreWeWritingNtfs = false;
    /* make a garbage bufer */
    char DriveName[64];
    memset(DriveName, 0x00, sizeof(DriveName));

    if (argc >= 2) {
        DiskIndex = atoi(argv[1]);
      //  Input = argv[2];
    }

    for (int i = 0; i < argc; i++) {
        string  = argv[i];
        if (strncmp(string, "writentfs", 9) == 0)
        {
            printf("We're going to test format an ntfs partition\r\n");
            AreWeWritingNtfs = true;
        }
    }


    /* First turn this index into a drive name */
    TranslateDriveName(DriveName, DiskIndex);

    printf("You've selected drive: %s\r\n", DriveName);

    bool AreWeGpt = DetectDiskScheme(DriveName);
    if (AreWeGpt == TRUE)
    {
        printf("Dumping GPT partitions\r\n......\r\n");
        unsigned long long sector = 1;
        char secBuf[SECTOR_SIZE];
        bool res = ReadSect(DriveName, secBuf, sector);
        if (!res) {
            printf("Can't read sector - Are you sure you're an admin/sudo?\r\n");
            return -1;
        }

        GPT_HEADER *GptHeader;
        GptHeader = (GPT_HEADER*)secBuf;

        processGPTPartitionHeader(DriveName, GptHeader);
    }
    else
    {
        DumpMbrPartitionTables(DriveName);
    }
    return 0;
}