/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Image sections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

/* Image file data is described in 512 byte sectors */
#define MI_IMAGE_SECTOR_SHIFT       9
#define MI_IMAGE_SECTOR_SIZE        (1UL << MI_IMAGE_SECTOR_SHIFT)

/* Largest header area read to find the section headers */
#define MI_IMAGE_MAX_HEADER_SIZE    _64K

/* Largest image that can be mapped */
#define MI_IMAGE_MAX_SIZE           0x77000000UL

#define TAG_IMAGE_CONTROL_AREA      'aImM'
#define TAG_IMAGE_SEGMENT           'sImM'
#define TAG_IMAGE_PROTOTYPES        'pImM'
#define TAG_IMAGE_HEADER            'hImM'

/* What the headers say about where things go */
typedef struct _MI_IMAGE_LAYOUT
{
    PIMAGE_SECTION_HEADER SectionHeaders;
    ULONG NumberOfSections;
    ULONG SizeOfImage;
    ULONG SizeOfHeaders;
    ULONG SectionAlignment;
    ULONG FileAlignment;
    ULONG_PTR ImageBase;
    BOOLEAN Flat;
} MI_IMAGE_LAYOUT, *PMI_IMAGE_LAYOUT;

/* Protection of a section, indexed by its shared, execute, read and write bits */
static const UCHAR MiImageProtection[16] =
{
    MM_NOACCESS,            /* none */
    MM_NOACCESS,            /* shared */
    MM_EXECUTE,             /* execute */
    MM_EXECUTE,             /* execute, shared */
    MM_READONLY,            /* read */
    MM_READONLY,            /* read, shared */
    MM_EXECUTE_READ,        /* read, execute */
    MM_EXECUTE_READ,        /* read, execute, shared */
    MM_WRITECOPY,           /* write */
    MM_READWRITE,           /* write, shared */
    MM_EXECUTE_WRITECOPY,   /* write, execute */
    MM_EXECUTE_READWRITE,   /* write, execute, shared */
    MM_WRITECOPY,           /* write, read */
    MM_READWRITE,           /* write, read, shared */
    MM_EXECUTE_WRITECOPY,   /* write, read, execute */
    MM_EXECUTE_READWRITE    /* write, read, execute, shared */
};

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Reads the start of an image file into nonpaged pool.
 *
 * @param[in] File
 * Image file.
 *
 * @param[in] Length
 * Bytes wanted from the start of the file.
 *
 * @param[out] Buffer
 * Receives a page aligned buffer holding the data, zero filled past what was read.
 *
 * @param[out] ReadLength
 * Receives the number of bytes the file had.
 */
static
NTSTATUS
MiReadImageHeaders(
    _In_ PFILE_OBJECT File,
    _In_ ULONG Length,
    _Out_ PVOID *Buffer,
    _Out_ PULONG ReadLength)
{
    LARGE_INTEGER FileOffset;
    IO_STATUS_BLOCK IoStatus;
    ULONG BufferSize;
    NTSTATUS Status;
    KEVENT Event;
    KIRQL OldIrql;
    PVOID Data;
    PMDL Mdl;

    *Buffer = NULL;
    *ReadLength = 0;

    BufferSize = (ULONG)ROUND_TO_PAGES(Length);
    Data = ExAllocatePoolWithTag(NonPagedPool, BufferSize, TAG_IMAGE_HEADER);
    if (!Data)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Data, BufferSize);

    Mdl = IoAllocateMdl(Data, BufferSize, FALSE, FALSE, NULL);
    if (!Mdl)
    {
        ExFreePoolWithTag(Data, TAG_IMAGE_HEADER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MmBuildMdlForNonPagedPool(Mdl);
    Mdl->MdlFlags |= MDL_IO_PAGE_READ;

    FileOffset.QuadPart = 0;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    KeRaiseIrql(APC_LEVEL, &OldIrql);

    Status = IoPageRead(File, Mdl, &FileOffset, &Event, &IoStatus);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, WrPageIn, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
        MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);

    KeLowerIrql(OldIrql);
    IoFreeMdl(Mdl);

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, TAG_IMAGE_HEADER);
        return Status;
    }

    /* Whatever the paging read left over past the file keeps the zeroes */
    if (IoStatus.Information < BufferSize)
    {
        RtlZeroMemory((PUCHAR)Data + IoStatus.Information,
                      BufferSize - IoStatus.Information);
    }

    *Buffer = Data;
    *ReadLength = (ULONG)min(IoStatus.Information, Length);
    return STATUS_SUCCESS;
}

/**
 * @brief Tells whether a value is a power of two.
 */
static
BOOLEAN
MiIsPowerOfTwo(
    _In_ ULONG Value)
{
    return (BOOLEAN)((Value != 0) && !(Value & (Value - 1)));
}

/**
 * @brief Checks the headers of a PE image and captures what the section needs.
 *
 * @param[in] Headers
 * Start of the file.
 *
 * @param[in] HeadersLength
 * Valid bytes in Headers.
 *
 * @param[in] FileSize
 * Size of the image file.
 *
 * @param[out] Layout
 * Receives the layout of the image, pointing into Headers.
 *
 * @param[out] ImageInformation
 * Receives what the headers say about the image.
 *
 * @param[out] NeededLength
 * Receives how many bytes of the file are needed when Headers is too short.
 *
 * @return STATUS_BUFFER_TOO_SMALL when more of the file has to be read first.
 */
static
NTSTATUS
MiCaptureImageHeaders(
    _In_reads_bytes_(HeadersLength) PVOID Headers,
    _In_ ULONG HeadersLength,
    _In_ ULONG FileSize,
    _Out_ PMI_IMAGE_LAYOUT Layout,
    _Out_ PSECTION_IMAGE_INFORMATION ImageInformation,
    _Out_ PULONG NeededLength)
{
    PIMAGE_DOS_HEADER DosHeader = Headers;
    PIMAGE_NT_HEADERS32 NtHeaders;
    PIMAGE_OPTIONAL_HEADER32 Optional32;
    PIMAGE_DATA_DIRECTORY ComDirectory = NULL;
    ULONG NtOffset, NtEnd, SectionsOffset, SectionsEnd;
    ULONG AddressOfEntryPoint, SizeOfCode;
    USHORT Machine;
#ifdef _WIN64
    PIMAGE_OPTIONAL_HEADER64 Optional64;
#endif

    *NeededLength = 0;
    RtlZeroMemory(Layout, sizeof(*Layout));
    RtlZeroMemory(ImageInformation, sizeof(*ImageInformation));

    if ((HeadersLength < sizeof(IMAGE_DOS_HEADER)) || (DosHeader->e_magic != IMAGE_DOS_SIGNATURE))
        return STATUS_INVALID_IMAGE_NOT_MZ;

    /* The NT headers are looked at with the larger 64 bit layout */
    NtOffset = (ULONG)DosHeader->e_lfanew;
    NtEnd = NtOffset + sizeof(IMAGE_NT_HEADERS64);
    if ((NtEnd <= NtOffset) || (NtEnd > FileSize) || (NtOffset & (sizeof(ULONG) - 1)))
        return STATUS_INVALID_IMAGE_PROTECT;

    if (NtEnd > HeadersLength)
    {
        if (NtEnd > MI_IMAGE_MAX_HEADER_SIZE)
            return STATUS_INVALID_IMAGE_FORMAT;

        *NeededLength = NtEnd;
        return STATUS_BUFFER_TOO_SMALL;
    }

    NtHeaders = (PIMAGE_NT_HEADERS32)((PUCHAR)Headers + NtOffset);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_PROTECT;

    Machine = NtHeaders->FileHeader.Machine;
    if (!Machine && !NtHeaders->FileHeader.SizeOfOptionalHeader)
        return STATUS_INVALID_IMAGE_PROTECT;

    if (!(NtHeaders->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Fields are read where they belong, whatever the optional header size says */
    Optional32 = &NtHeaders->OptionalHeader;
    switch (Optional32->Magic)
    {
        case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        {
            if ((Machine != IMAGE_FILE_MACHINE_I386) && (Machine != IMAGE_FILE_MACHINE_ARMV7))
                return STATUS_INVALID_IMAGE_FORMAT;

            Layout->ImageBase = Optional32->ImageBase;
            Layout->SectionAlignment = Optional32->SectionAlignment;
            Layout->FileAlignment = Optional32->FileAlignment;
            Layout->SizeOfImage = Optional32->SizeOfImage;
            Layout->SizeOfHeaders = Optional32->SizeOfHeaders;

            AddressOfEntryPoint = Optional32->AddressOfEntryPoint;
            SizeOfCode = Optional32->SizeOfCode;

            ImageInformation->MaximumStackSize = Optional32->SizeOfStackReserve;
            ImageInformation->CommittedStackSize = Optional32->SizeOfStackCommit;
            ImageInformation->SubSystemType = Optional32->Subsystem;
            ImageInformation->SubSystemMinorVersion = Optional32->MinorSubsystemVersion;
            ImageInformation->SubSystemMajorVersion = Optional32->MajorSubsystemVersion;
            ImageInformation->LoaderFlags = Optional32->LoaderFlags;

            /* There is no side by side support, keep the loader away from manifests */
            ImageInformation->DllCharacteristics = Optional32->DllCharacteristics |
                                                   IMAGE_DLLCHARACTERISTICS_NO_ISOLATION;

            if (Optional32->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
                ComDirectory = &Optional32->DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
            break;
        }

        case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        {
#ifdef _WIN64
            if (Machine != IMAGE_FILE_MACHINE_AMD64)
                return STATUS_INVALID_IMAGE_FORMAT;

            Optional64 = (PIMAGE_OPTIONAL_HEADER64)Optional32;

            Layout->ImageBase = (ULONG_PTR)Optional64->ImageBase;
            Layout->SectionAlignment = Optional64->SectionAlignment;
            Layout->FileAlignment = Optional64->FileAlignment;
            Layout->SizeOfImage = Optional64->SizeOfImage;
            Layout->SizeOfHeaders = Optional64->SizeOfHeaders;

            AddressOfEntryPoint = Optional64->AddressOfEntryPoint;
            SizeOfCode = Optional64->SizeOfCode;

            ImageInformation->MaximumStackSize = (SIZE_T)Optional64->SizeOfStackReserve;
            ImageInformation->CommittedStackSize = (SIZE_T)Optional64->SizeOfStackCommit;
            ImageInformation->SubSystemType = Optional64->Subsystem;
            ImageInformation->SubSystemMinorVersion = Optional64->MinorSubsystemVersion;
            ImageInformation->SubSystemMajorVersion = Optional64->MajorSubsystemVersion;
            ImageInformation->LoaderFlags = Optional64->LoaderFlags;
            ImageInformation->DllCharacteristics = Optional64->DllCharacteristics;

            if (Optional64->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
                ComDirectory = &Optional64->DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
            break;
#else
            return STATUS_INVALID_IMAGE_WIN_64;
#endif
        }

        default:
            return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* Raw data is described in sectors unless the file is laid out like memory */
    if ((Layout->FileAlignment == 0) ||
        ((Layout->FileAlignment & (MI_IMAGE_SECTOR_SIZE - 1)) &&
         (Layout->FileAlignment != Layout->SectionAlignment)) ||
        !MiIsPowerOfTwo(Layout->SectionAlignment) ||
        !MiIsPowerOfTwo(Layout->FileAlignment) ||
        (Layout->SectionAlignment < Layout->FileAlignment) ||
        (Layout->SizeOfImage > MI_IMAGE_MAX_SIZE) ||
        (Layout->SizeOfHeaders >= Layout->SizeOfImage) ||
        (NtHeaders->FileHeader.SizeOfOptionalHeader & (sizeof(ULONG_PTR) - 1)) ||
        (Layout->ImageBase & (_64K - 1)))
    {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    if ((NtHeaders->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) &&
        (ImageInformation->DllCharacteristics & IMAGE_DLLCHARACTERISTICS_APPCONTAINER))
    {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    SectionsOffset = NtOffset + FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader) +
                     NtHeaders->FileHeader.SizeOfOptionalHeader;
    SectionsEnd = SectionsOffset + NtHeaders->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (SectionsEnd <= NtOffset)
        return STATUS_INVALID_IMAGE_FORMAT;

    if (SectionsEnd > HeadersLength)
    {
        if ((SectionsEnd > MI_IMAGE_MAX_HEADER_SIZE) || (SectionsEnd > FileSize))
            return STATUS_INVALID_IMAGE_FORMAT;

        *NeededLength = SectionsEnd;
        return STATUS_BUFFER_TOO_SMALL;
    }

    Layout->SectionHeaders = (PIMAGE_SECTION_HEADER)((PUCHAR)Headers + SectionsOffset);
    Layout->NumberOfSections = NtHeaders->FileHeader.NumberOfSections;
    Layout->Flat = (BOOLEAN)(Layout->SectionAlignment < PAGE_SIZE);

    ImageInformation->TransferAddress = (PVOID)(Layout->ImageBase + AddressOfEntryPoint);
    ImageInformation->ImageContainsCode = (BOOLEAN)((SizeOfCode != 0) || (AddressOfEntryPoint != 0));
    ImageInformation->ImageCharacteristics = NtHeaders->FileHeader.Characteristics;
    ImageInformation->Machine = Machine;
    ImageInformation->ImageFileSize = FileSize;

    if (ComDirectory && ComDirectory->VirtualAddress && ComDirectory->Size)
        ImageInformation->LoaderFlags |= 1;

    return STATUS_SUCCESS;
}

/**
 * @brief Sets up an image subsection and the file data it describes.
 *
 * @param[in] ControlArea
 * Image control area.
 *
 * @param[in] Subsection
 * Subsection to set up.
 *
 * @param[in] PointerPte
 * First prototype PTE of the subsection.
 *
 * @param[in] PteCount
 * Number of prototype PTEs.
 *
 * @param[in] Protection
 * Protection of the pages.
 *
 * @param[in] RawOffset
 * File offset of the data.
 *
 * @param[in] RawEnd
 * File offset where the data ends.
 */
static
VOID
MiInitializeImageSubsection(
    _In_ PCONTROL_AREA ControlArea,
    _Out_ PSUBSECTION Subsection,
    _In_ PMMPTE PointerPte,
    _In_ ULONG PteCount,
    _In_ ULONG Protection,
    _In_ ULONG RawOffset,
    _In_ ULONG RawEnd)
{
    Subsection->ControlArea = ControlArea;
    Subsection->SubsectionBase = PointerPte;
    Subsection->PtesInSubsection = PteCount;
    Subsection->u.SubsectionFlags.Protection = Protection;
    Subsection->u.SubsectionFlags.ReadOnly = !(Protection & MM_READWRITE);
    Subsection->u.SubsectionFlags.ReadWrite = !!(Protection & MM_READWRITE);
    Subsection->u.SubsectionFlags.GlobalMemory = ((Protection & MM_WRITECOPY) == MM_READWRITE);
    Subsection->StartingSector = RawOffset >> MI_IMAGE_SECTOR_SHIFT;
    Subsection->NumberOfFullSectors = (RawEnd >> MI_IMAGE_SECTOR_SHIFT) - Subsection->StartingSector;
    Subsection->u.SubsectionFlags.SectorEndOffset = RawEnd & (MI_IMAGE_SECTOR_SIZE - 1);
}

/**
 * @brief Describes an image with sections below a page, mapped as the file is.
 *
 * @param[in] ControlArea
 * Image control area with one subsection.
 *
 * @param[in] Layout
 * Layout of the image.
 *
 * @param[in] FileSize
 * Size of the image file.
 */
static
NTSTATUS
MiBuildFlatImage(
    _In_ PCONTROL_AREA ControlArea,
    _In_ PMI_IMAGE_LAYOUT Layout,
    _In_ ULONG FileSize)
{
    PSEGMENT Segment = ControlArea->Segment;
    PSUBSECTION Subsection = (PSUBSECTION)(ControlArea + 1);
    PIMAGE_SECTION_HEADER Section;
    MMPTE SubsectionPte, ZeroPte;
    ULONG i, Size;

    for (i = 0; i < Layout->NumberOfSections; i++)
    {
        Section = &Layout->SectionHeaders[i];
        Size = Section->Misc.VirtualSize ? Section->Misc.VirtualSize : Section->SizeOfRawData;

        if ((Section->PointerToRawData + Section->SizeOfRawData < Section->PointerToRawData) ||
            (Section->PointerToRawData != Section->VirtualAddress) ||
            (Size > Section->SizeOfRawData))
        {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
    }

    MiInitializeImageSubsection(ControlArea,
                                Subsection,
                                Segment->PrototypePte,
                                Segment->TotalNumberOfPtes,
                                MM_EXECUTE_WRITECOPY,
                                0,
                                FileSize);

    MI_MAKE_SUBSECTION_PTE(&SubsectionPte, Subsection);
    SubsectionPte.u.Subsect.Protection = MM_EXECUTE_WRITECOPY;
    MI_MAKE_SOFTWARE_PTE(&ZeroPte, MM_EXECUTE_WRITECOPY);

    for (i = 0; i < Segment->TotalNumberOfPtes; i++)
        Segment->PrototypePte[i] = ((i << PAGE_SHIFT) < FileSize) ? SubsectionPte : ZeroPte;

    return STATUS_SUCCESS;
}

/**
 * @brief Describes the headers and every section of an image with its own subsection.
 *
 * @param[in] ControlArea
 * Image control area with a subsection for the headers and one per section.
 *
 * @param[in] Layout
 * Layout of the image.
 *
 * @param[in] FileSize
 * Size of the image file.
 */
static
NTSTATUS
MiBuildSectionedImage(
    _In_ PCONTROL_AREA ControlArea,
    _In_ PMI_IMAGE_LAYOUT Layout,
    _In_ ULONG FileSize)
{
    PSEGMENT Segment = ControlArea->Segment;
    PSUBSECTION Subsection = (PSUBSECTION)(ControlArea + 1);
    PMMPTE PointerPte = Segment->PrototypePte;
    ULONG Remaining = Segment->TotalNumberOfPtes;
    ULONG SectionAlignment = Layout->SectionAlignment;
    ULONG FileAlignment = Layout->FileAlignment;
    PIMAGE_SECTION_HEADER Section;
    ULONG i, j, Size, End, PteCount, Protection;
    ULONG RawOffset, RawSize, RawEnd, VirtualEnd;
    ULONG64 LastRawEnd;
    MMPTE SubsectionPte, ZeroPte;

    /* Headers first, bytes past SizeOfHeaders read as zeroes */
    End = Layout->SizeOfHeaders + SectionAlignment - 1;
    if ((Layout->SizeOfHeaders == 0) || (End <= Layout->SizeOfHeaders))
        return STATUS_INVALID_IMAGE_FORMAT;

    PteCount = ALIGN_DOWN_BY(End, SectionAlignment) >> PAGE_SHIFT;
    if (PteCount > Remaining)
        return STATUS_INVALID_IMAGE_FORMAT;

    MiInitializeImageSubsection(ControlArea,
                                Subsection,
                                PointerPte,
                                PteCount,
                                MM_READONLY,
                                0,
                                Layout->SizeOfHeaders);

    MI_MAKE_SUBSECTION_PTE(&SubsectionPte, Subsection);
    SubsectionPte.u.Subsect.Protection = MM_READONLY;

    for (j = 0; j < PteCount; j++, PointerPte++)
        PointerPte->u.Long = ((j << PAGE_SHIFT) < Layout->SizeOfHeaders) ? SubsectionPte.u.Long : 0;

    Remaining -= PteCount;
    VirtualEnd = PteCount << PAGE_SHIFT;

    /* Only the data of the last section has to be in the file, no section at all is invalid */
    LastRawEnd = (ULONG64)FileSize + 1;

    for (i = 0; i < Layout->NumberOfSections; i++)
    {
        Section = &Layout->SectionHeaders[i];
        Subsection->NextSubsection = Subsection + 1;
        Subsection++;

        Size = Section->Misc.VirtualSize ? Section->Misc.VirtualSize : Section->SizeOfRawData;
        RawSize = Section->SizeOfRawData;
        RawOffset = RawSize ? Section->PointerToRawData : 0;

        if (RawOffset + RawSize < RawOffset)
            return STATUS_INVALID_IMAGE_FORMAT;

        /* Sections follow each other in memory without holes */
        if ((Section->VirtualAddress != VirtualEnd) || (Size == 0))
            return STATUS_INVALID_IMAGE_FORMAT;

        End = Size + SectionAlignment - 1;
        if (End <= Size)
            return STATUS_INVALID_IMAGE_FORMAT;

        PteCount = ALIGN_DOWN_BY(End, SectionAlignment) >> PAGE_SHIFT;
        if (PteCount > Remaining)
            return STATUS_INVALID_IMAGE_FORMAT;

        /* The page holding the end of the data is read up to the file alignment */
        RawEnd = ALIGN_DOWN_BY(RawOffset + FileAlignment + RawSize - 1, FileAlignment);
        if (RawEnd < RawOffset)
            return STATUS_INVALID_IMAGE_FORMAT;

        Protection = MiImageProtection[Section->Characteristics >> 28];

        MiInitializeImageSubsection(ControlArea,
                                    Subsection,
                                    PointerPte,
                                    PteCount,
                                    Protection,
                                    RawOffset,
                                    RawEnd);

        MI_MAKE_SOFTWARE_PTE(&ZeroPte, Protection);
        if (RawOffset)
        {
            MI_MAKE_SUBSECTION_PTE(&SubsectionPte, Subsection);
            SubsectionPte.u.Subsect.Protection = Protection;
        }
        else
        {
            SubsectionPte = ZeroPte;
        }

        /* Pages past the virtual size are not part of the image */
        for (j = 0; j < PteCount; j++, PointerPte++)
        {
            if ((j << PAGE_SHIFT) >= Size)
                PointerPte->u.Long = 0;
            else if ((j << PAGE_SHIFT) < RawSize)
                *PointerPte = SubsectionPte;
            else
                *PointerPte = ZeroPte;
        }

        LastRawEnd = (ULONG64)RawOffset + RawSize;
        Remaining -= PteCount;
        VirtualEnd += PteCount << PAGE_SHIFT;
    }

    if ((LastRawEnd > FileSize) || (Remaining >= (SectionAlignment >> PAGE_SHIFT)))
        return STATUS_INVALID_IMAGE_FORMAT;

    /* What SizeOfImage has past the last section is not part of the image either */
    RtlZeroMemory(PointerPte, Remaining * sizeof(MMPTE));
    return STATUS_SUCCESS;
}

/**
 * @brief Builds the control area describing an image file.
 *
 * @param[in] File
 * Image file.
 *
 * @param[out] OutControlArea
 * Receives the control area, not yet known to the file.
 */
static
NTSTATUS
MiCreateImageFileMap(
    _In_ PFILE_OBJECT File,
    _Out_ PCONTROL_AREA *OutControlArea)
{
    SECTION_IMAGE_INFORMATION ImageInformation;
    ULONG SubsectionCount, PteCount, Length, ReadLength, Needed;
    PCONTROL_AREA ControlArea;
    LARGE_INTEGER FileSize;
    MI_IMAGE_LAYOUT Layout;
    PSEGMENT Segment;
    PVOID Headers;
    NTSTATUS Status;

    *OutControlArea = NULL;

    Status = FsRtlGetFileSize(File, &FileSize);
    if (!NT_SUCCESS(Status))
        return Status;

    if (FileSize.HighPart != 0)
        return STATUS_INVALID_FILE_FOR_SECTION;

    /* One page holds the headers of most images, read more when it does not */
    Length = PAGE_SIZE;
    for (;;)
    {
        Status = MiReadImageHeaders(File, Length, &Headers, &ReadLength);
        if (!NT_SUCCESS(Status))
            return (Status == STATUS_FILE_LOCK_CONFLICT) ? Status : STATUS_INVALID_FILE_FOR_SECTION;

        Status = MiCaptureImageHeaders(Headers,
                                       ReadLength,
                                       FileSize.LowPart,
                                       &Layout,
                                       &ImageInformation,
                                       &Needed);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            break;

        ExFreePoolWithTag(Headers, TAG_IMAGE_HEADER);

        /* A file ending inside its own headers is not an image */
        if ((ReadLength < Length) || (Needed <= ReadLength))
            return STATUS_INVALID_IMAGE_FORMAT;

        Length = Needed;
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Headers, TAG_IMAGE_HEADER);
        return Status;
    }

    PteCount = (ULONG)BYTES_TO_PAGES(Layout.SizeOfImage);
    SubsectionCount = Layout.Flat ? 1 : Layout.NumberOfSections + 1;

    ControlArea = ExAllocatePoolWithTag(NonPagedPool,
                                        sizeof(CONTROL_AREA) + SubsectionCount * sizeof(SUBSECTION),
                                        TAG_IMAGE_CONTROL_AREA);
    Segment = ExAllocatePoolWithTag(NonPagedPool,
                                    sizeof(SEGMENT) + sizeof(SECTION_IMAGE_INFORMATION),
                                    TAG_IMAGE_SEGMENT);
    if (!ControlArea || !Segment)
    {
        if (ControlArea)
            ExFreePoolWithTag(ControlArea, TAG_IMAGE_CONTROL_AREA);
        if (Segment)
            ExFreePoolWithTag(Segment, TAG_IMAGE_SEGMENT);
        ExFreePoolWithTag(Headers, TAG_IMAGE_HEADER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ControlArea, sizeof(CONTROL_AREA) + SubsectionCount * sizeof(SUBSECTION));
    RtlZeroMemory(Segment, sizeof(SEGMENT));
    ControlArea->Segment = Segment;

    Segment->PrototypePte = ExAllocatePoolWithTag(PagedPool,
                                                  PteCount * sizeof(MMPTE),
                                                  TAG_IMAGE_PROTOTYPES);
    if (!Segment->PrototypePte)
    {
        MiFreeImageFileMap(ControlArea);
        ExFreePoolWithTag(Headers, TAG_IMAGE_HEADER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Segment->ControlArea = ControlArea;
    Segment->TotalNumberOfPtes = PteCount;
    Segment->NonExtendedPtes = PteCount;
    Segment->SizeOfSegment = (ULONG64)PteCount << PAGE_SHIFT;
    Segment->SegmentPteTemplate.u.Soft.Protection = MM_EXECUTE_WRITECOPY;
    Segment->BasedAddress = (PVOID)Layout.ImageBase;
    Segment->u2.ImageInformation = (PSECTION_IMAGE_INFORMATION)(Segment + 1);
    *Segment->u2.ImageInformation = ImageInformation;

    ControlArea->FilePointer = File;
    ControlArea->u.Flags.Image = 1;
    ControlArea->u.Flags.File = 1;

    if (Layout.Flat)
        Status = MiBuildFlatImage(ControlArea, &Layout, FileSize.LowPart);
    else
        Status = MiBuildSectionedImage(ControlArea, &Layout, FileSize.LowPart);

    ExFreePoolWithTag(Headers, TAG_IMAGE_HEADER);

    if (!NT_SUCCESS(Status))
    {
        MiFreeImageFileMap(ControlArea);
        return Status;
    }

    *OutControlArea = ControlArea;
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Frees an image control area and everything hanging off it.
 * @remarks The file object reference is left alone.
 */
VOID
NTAPI
MiFreeImageFileMap(
    _In_ PCONTROL_AREA ControlArea)
{
    PSEGMENT Segment = ControlArea->Segment;

    if (Segment)
    {
        if (Segment->PrototypePte)
            ExFreePoolWithTag(Segment->PrototypePte, TAG_IMAGE_PROTOTYPES);

        ExFreePoolWithTag(Segment, TAG_IMAGE_SEGMENT);
    }

    ExFreePoolWithTag(ControlArea, TAG_IMAGE_CONTROL_AREA);
}

/**
 * @brief Gets the image control area of a file, creating it when there is none.
 *
 * @param[in] File
 * Image file.
 *
 * @param[out] OutControlArea
 * Receives the control area with one more section and user reference.
 */
NTSTATUS
NTAPI
MiReferenceImageFileMap(
    _In_ PFILE_OBJECT File,
    _Out_ PCONTROL_AREA *OutControlArea)
{
    PSECTION_OBJECT_POINTERS SectionPointers = File->SectionObjectPointer;
    PCONTROL_AREA ControlArea, NewControlArea = NULL;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KIRQL OldIrql;

    for (;;)
    {
        OldIrql = MiAcquirePfnLock();

        ControlArea = SectionPointers->ImageSectionObject;
        if (ControlArea)
        {
            ASSERT(ControlArea->u.Flags.Image == 1);
            ASSERT(ControlArea->u.Flags.BeingDeleted == 0);

            /* Take it back from the cleanup list */
            if (ControlArea->DereferenceList.Flink)
            {
                RemoveEntryList(&ControlArea->DereferenceList);
                ControlArea->DereferenceList.Flink = NULL;
            }

            ControlArea->NumberOfSectionReferences++;
            ControlArea->NumberOfUserReferences++;
            MiReleasePfnLock(OldIrql);

            if (NewControlArea)
                MiFreeImageFileMap(NewControlArea);

            *OutControlArea = ControlArea;
            return STATUS_SUCCESS;
        }

        if (NewControlArea)
        {
            ObReferenceObject(File);
            NewControlArea->NumberOfSectionReferences = 1;
            NewControlArea->NumberOfUserReferences = 1;
            SectionPointers->ImageSectionObject = NewControlArea;
            MiReleasePfnLock(OldIrql);

            *OutControlArea = NewControlArea;
            return STATUS_SUCCESS;
        }

        MiReleasePfnLock(OldIrql);

        /* Data written through the cache or a mapping has to be in the file before reading it */
        if (SectionPointers->DataSectionObject || SectionPointers->SharedCacheMap)
            CcFlushCache(SectionPointers, NULL, 0, &IoStatus);

        Status = MiCreateImageFileMap(File, &NewControlArea);
        if (!NT_SUCCESS(Status))
            return Status;
    }
}

/**
 * @brief Gets the protection a prototype PTE of an image got from its section.
 *
 * @param[in] ControlArea
 * Image control area.
 *
 * @param[in] PointerProtoPte
 * Prototype PTE of the image.
 */
ULONG
NTAPI
MiGetImageProtoPteProtection(
    _In_ PCONTROL_AREA ControlArea,
    _In_ PMMPTE PointerProtoPte)
{
    PSUBSECTION Subsection;

    ASSERT(ControlArea->u.Flags.Image == 1);

    for (Subsection = (PSUBSECTION)(ControlArea + 1);
         Subsection != NULL;
         Subsection = Subsection->NextSubsection)
    {
        if ((PointerProtoPte >= Subsection->SubsectionBase) &&
            (PointerProtoPte < &Subsection->SubsectionBase[Subsection->PtesInSubsection]))
        {
            return Subsection->u.SubsectionFlags.Protection;
        }
    }

    /* Pages after the last section */
    return MM_NOACCESS;
}

/**
 * @brief Gets where the data of an image page is in its file.
 *
 * @param[in] Subsection
 * Image subsection holding the page.
 *
 * @param[in] PointerProtoPte
 * Prototype PTE of the page.
 *
 * @param[out] FileOffset
 * Receives the file offset of the page.
 *
 * @return Bytes of file data in the page, the rest reads as zeroes.
 */
ULONG
NTAPI
MiGetImagePageFileOffset(
    _In_ PSUBSECTION Subsection,
    _In_ PMMPTE PointerProtoPte,
    _Out_ PLARGE_INTEGER FileOffset)
{
    ULONG64 RawSize, PageOffset;

    ASSERT(Subsection->ControlArea->u.Flags.Image == 1);

    PageOffset = (ULONG64)(PointerProtoPte - Subsection->SubsectionBase) << PAGE_SHIFT;
    RawSize = ((ULONG64)Subsection->NumberOfFullSectors << MI_IMAGE_SECTOR_SHIFT) +
              Subsection->u.SubsectionFlags.SectorEndOffset;

    FileOffset->QuadPart = ((LONGLONG)Subsection->StartingSector << MI_IMAGE_SECTOR_SHIFT) + PageOffset;

    if (PageOffset >= RawSize)
        return 0;

    return (ULONG)min(RawSize - PageOffset, PAGE_SIZE);
}

/**
 * @brief Throws away the image section of a file if nothing uses it.
 *
 * @param[in] SectionObjectPointer
 * Section pointers of the file.
 *
 * @param[in] FlushType
 * MmFlushForDelete also refuses when user sections map the file data.
 *
 * @return TRUE if the file has no image section left.
 */
BOOLEAN
NTAPI
MmFlushImageSection(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_ MMFLUSH_TYPE FlushType)
{
    PCONTROL_AREA ControlArea;
    LARGE_INTEGER Delay;
    KIRQL OldIrql;

    Delay.QuadPart = -10 * 10000LL;

    for (;;)
    {
        OldIrql = MiAcquirePfnLock();

        /* User sections and views of the data keep the file from going away */
        ControlArea = SectionObjectPointer->DataSectionObject;
        if ((FlushType == MmFlushForDelete) && ControlArea && (ControlArea->NumberOfUserReferences != 0))
        {
            MiReleasePfnLock(OldIrql);
            return FALSE;
        }

        ControlArea = SectionObjectPointer->ImageSectionObject;
        if (!ControlArea)
        {
            MiReleasePfnLock(OldIrql);
            return TRUE;
        }

        if ((ControlArea->NumberOfSectionReferences != 0) ||
            (ControlArea->NumberOfMappedViews != 0) ||
            (ControlArea->FlushInProgressCount != 0))
        {
            MiReleasePfnLock(OldIrql);
            return FALSE;
        }

        /* The cleanup thread is deleting it already */
        if (ControlArea->u.Flags.BeingPurged)
        {
            MiReleasePfnLock(OldIrql);
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            continue;
        }

        MiReferenceDataFileMapForIoUnsafe(ControlArea);
        MiReleasePfnLock(OldIrql);

        /* The last reference going away deletes it right here */
        MiDereferenceDataFileMapForIo(ControlArea);
    }
}

/* EOF */
