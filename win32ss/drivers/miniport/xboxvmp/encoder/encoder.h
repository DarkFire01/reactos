
ULONG32
Encoder_Detect();

#define Conexant 0x54
#define Focus 0x6A
#define Xcalibur 0x70

VOID
Encoder_QueryVideoModes(PXBOXVMP_DEVICE_EXTENSION DeviceExtension);
VOID
Encoder_AdjustVideoMode(PXBOXVMP_DEVICE_EXTENSION DeviceExtension);
VOID
Encoder_Reset(PXBOXVMP_DEVICE_EXTENSION DeviceExtension);


VOID
Conexant_QueryVideoModes();
VOID
Conexant_AdjustVideoMode();
VOID
Conexant_Reset();