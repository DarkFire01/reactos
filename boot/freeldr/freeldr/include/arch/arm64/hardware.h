
#define FREELDR_BASE       0x0001F000
#define FREELDR_PE_BASE    0x0001F000
#define MAX_FREELDR_PE_SIZE 0xFFFFFF

#define DiskReadBuffer 0

VOID StallExecutionProcessor(ULONG Microseconds);
VOID HalpCalibrateStallExecution(VOID);
#define MAX_BIOS_DESCRIPTORS 80
#define KSEG0_BASE              (ULONG_PTR)0x80000000
#define DriveMapGetBiosDriveNumber(DeviceName) 0
#define KI_USER_SHARED_DATA     0xFFFFF78000000000ULL

#define DbgBreakPoint __debugbreak
