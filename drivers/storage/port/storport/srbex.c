/*
 * PROJECT:     ReactOS Storport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Storport extended request block translation
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>


/*
 * The extended request trails the request reference in one allocation, so the
 * reference has to be a whole number of the extended request's alignment.
 */
C_ASSERT((sizeof(QUEUED_REQUEST_REFERENCE) % TYPE_ALIGNMENT(EXTENDED_REQUEST)) == 0);

/* A miniport walks to the address and the command block by offset */
C_ASSERT(FIELD_OFFSET(EXTENDED_REQUEST, Address) >= sizeof(STORAGE_REQUEST_BLOCK));
C_ASSERT(FIELD_OFFSET(EXTENDED_REQUEST, Cdb) >=
         FIELD_OFFSET(EXTENDED_REQUEST, Address) + sizeof(STOR_ADDR_BTL8));


/* FUNCTIONS ******************************************************************/

/**
 * @brief Fills in the address of a logical unit.
 *
 * Bus, target and LUN is the only addressing storport hands out, so every
 * caller that needs to name a unit to a miniport goes through here.
 */
VOID
StorpBuildUnitAddress(
    _In_ PPDO_DEVICE_EXTENSION PdoExtension,
    _Out_ PSTOR_ADDR_BTL8 Address)
{
    RtlZeroMemory(Address, sizeof(*Address));

    Address->Type = STOR_ADDRESS_TYPE_BTL8;
    Address->AddressLength = STOR_ADDR_BTL8_ADDRESS_LENGTH;
    Address->Path = (UCHAR)PdoExtension->Bus;
    Address->Target = (UCHAR)PdoExtension->Target;
    Address->Lun = (UCHAR)PdoExtension->Lun;
}


/**
 * @brief Tells whether a request block is in the extended format.
 *
 * The first four bytes mean the same thing in both formats, so the function
 * code alone is enough to tell them apart.
 */
BOOLEAN
StorpIsExtendedSrb(
    _In_ PVOID Srb)
{
    PSTORAGE_REQUEST_BLOCK_HEADER Header = (PSTORAGE_REQUEST_BLOCK_HEADER)Srb;

    return (Header->Function == SRB_FUNCTION_STORAGE_REQUEST_BLOCK);
}


/**
 * @brief Finds the reference a request was handed to a miniport under.
 *
 * The back pointer lives at a different offset in each of the two formats, so
 * every port routine that takes an SRB from a miniport comes through here.
 */
PQUEUED_REQUEST_REFERENCE
StorpRequestReference(
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    if (StorpIsExtendedSrb(Srb))
    {
        PSTORAGE_REQUEST_BLOCK Extended = (PSTORAGE_REQUEST_BLOCK)Srb;

        return (PQUEUED_REQUEST_REFERENCE)Extended->OriginalRequest;
    }

    return (PQUEUED_REQUEST_REFERENCE)Srb->OriginalRequest;
}


/**
 * @brief Finds the data buffer of a request in either format.
 */
PVOID
StorpSrbDataBuffer(
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    if (StorpIsExtendedSrb(Srb))
    {
        PSTORAGE_REQUEST_BLOCK Extended = (PSTORAGE_REQUEST_BLOCK)Srb;

        return Extended->DataBuffer;
    }

    return Srb->DataBuffer;
}


/**
 * @brief Fills in the extended request a miniport will be given.
 *
 * @param Srb The request as the class layer built it.
 * @param Request Storage for the translated request, already zeroed.
 *
 * Only the request functions we forward to a miniport are handled. Of those,
 * just SRB_FUNCTION_EXECUTE_SCSI carries a command, so the rest end up with no
 * extended data at all.
 */
VOID
StorpBuildExtendedSrb(
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _Out_ PEXTENDED_REQUEST Request)
{
    PSTORAGE_REQUEST_BLOCK Extended = &Request->Srb;

    Extended->Length = sizeof(STORAGE_REQUEST_BLOCK);
    Extended->Function = SRB_FUNCTION_STORAGE_REQUEST_BLOCK;
    Extended->SrbStatus = SRB_STATUS_PENDING;
    Extended->Signature = SRB_SIGNATURE;
    Extended->Version = STORAGE_REQUEST_BLOCK_VERSION_1;
    Extended->SrbLength = sizeof(EXTENDED_REQUEST);
    Extended->SrbFunction = Srb->Function;
    Extended->SrbFlags = Srb->SrbFlags;
    Extended->RequestTag = Srb->QueueTag;
    Extended->RequestAttribute = Srb->QueueAction;
    Extended->TimeOutValue = Srb->TimeOutValue;
    Extended->DataTransferLength = Srb->DataTransferLength;
    Extended->DataBuffer = Srb->DataBuffer;

    /* The miniport hands this straight back to us on completion */
    Extended->OriginalRequest = Srb->OriginalRequest;
    Extended->MiniportContext = Srb->SrbExtension;

    Extended->AddressOffset = FIELD_OFFSET(EXTENDED_REQUEST, Address);

    Request->Address.Type = STOR_ADDRESS_TYPE_BTL8;
    Request->Address.AddressLength = STOR_ADDR_BTL8_ADDRESS_LENGTH;
    Request->Address.Path = Srb->PathId;
    Request->Address.Target = Srb->TargetId;
    Request->Address.Lun = Srb->Lun;

    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI)
        return;

    Extended->NumSrbExData = 1;
    Extended->SrbExDataOffset[0] = FIELD_OFFSET(EXTENDED_REQUEST, Cdb);

    Request->Cdb.Type = SrbExDataTypeScsiCdb16;
    Request->Cdb.Length = SRBEX_DATA_SCSI_CDB16_LENGTH;
    Request->Cdb.ScsiStatus = Srb->ScsiStatus;
    Request->Cdb.SenseInfoBufferLength = Srb->SenseInfoBufferLength;
    Request->Cdb.CdbLength = Srb->CdbLength;
    Request->Cdb.SenseInfoBuffer = Srb->SenseInfoBuffer;

    RtlCopyMemory(Request->Cdb.Cdb, Srb->Cdb, sizeof(Request->Cdb.Cdb));
}


/**
 * @brief Carries the miniport's answer back to the original request block.
 */
VOID
StorpCompleteExtendedSrb(
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PEXTENDED_REQUEST Request)
{
    PSTORAGE_REQUEST_BLOCK Extended = &Request->Srb;

    Srb->SrbStatus = Extended->SrbStatus;
    Srb->DataTransferLength = Extended->DataTransferLength;

    if (Extended->NumSrbExData == 0)
        return;

    Srb->ScsiStatus = Request->Cdb.ScsiStatus;
    Srb->SenseInfoBufferLength = Request->Cdb.SenseInfoBufferLength;
}
