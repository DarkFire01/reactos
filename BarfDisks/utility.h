
#pragma once

#ifdef __unix__
#define _LARGEFILE_SOURCE
#endif

#define SECTOR_SIZE         512

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifndef __unix__
#include <windows.h>
#endif
#include "mbr.h"
#include "gpt.h"
#include "fat32.h"

void TranslateDriveName(char *DriveName, unsigned int DiskIndex);

bool
WriteSect_InC(const char *dsk,
             char *buf,
             unsigned long long num);

bool
ReadSect_InC(const char *dsk,
             char *buf,
             unsigned long long num);
#define ReadSect(dsk , buf, num) ReadSect_InC(dsk, buf, num)
#define WriteSect(dsk, buf, num) WriteSect_InC(dsk, buf, num)
void
printS(const char *str, unsigned char *buf, int size, int isHex);


void printGPTHeader(GPT_HEADER *p);

void printMBRHeader(MASTER_BOOT_RECORD *p);
void printG(char *format, unsigned char *buf);

void
PrintFat32Header(FAT_BOOTSECTOR32 *p);