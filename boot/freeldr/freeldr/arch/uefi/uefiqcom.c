/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Keeps the USB IOMMU domain of Qualcomm firmware for a kernel debugger on USB
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/*
 * UsbConfigDxe of the SDM845 Windows firmware deletes its USB IOMMU domain when
 * boot services exit, which resets the controller's stream in the SMMU. The
 * controller's DMA then stalls the bus once the kernel debugger uses it. The
 * driver only deletes the domain while its valid flag is set.
 */
#define USBCFG_EXIT_HANDLER         0x1C38
#define USBCFG_USB_EXIT             0x5294
#define USBCFG_DOMAIN_VALID         0x14818

static const UCHAR UsbCfgExitHandlerCode[] =
{
    0xF3, 0x0F, 0x1E, 0xF8, 0xFD, 0x7B, 0x01, 0xA9, 0xFD, 0x43, 0x00, 0x91, 0x94, 0x0D, 0x00, 0x94,
    0xE0, 0x00, 0xF8, 0xB6, 0x61, 0x00, 0x00, 0xD0, 0x62, 0x00, 0x00, 0xD0, 0x21, 0x5C, 0x17, 0x91
};

static const UCHAR UsbCfgUsbExitCode[] =
{
    0xFF, 0xC3, 0x04, 0xD1, 0xFC, 0x8B, 0x00, 0xF9, 0xFD, 0x7B, 0x12, 0xA9, 0xFD, 0x83, 0x04, 0x91,
    0xE8, 0x03, 0x1F, 0xAA, 0xA8, 0x83, 0x1E, 0xF8, 0x01, 0x00, 0x00, 0x14, 0x68, 0x00, 0x00, 0xF0
};

/* File name of UsbConfigDxe in the firmware volume */
static const EFI_GUID UsbCfgFileGuid =
    { 0xCD823A4D, 0x7DEC, 0x4531, { 0xAE, 0x5D, 0x41, 0x34, 0xFA, 0x41, 0x27, 0xB8 } };

extern EFI_SYSTEM_TABLE* GlobalSystemTable;

/* TRUE when the image was loaded from the firmware volume file of UsbConfigDxe */
static
BOOLEAN
UefiIsUsbConfigFile(
    _In_opt_ const EFI_DEVICE_PATH_PROTOCOL* Path)
{
    const MEDIA_FW_VOL_FILEPATH_DEVICE_PATH* File;
    USHORT Length;

    while (Path != NULL && Path->Type != END_DEVICE_PATH_TYPE)
    {
        Length = (USHORT)(Path->Length[0] | (Path->Length[1] << 8));
        if (Length < sizeof(*Path))
            return FALSE;

        if (Path->Type == MEDIA_DEVICE_PATH && Path->SubType == MEDIA_PIWG_FW_FILE_DP &&
            Length >= sizeof(*File))
        {
            File = (const MEDIA_FW_VOL_FILEPATH_DEVICE_PATH*)Path;
            return RtlEqualMemory(&File->FvFileName, &UsbCfgFileGuid, sizeof(UsbCfgFileGuid));
        }

        Path = (const EFI_DEVICE_PATH_PROTOCOL*)((const UCHAR*)Path + Length);
    }

    return FALSE;
}

/* TRUE when the image is the UsbConfigDxe build whose layout is known */
static
BOOLEAN
UefiIsKnownUsbConfig(
    _In_ const EFI_LOADED_IMAGE_PROTOCOL* Image)
{
    const UCHAR* Base = Image->ImageBase;

    if (Base == NULL || Image->ImageSize <= USBCFG_DOMAIN_VALID)
        return FALSE;

    return RtlEqualMemory(Base + USBCFG_EXIT_HANDLER, UsbCfgExitHandlerCode, sizeof(UsbCfgExitHandlerCode)) &&
           RtlEqualMemory(Base + USBCFG_USB_EXIT, UsbCfgUsbExitCode, sizeof(UsbCfgUsbExitCode));
}

/**
 * @brief Stops the firmware's USB driver from deleting the USB IOMMU domain at exit.
 *
 * The rest of its exit work, stopping the controller and its clocks, still runs.
 */
VOID
UefiKeepUsbIommuDomain(VOID)
{
    EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* Image;
    EFI_HANDLE* Handles = NULL;
    UINTN Count = 0;
    UINTN i;
    PUCHAR Valid;
    EFI_STATUS Status;

    Status = GlobalSystemTable->BootServices->LocateHandleBuffer(ByProtocol,
                                                                 &LoadedImageGuid,
                                                                 NULL,
                                                                 &Count,
                                                                 &Handles);
    if (Status != EFI_SUCCESS)
    {
        TuiPrintf("freeldr: no loaded images to look at: %lx\n", (ULONG)Status);
        return;
    }

    TuiPrintf("freeldr: looking for the firmware's USB driver in %lu images\n", (ULONG)Count);
    for (i = 0; i < Count; i++)
    {
        Status = GlobalSystemTable->BootServices->HandleProtocol(Handles[i],
                                                                 &LoadedImageGuid,
                                                                 (VOID**)&Image);
        if (Status != EFI_SUCCESS || !UefiIsUsbConfigFile(Image->FilePath))
            continue;

        TuiPrintf("freeldr: USB driver at %p, size %lx\n", Image->ImageBase, (ULONG)Image->ImageSize);
        if (!UefiIsKnownUsbConfig(Image))
        {
            TuiPrintf("freeldr: unknown build of the USB driver, left alone\n");
            break;
        }

        Valid = (PUCHAR)Image->ImageBase + USBCFG_DOMAIN_VALID;
        if (*Valid != 1)
        {
            TuiPrintf("freeldr: the firmware has no USB IOMMU domain to keep\n");
            break;
        }

        *Valid = 0;
        TuiPrintf("freeldr: keeping the firmware's USB IOMMU domain for the debugger\n");
        break;
    }

    GlobalSystemTable->BootServices->FreePool(Handles);
}
