
#include "../../include/rosefip.h"

/* Entry-point to kernel */
typedef VOID (NTAPI *KERNEL_ENTRY_POINT) (PLOADER_PARAMETER_BLOCK LoaderBlock);

RefiLoadAndBootReactOSCommon(
    IN USHORT OperatingSystemVersion,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN PCSTR BootOptions,
    IN PCSTR BootPath);