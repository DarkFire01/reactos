#pragma once

#pragma pack(1)
/* GPT Header */
typedef struct _GPT_HEADER {
    unsigned char signature[8];
    unsigned int fixed;
    unsigned int headerSize;
    unsigned char headerCRC32[4];
    unsigned int reserved;
    unsigned long long firstHeaderLBA;
    unsigned long long secondHeaderLBA;
    unsigned long long firstPartLBA;
    unsigned long long lastPartLBA;
    unsigned char guid[16];
    unsigned long long startPartHeaderLBA;
    unsigned int partCount;
    unsigned int partHeaderSize;
    unsigned char partSeqCRC32[4];
} GPT_HEADER, *PGPT_HEADER;

/* GPT Partition Header */
typedef struct _GPT_PARTITION_HEADER {
    unsigned char partTypeGUID[16];
    unsigned char partGUID[16];
    unsigned long long firstLBA;
    unsigned long long lastLBA;
    unsigned char label[8];
    unsigned char name[72];
} GPT_PARTITION_HEADER ,*PGPT_PARTITION_HEADER;
#pragma pack(pop)

