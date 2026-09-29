/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A disk on the wire every machine of this kind has had
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * The controller is the one an operating system already knows how to drive,
 * reached by port and moved a word at a time. It is deliberately not the
 * fastest way to carry a sector: it is the way that needs no driver written
 * for it, which is what lets something that predates this machine boot on it.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

/* Where the two channels answer, and the one register that sits apart */
#define ATA_PRIMARY_BASE        0x01F0
#define ATA_PRIMARY_CONTROL     0x03F6
#define ATA_PRIMARY_LINE        14
#define ATA_SECONDARY_BASE      0x0170
#define ATA_SECONDARY_CONTROL   0x0376
#define ATA_SECONDARY_LINE      15

/* Offsets from the base */
#define ATA_DATA                0
#define ATA_ERROR               1   /* Read */
#define ATA_FEATURES            1   /* Write */
#define ATA_SECTOR_COUNT        2
#define ATA_LBA_LOW             3
#define ATA_LBA_MID             4
#define ATA_LBA_HIGH            5
#define ATA_DRIVE               6
#define ATA_STATUS              7   /* Read */
#define ATA_COMMAND             7   /* Write */

#define ATA_REGISTER_COUNT      8

/* What the status register says */
#define STATUS_ERROR            0x01
#define STATUS_INDEX            0x02
#define STATUS_CORRECTED        0x04
#define STATUS_READY_DATA       0x08
#define STATUS_SEEK_DONE        0x10
#define STATUS_FAULT            0x20
#define STATUS_READY            0x40
#define STATUS_BUSY             0x80

/* What the error register says */
#define ATA_ERROR_ADDRESS_MARK      0x01
#define ATA_ERROR_TRACK_ZERO        0x02
#define ATA_ERROR_ABORTED           0x04
#define ATA_ERROR_NOT_FOUND         0x10
#define ATA_ERROR_UNCORRECTABLE     0x40

/* The drive register, whose top bits are not a choice */
#define DRIVE_SLAVE             0x10
#define DRIVE_LBA               0x40
#define DRIVE_HEAD_MASK         0x0F

/* What the control register is asked for */
#define CONTROL_NO_INTERRUPT    0x02
#define CONTROL_RESET           0x04

/* The commands that are answered */
#define COMMAND_READ_SECTORS    0x20
#define COMMAND_READ_NO_RETRY   0x21
#define COMMAND_WRITE_SECTORS   0x30
#define COMMAND_WRITE_NO_RETRY  0x31
#define COMMAND_SET_PARAMETERS  0x91
#define COMMAND_READ_MULTIPLE   0xC4
#define COMMAND_WRITE_MULTIPLE  0xC5
#define COMMAND_SET_MULTIPLE    0xC6
#define COMMAND_FLUSH           0xE7
#define COMMAND_FLUSH_EXT       0xEA
#define COMMAND_IDENTIFY        0xEC

#define SECTOR_SIZE             512

/* The shape reported for a disk whose real one nothing can ask about */
#define GEOMETRY_HEADS          16
#define GEOMETRY_SECTORS        63

/* The one removable size that still matters, and the shape it always had */
#define FLOPPY_1440_SECTORS             2880
#define FLOPPY_1440_HEADS               2
#define FLOPPY_1440_SECTORS_PER_TRACK   18

/* TYPES **********************************************************************/

typedef struct _STORAGE_DEVICE
{
    RTVM_DEVICE Device;

    USHORT BasePort;
    USHORT ControlPort;
    ULONG Line;

    /* The image, and how much of it there is */
    HANDLE Image;
    ULONG64 SectorCount;
    BOOLEAN ReadOnly;

    /* The shape it is described as having, worked out from its size */
    ULONG Cylinders;
    ULONG Heads;
    ULONG Sectors;

    /* Set only when the operator gave a shape rather than letting it be worked out */
    ULONG ForcedHeads;
    ULONG ForcedSectors;

    /* Whether this is the sort of medium that comes out of the machine */
    BOOLEAN Removable;

    /* The registers, as last written */
    UCHAR Features;
    UCHAR Count;
    UCHAR LbaLow;
    UCHAR LbaMid;
    UCHAR LbaHigh;
    UCHAR Drive;
    UCHAR Status;
    UCHAR Error;
    UCHAR Control;

    /*
     * Where a transfer in progress is held. A command moves its whole payload
     * here at once and the guest then carries it in or out a word at a time,
     * which is what the wire looks like from the other end.
     */
    UCHAR Buffer[SECTOR_SIZE * 256];
    ULONG BufferLength;
    ULONG BufferOffset;

    /* How many sectors are still to come after the one being carried */
    ULONG Remaining;
    ULONG64 NextSector;
    BOOLEAN Writing;

    BOOLEAN LineAsserted;

    CHAR ImagePath[MAX_PATH];
} STORAGE_DEVICE, *PSTORAGE_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE StorageVtable;

/* FUNCTIONS ******************************************************************/

static
VOID
StorageSetLine(
    _Inout_ PSTORAGE_DEVICE Storage,
    _In_ BOOLEAN Asserted)
{
    const RTVM_HOST_INTERFACE *Host = Storage->Device.Host;

    /* A channel told not to interrupt still finishes, it just says nothing */
    if (Storage->Control & CONTROL_NO_INTERRUPT)
        Asserted = FALSE;

    if (Asserted == Storage->LineAsserted)
        return;

    Storage->LineAsserted = Asserted;

    if (Host->SetInterruptLine != NULL)
        Host->SetInterruptLine(Host->Context, Storage->Line, Asserted);
}

/* Whether the drive the registers name is the one this device is */
static
BOOLEAN
StorageSelected(
    _In_ PSTORAGE_DEVICE Storage)
{
    return (Storage->Drive & DRIVE_SLAVE) == 0;
}

/**
 * @brief
 * Works out which sector the registers are pointing at.
 *
 * @remarks
 * Both ways of saying it are answered. Addressing by block is what everything
 * written since uses, and the older way of counting in cylinders, heads and
 * sectors is what a boot sector from before it will ask for.
 */
static
ULONG64
StorageTargetSector(
    _In_ PSTORAGE_DEVICE Storage)
{
    if (Storage->Drive & DRIVE_LBA)
    {
        return ((ULONG64)(Storage->Drive & DRIVE_HEAD_MASK) << 24) |
               ((ULONG64)Storage->LbaHigh << 16) |
               ((ULONG64)Storage->LbaMid << 8) |
               (ULONG64)Storage->LbaLow;
    }

    {
        ULONG Cylinder = ((ULONG)Storage->LbaHigh << 8) | Storage->LbaMid;
        ULONG Head = Storage->Drive & DRIVE_HEAD_MASK;
        ULONG Sector = Storage->LbaLow;

        /* Sectors have always been counted from one, unlike everything else */
        if (Sector == 0)
            return (ULONG64)-1;

        return ((ULONG64)Cylinder * Storage->Heads + Head) * Storage->Sectors +
               (Sector - 1);
    }
}

static
BOOLEAN
StorageSeek(
    _In_ PSTORAGE_DEVICE Storage,
    _In_ ULONG64 Sector)
{
    LARGE_INTEGER Offset;

    Offset.QuadPart = (LONGLONG)(Sector * SECTOR_SIZE);

    return SetFilePointerEx(Storage->Image, Offset, NULL, FILE_BEGIN) != FALSE;
}

static
VOID
StorageFail(
    _Inout_ PSTORAGE_DEVICE Storage,
    _In_ UCHAR Reason)
{
    Storage->Error = Reason;
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_ERROR;
    Storage->BufferLength = 0;
    Storage->BufferOffset = 0;
    Storage->Remaining = 0;
    StorageSetLine(Storage, TRUE);
}

/**
 * @brief
 * Reads the run of sectors the registers asked for into the buffer.
 */
static
VOID
StorageBeginRead(
    _Inout_ PSTORAGE_DEVICE Storage)
{
    ULONG Count = (Storage->Count == 0) ? 256 : Storage->Count;
    ULONG64 Sector = StorageTargetSector(Storage);
    ULONG Length = Count * SECTOR_SIZE;
    DWORD Read = 0;

    if ((Sector == (ULONG64)-1) || (Sector + Count > Storage->SectorCount))
    {
        StorageFail(Storage, ATA_ERROR_NOT_FOUND);
        return;
    }

    if (!StorageSeek(Storage, Sector) ||
        !ReadFile(Storage->Image, Storage->Buffer, Length, &Read, NULL))
    {
        StorageFail(Storage, ATA_ERROR_UNCORRECTABLE);
        return;
    }

    /* A short read is the end of a file that is not as long as it claims */
    if (Read < Length)
        memset(&Storage->Buffer[Read], 0, Length - Read);

    Storage->BufferLength = Length;
    Storage->BufferOffset = 0;
    Storage->Writing = FALSE;
    Storage->Remaining = 0;
    Storage->Error = 0;
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_READY_DATA;

    StorageSetLine(Storage, TRUE);
}

static
VOID
StorageBeginWrite(
    _Inout_ PSTORAGE_DEVICE Storage)
{
    ULONG Count = (Storage->Count == 0) ? 256 : Storage->Count;
    ULONG64 Sector = StorageTargetSector(Storage);

    if (Storage->ReadOnly)
    {
        StorageFail(Storage, ATA_ERROR_ABORTED);
        return;
    }

    if ((Sector == (ULONG64)-1) || (Sector + Count > Storage->SectorCount))
    {
        StorageFail(Storage, ATA_ERROR_NOT_FOUND);
        return;
    }

    /* Nothing is written until the guest has handed over the whole run */
    Storage->BufferLength = Count * SECTOR_SIZE;
    Storage->BufferOffset = 0;
    Storage->Writing = TRUE;
    Storage->NextSector = Sector;
    Storage->Error = 0;
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_READY_DATA;
}

static
VOID
StorageFinishWrite(
    _Inout_ PSTORAGE_DEVICE Storage)
{
    DWORD Written = 0;

    if (!StorageSeek(Storage, Storage->NextSector) ||
        !WriteFile(Storage->Image, Storage->Buffer,
                   Storage->BufferLength, &Written, NULL))
    {
        StorageFail(Storage, ATA_ERROR_UNCORRECTABLE);
        return;
    }

    Storage->BufferLength = 0;
    Storage->BufferOffset = 0;
    Storage->Writing = FALSE;
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE;

    StorageSetLine(Storage, TRUE);
}

/* Puts a run of characters into the identify block, in the order it wants */
static
VOID
StorageIdentifyText(
    _Out_writes_bytes_(Length) PUCHAR Target,
    _In_ PCSTR Text,
    _In_ ULONG Length)
{
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
    {
        CHAR Value = (Text[Index] != '\0') ? Text[Index] : ' ';

        /* Each pair arrives the other way round, so they are written so */
        Target[Index ^ 1] = (UCHAR)Value;

        if (Text[Index] == '\0')
        {
            ULONG Rest;

            for (Rest = Index + 1; Rest < Length; Rest++)
                Target[Rest ^ 1] = ' ';

            break;
        }
    }
}

/**
 * @brief
 * Fills in the block that says what this drive is.
 */
static
VOID
StorageIdentify(
    _Inout_ PSTORAGE_DEVICE Storage)
{
    PUSHORT Words = (PUSHORT)Storage->Buffer;
    ULONG64 Sectors = Storage->SectorCount;

    memset(Storage->Buffer, 0, SECTOR_SIZE);

    /* A fixed disk, which is what the top bits of the first word say */
    Words[0] = 0x0040;
    Words[1] = (USHORT)Storage->Cylinders;
    Words[3] = (USHORT)Storage->Heads;
    Words[6] = (USHORT)Storage->Sectors;

    StorageIdentifyText(&Storage->Buffer[20], "0", 20);
    StorageIdentifyText(&Storage->Buffer[46], "1.0", 8);
    StorageIdentifyText(&Storage->Buffer[54], "ReacTVmm disk", 40);

    /* How many sectors move in one go, and that the field means anything */
    Words[47] = 0x8001;
    Words[49] = 0x0200;
    Words[50] = 0x4000;

    /* The fields above are the ones that are valid */
    Words[53] = 0x0007;
    Words[54] = (USHORT)Storage->Cylinders;
    Words[55] = (USHORT)Storage->Heads;
    Words[56] = (USHORT)Storage->Sectors;

    Words[57] = (USHORT)(Sectors & 0xFFFF);
    Words[58] = (USHORT)((Sectors >> 16) & 0xFFFF);

    /* How it may be addressed, which is the only way worth using */
    Words[60] = (USHORT)(Sectors & 0xFFFF);
    Words[61] = (USHORT)((Sectors >> 16) & 0xFFFF);

    Storage->BufferLength = SECTOR_SIZE;
    Storage->BufferOffset = 0;
    Storage->Writing = FALSE;
    Storage->Error = 0;
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE | STATUS_READY_DATA;

    StorageSetLine(Storage, TRUE);
}

static
VOID
StorageCommand(
    _Inout_ PSTORAGE_DEVICE Storage,
    _In_ UCHAR Command)
{
    const RTVM_HOST_INTERFACE *Host = Storage->Device.Host;

    /* A command aimed at the other drive on the cable is not this one's */
    if (!StorageSelected(Storage))
    {
        Storage->Status = 0;
        return;
    }

    StorageSetLine(Storage, FALSE);

    switch (Command)
    {
        case COMMAND_READ_SECTORS:
        case COMMAND_READ_NO_RETRY:
        case COMMAND_READ_MULTIPLE:
            StorageBeginRead(Storage);
            break;

        case COMMAND_WRITE_SECTORS:
        case COMMAND_WRITE_NO_RETRY:
        case COMMAND_WRITE_MULTIPLE:
            StorageBeginWrite(Storage);
            break;

        case COMMAND_IDENTIFY:
            StorageIdentify(Storage);
            break;

        case COMMAND_SET_PARAMETERS:
            /*
             * Being told the shape to use. It is taken, because a drive that
             * refuses leaves the caller believing it has no disk.
             */
            Storage->Heads = (Storage->Drive & DRIVE_HEAD_MASK) + 1;
            Storage->Sectors = (Storage->Count != 0) ? Storage->Count : GEOMETRY_SECTORS;
            Storage->Status = STATUS_READY | STATUS_SEEK_DONE;
            StorageSetLine(Storage, TRUE);
            break;

        case COMMAND_SET_MULTIPLE:
        case COMMAND_FLUSH:
        case COMMAND_FLUSH_EXT:
            if (Storage->Image != INVALID_HANDLE_VALUE)
                FlushFileBuffers(Storage->Image);

            Storage->Status = STATUS_READY | STATUS_SEEK_DONE;
            StorageSetLine(Storage, TRUE);
            break;

        default:
            Host->Log(Host->Context, RtvmLogTrace,
                      "%s: command %02x is not one this answers\n",
                      Storage->Device.Name, Command);
            StorageFail(Storage, ATA_ERROR_ABORTED);
            break;
    }
}

/* THE DEVICE *****************************************************************/

static
RTVM_STATUS
RTVMAPI
StorageStart(
    _In_ PRTVM_DEVICE Device)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    if (Storage->Image == INVALID_HANDLE_VALUE)
    {
        Host->Log(Host->Context, RtvmLogError,
                  "%s: there is no image to be a disk from\n", Device->Name);
        return RtvmFailed;
    }

    Status = Host->ClaimPortRange(Host->Context, Device,
                                  Storage->BasePort, ATA_REGISTER_COUNT);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, Storage->ControlPort, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: %llu sectors at %04x on line %lu, %lu/%lu/%lu, %s\n",
              Device->Name,
              Storage->SectorCount,
              Storage->BasePort,
              Storage->Line,
              Storage->Cylinders,
              Storage->Heads,
              Storage->Sectors,
              Storage->ImagePath);

    return RtvmOk;
}

static
VOID
RTVMAPI
StorageStop(
    _In_ PRTVM_DEVICE Device)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;

    if (Storage->Image != INVALID_HANDLE_VALUE)
        FlushFileBuffers(Storage->Image);
}

static
VOID
RTVMAPI
StorageReset(
    _In_ PRTVM_DEVICE Device)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;

    Storage->Features = 0;
    Storage->Count = 1;
    Storage->LbaLow = 1;
    Storage->LbaMid = 0;
    Storage->LbaHigh = 0;
    Storage->Drive = 0;
    Storage->Error = 0;
    Storage->Control = 0;
    Storage->BufferLength = 0;
    Storage->BufferOffset = 0;
    Storage->Remaining = 0;
    Storage->Writing = FALSE;

    /* Up, still, and with nothing to say */
    Storage->Status = STATUS_READY | STATUS_SEEK_DONE;

    StorageSetLine(Storage, FALSE);
}

static
VOID
RTVMAPI
StorageDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;

    if (Storage->Image != INVALID_HANDLE_VALUE)
        CloseHandle(Storage->Image);

    free(Storage);
}

static
RTVM_STATUS
RTVMAPI
StorageIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;
    ULONG Register;

    /* The one that sits apart, which says what the status is without taking it */
    if (Port == Storage->ControlPort)
    {
        *Value = StorageSelected(Storage) ? Storage->Status : 0;
        return RtvmOk;
    }

    Register = (ULONG)(Port - Storage->BasePort);

    if (Register >= ATA_REGISTER_COUNT)
        return RtvmNotClaimed;

    /* Everything but the data register reads as nothing on the other drive */
    if (!StorageSelected(Storage) && (Register != ATA_DRIVE))
    {
        *Value = 0;
        return RtvmOk;
    }

    switch (Register)
    {
        case ATA_DATA:
        {
            ULONG Taken = 0;
            ULONG Index;

            /*
             * Carried out of the buffer in whatever width was asked for. A
             * read past the end gives nothing rather than what came before.
             */
            for (Index = 0; Index < Width; Index++)
            {
                UCHAR Byte = 0;

                if (Storage->BufferOffset < Storage->BufferLength)
                    Byte = Storage->Buffer[Storage->BufferOffset++];

                Taken |= (ULONG)Byte << (Index * 8);
            }

            if (Storage->BufferOffset >= Storage->BufferLength)
            {
                /* The whole payload is across, so there is nothing more to give */
                Storage->BufferLength = 0;
                Storage->BufferOffset = 0;
                Storage->Status = STATUS_READY | STATUS_SEEK_DONE;
                StorageSetLine(Storage, FALSE);
            }

            *Value = Taken;
            return RtvmOk;
        }

        case ATA_ERROR:       *Value = Storage->Error; break;
        case ATA_SECTOR_COUNT:*Value = Storage->Count; break;
        case ATA_LBA_LOW:     *Value = Storage->LbaLow; break;
        case ATA_LBA_MID:     *Value = Storage->LbaMid; break;
        case ATA_LBA_HIGH:    *Value = Storage->LbaHigh; break;
        case ATA_DRIVE:       *Value = Storage->Drive | 0xA0; break;

        case ATA_STATUS:
            *Value = Storage->Status;

            /* Reading this is what says the interrupt has been seen */
            StorageSetLine(Storage, FALSE);
            break;

        default:
            *Value = 0;
            break;
    }

    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
StorageIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PSTORAGE_DEVICE Storage = (PSTORAGE_DEVICE)Device->DeviceContext;
    ULONG Register;

    if (Port == Storage->ControlPort)
    {
        UCHAR Previous = Storage->Control;

        Storage->Control = (UCHAR)(Value & 0xFF);

        /* Let go of the line if it has just been told to stay quiet */
        if (Storage->Control & CONTROL_NO_INTERRUPT)
            StorageSetLine(Storage, FALSE);

        /* Coming out of a reset puts everything back where it started */
        if ((Previous & CONTROL_RESET) && !(Storage->Control & CONTROL_RESET))
            StorageReset(Device);

        return RtvmOk;
    }

    Register = (ULONG)(Port - Storage->BasePort);

    if (Register >= ATA_REGISTER_COUNT)
        return RtvmNotClaimed;

    /* Which drive is being spoken to is the one thing both always listen for */
    if (Register == ATA_DRIVE)
    {
        Storage->Drive = (UCHAR)(Value & 0xFF);
        return RtvmOk;
    }

    if (!StorageSelected(Storage))
        return RtvmOk;

    switch (Register)
    {
        case ATA_DATA:
        {
            ULONG Index;

            for (Index = 0; Index < Width; Index++)
            {
                if (Storage->BufferOffset < sizeof(Storage->Buffer))
                {
                    Storage->Buffer[Storage->BufferOffset++] =
                        (UCHAR)((Value >> (Index * 8)) & 0xFF);
                }
            }

            /* Once the whole run is here it goes out to the image at once */
            if (Storage->Writing &&
                (Storage->BufferOffset >= Storage->BufferLength))
            {
                StorageFinishWrite(Storage);
            }
            break;
        }

        case ATA_FEATURES:    Storage->Features = (UCHAR)Value; break;
        case ATA_SECTOR_COUNT:Storage->Count = (UCHAR)Value; break;
        case ATA_LBA_LOW:     Storage->LbaLow = (UCHAR)Value; break;
        case ATA_LBA_MID:     Storage->LbaMid = (UCHAR)Value; break;
        case ATA_LBA_HIGH:    Storage->LbaHigh = (UCHAR)Value; break;

        case ATA_COMMAND:
            StorageCommand(Storage, (UCHAR)(Value & 0xFF));
            break;
    }

    return RtvmOk;
}

static const RTVM_DEVICE_VTABLE StorageVtable =
{
    sizeof(StorageVtable),
    StorageStart,
    StorageStop,
    StorageReset,
    StorageDestroy,
    StorageIoRead,
    StorageIoWrite,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

static
PCSTR
StorageSetting(
    _In_opt_ PCSTR Parameters,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Walk = Parameters;

    while ((Walk != NULL) && (*Walk != '\0'))
    {
        if ((_strnicmp(Walk, Name, Length) == 0) &&
            ((Walk[Length] == '=') || (Walk[Length] == ',') || (Walk[Length] == '\0')))
        {
            return (Walk[Length] == '=') ? &Walk[Length + 1] : &Walk[Length];
        }

        Walk = strchr(Walk, ',');
        if (Walk != NULL)
            Walk++;
    }

    return NULL;
}

static
VOID
StorageCopySetting(
    _In_ PCSTR Value,
    _Out_writes_z_(Size) PSTR Buffer,
    _In_ SIZE_T Size)
{
    SIZE_T Index = 0;

    while ((Index + 1 < Size) && (Value[Index] != '\0') && (Value[Index] != ','))
    {
        Buffer[Index] = Value[Index];
        Index++;
    }

    Buffer[Index] = '\0';
}

/**
 * @brief
 * Describes the image as a shape, since the old way of addressing needs one.
 *
 * @remarks
 * The numbers do not have to match anything real, only to multiply out to no
 * more than the image holds. Sixteen heads and sixty three sectors is what
 * everything has claimed for decades, so it is what anything reading this will
 * be expecting to see.
 */
static
VOID
StorageDescribe(
    _Inout_ PSTORAGE_DEVICE Storage)
{
    /*
     * A removable medium has a shape that is not negotiable: whatever wrote it
     * put the numbers in its own first sector and every boot sector on it
     * addresses by them. Guessing a different pair would send each read to the
     * wrong place, so the size is taken as saying which one it is.
     */
    if (Storage->SectorCount == FLOPPY_1440_SECTORS)
    {
        Storage->Heads = FLOPPY_1440_HEADS;
        Storage->Sectors = FLOPPY_1440_SECTORS_PER_TRACK;
    }
    else
    {
        Storage->Heads = GEOMETRY_HEADS;
        Storage->Sectors = GEOMETRY_SECTORS;
    }

    /* Told outright, for a medium that is neither */
    if (Storage->ForcedHeads != 0)
        Storage->Heads = Storage->ForcedHeads;

    if (Storage->ForcedSectors != 0)
        Storage->Sectors = Storage->ForcedSectors;

    Storage->Cylinders = (ULONG)(Storage->SectorCount /
                                 (Storage->Heads * Storage->Sectors));

    if (Storage->Cylinders == 0)
        Storage->Cylinders = 1;

    /* The field it goes in is sixteen bits, so it cannot say more than that */
    if (Storage->Cylinders > 65535)
        Storage->Cylinders = 65535;
}

static
RTVM_STATUS
RTVMAPI
StorageCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PSTORAGE_DEVICE Storage;
    LARGE_INTEGER Size;
    PCSTR Value;
    DWORD Access;

    Storage = (PSTORAGE_DEVICE)calloc(1, sizeof(*Storage));
    if (Storage == NULL)
        return RtvmNoMemory;

    Storage->Device.Size = sizeof(Storage->Device);
    Storage->Device.Vtable = &StorageVtable;
    Storage->Device.Host = Host;
    Storage->Device.DeviceContext = Storage;
    Storage->Image = INVALID_HANDLE_VALUE;

    if (StorageSetting(Parameters, "secondary") != NULL)
    {
        Storage->BasePort = ATA_SECONDARY_BASE;
        Storage->ControlPort = ATA_SECONDARY_CONTROL;
        Storage->Line = ATA_SECONDARY_LINE;
        StringCchCopyA(Storage->Device.Name, sizeof(Storage->Device.Name), "disk1");
    }
    else
    {
        Storage->BasePort = ATA_PRIMARY_BASE;
        Storage->ControlPort = ATA_PRIMARY_CONTROL;
        Storage->Line = ATA_PRIMARY_LINE;
        StringCchCopyA(Storage->Device.Name, sizeof(Storage->Device.Name), "disk0");
    }

    Storage->ReadOnly = (StorageSetting(Parameters, "readonly") != NULL);

    Value = StorageSetting(Parameters, "heads");
    if ((Value != NULL) && (*Value != '\0'))
        Storage->ForcedHeads = strtoul(Value, NULL, 0);

    Value = StorageSetting(Parameters, "sectors");
    if ((Value != NULL) && (*Value != '\0'))
        Storage->ForcedSectors = strtoul(Value, NULL, 0);

    Value = StorageSetting(Parameters, "image");
    if ((Value == NULL) || (*Value == '\0'))
    {
        Host->Log(Host->Context, RtvmLogError,
                  "a disk needs image=<path> to be a disk of anything\n");
        free(Storage);
        return RtvmBadParameter;
    }

    StorageCopySetting(Value, Storage->ImagePath, sizeof(Storage->ImagePath));

    Access = Storage->ReadOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);

    Storage->Image = CreateFileA(Storage->ImagePath,
                                 Access,
                                 FILE_SHARE_READ,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);

    /* A image that will not open for writing is still worth having read only */
    if ((Storage->Image == INVALID_HANDLE_VALUE) && !Storage->ReadOnly)
    {
        Storage->Image = CreateFileA(Storage->ImagePath,
                                     GENERIC_READ,
                                     FILE_SHARE_READ,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL,
                                     NULL);
        if (Storage->Image != INVALID_HANDLE_VALUE)
        {
            Storage->ReadOnly = TRUE;
            Host->Log(Host->Context, RtvmLogWarning,
                      "%s is only readable, so the disk cannot be written\n",
                      Storage->ImagePath);
        }
    }

    if (Storage->Image == INVALID_HANDLE_VALUE)
    {
        Host->Log(Host->Context, RtvmLogError,
                  "%s would not open, error %lu\n",
                  Storage->ImagePath, GetLastError());
        free(Storage);
        return RtvmFailed;
    }

    if (!GetFileSizeEx(Storage->Image, &Size) || (Size.QuadPart < SECTOR_SIZE))
    {
        Host->Log(Host->Context, RtvmLogError,
                  "%s is too small to be a disk\n", Storage->ImagePath);
        CloseHandle(Storage->Image);
        free(Storage);
        return RtvmFailed;
    }

    Storage->SectorCount = (ULONG64)Size.QuadPart / SECTOR_SIZE;
    Storage->Removable = (Storage->SectorCount == FLOPPY_1440_SECTORS);
    StorageDescribe(Storage);
    StorageReset(&Storage->Device);

    *Device = &Storage->Device;
    return RtvmOk;
}

/* WHAT THIS MODULE HAS *******************************************************/

static const RTVM_DEVICE_CLASS StorageClasses[] =
{
    {
        "disk",
        "Disk on the older wire, from an image file",
        StorageCreate
    }
};

static const RTVM_DEVICE_MODULE StorageModule =
{
    sizeof(StorageModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmstorage",
    "Disks",
    RTL_NUMBER_OF(StorageClasses),
    StorageClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &StorageModule;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/* EOF */
