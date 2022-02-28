#include "fs.h"

/* "UEFI has a inhouse file system implementation so why use this >:D?""
 * is what a nerd will ask.
 * MANY filesystems aren't supported by standard UEFI, and many implementations fail to properly
 * Implement the file system functions. 
 * The goal of ROSEFI is to give us the most functionality we can achieve, that's not an easy thing
 * 
 *
 */
EFI_STATUS
RefiInitalizeFileSystem()
{
    return 0;
}

EFI_STATUS
RefiLoadFile()
{
    return 0;
}

EFI_STATUS
RefiCloseFile()
{
    return 0;
}