
#include "../xboxvmp.h"
#include <debug.h>

VOID
Encoder_QueryVideoModes(PXBOXVMP_DEVICE_EXTENSION DeviceExtension)
{
    switch (DeviceExtension->EncoderID)
    {
        case Conexant:
            Conexant_QueryVideoModes();
            break;
        case Focus:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Focus");
            break;
        case Xcalibur:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Xcaliber, unsupported atm!");
            break;
    }
}

VOID
Encoder_AdjustVideoMode(PXBOXVMP_DEVICE_EXTENSION DeviceExtension)
{
    switch (DeviceExtension->EncoderID)
    {
        case Conexant:
            Conexant_AdjustVideoMode();
            break;
        case Focus:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Focus");
            break;
        case Xcalibur:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Xcaliber, unsupported atm!");
            break;
    }
}

ULONG32
Encoder_Detect()
{
    ULONG32 ResultHolder;
    /* This is kind of a stupid way to handle this, but it works so far so... */
    I2CTransmitByteGetReturn(Conexant, 0x00, (ULONG*)&ResultHolder);
    if (ResultHolder != 0)
    {
        return Conexant;
    }
    else
    {
        I2CTransmitByteGetReturn(Focus, 0x00, (ULONG*)&ResultHolder);
    }
    if (ResultHolder != 0)
    {
        return Focus;
    }
    else
    {
        I2CTransmitByteGetReturn(Xcalibur, 0x00, (ULONG*)&ResultHolder);
    }
    if (ResultHolder != 0)
    {
        return Xcalibur;
    }

    return 0;
}

VOID
Encoder_Reset(PXBOXVMP_DEVICE_EXTENSION DeviceExtension)
{
    switch (DeviceExtension->EncoderID)
    {
        case Conexant:
            Conexant_Reset();
            break;
        case Focus:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Focus");
            break;
        case Xcalibur:
            DPRINT1("Encoder_QueryVideoModes: Your encoder is an Xcaliber, unsupported atm!");
            break;
    }
}

