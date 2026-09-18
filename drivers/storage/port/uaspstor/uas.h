/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB Attached SCSI protocol definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _UAS_H_
#define _UAS_H_

/* A UAS device is a mass storage device speaking this interface protocol */
#define UAS_INTERFACE_PROTOCOL          0x62

/*
 * Each endpoint of the interface is followed by a descriptor saying what the
 * protocol uses it for, since the addresses themselves carry no meaning.
 */
#define UAS_PIPE_USAGE_DESCRIPTOR_TYPE  0x24

#define UAS_PIPE_ID_COMMAND             0x01
#define UAS_PIPE_ID_STATUS              0x02
#define UAS_PIPE_ID_DATA_IN             0x03
#define UAS_PIPE_ID_DATA_OUT            0x04

/* The first byte of every information unit says which one it is */
#define UAS_IU_COMMAND                  0x01
#define UAS_IU_SENSE                    0x03
#define UAS_IU_RESPONSE                 0x04
#define UAS_IU_TASK_MANAGEMENT          0x05
#define UAS_IU_READ_READY               0x06
#define UAS_IU_WRITE_READY              0x07

/* Task attributes, in the low three bits of the command IU attribute byte */
#define UAS_TASK_ATTRIBUTE_SIMPLE       0x00
#define UAS_TASK_ATTRIBUTE_HEAD_OF_QUEUE 0x01
#define UAS_TASK_ATTRIBUTE_ORDERED      0x02
#define UAS_TASK_ATTRIBUTE_ACA          0x04

/* Task management functions */
#define UAS_TMF_ABORT_TASK              0x01
#define UAS_TMF_ABORT_TASK_SET          0x02
#define UAS_TMF_CLEAR_TASK_SET          0x04
#define UAS_TMF_LOGICAL_UNIT_RESET      0x08
#define UAS_TMF_I_T_NEXUS_RESET         0x10
#define UAS_TMF_CLEAR_ACA               0x40
#define UAS_TMF_QUERY_TASK              0x80
#define UAS_TMF_QUERY_TASK_SET          0x81
#define UAS_TMF_QUERY_ASYNC_EVENT       0x82

/* What a response IU can say about a task management function */
#define UAS_RESPONSE_TMF_COMPLETE       0x00
#define UAS_RESPONSE_INVALID_IU         0x02
#define UAS_RESPONSE_TMF_NOT_SUPPORTED  0x04
#define UAS_RESPONSE_TMF_FAILED         0x05
#define UAS_RESPONSE_TMF_SUCCEEDED      0x08
#define UAS_RESPONSE_INCORRECT_LUN      0x09
#define UAS_RESPONSE_OVERLAPPED_TAG     0x0A

#include <pshpack1.h>

typedef struct _UAS_PIPE_USAGE_DESCRIPTOR
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bPipeId;
    UCHAR bReserved;
} UAS_PIPE_USAGE_DESCRIPTOR, *PUAS_PIPE_USAGE_DESCRIPTOR;

/* Every information unit starts the same way. Tags travel big endian. */
typedef struct _UAS_IU_HEADER
{
    UCHAR Id;
    UCHAR Reserved;
    UCHAR Tag[2];
} UAS_IU_HEADER, *PUAS_IU_HEADER;

typedef struct _UAS_COMMAND_IU
{
    UAS_IU_HEADER Header;
    UCHAR Attributes;
    UCHAR Reserved1;
    /* Command descriptor blocks longer than sixteen bytes, counted in dwords */
    UCHAR AdditionalCdbLength;
    UCHAR Reserved2;
    UCHAR Lun[8];
    UCHAR Cdb[16];
} UAS_COMMAND_IU, *PUAS_COMMAND_IU;

/*
 * The last word of a command. The sense data is only meaningful when the
 * status asks for it, and its length travels big endian like the tag.
 */
typedef struct _UAS_SENSE_IU
{
    UAS_IU_HEADER Header;
    UCHAR StatusQualifier[2];
    UCHAR Status;
    UCHAR Reserved[7];
    UCHAR Length[2];
    UCHAR SenseData[ANYSIZE_ARRAY];
} UAS_SENSE_IU, *PUAS_SENSE_IU;

/*
 * The draft this protocol shipped under put the status first and the sense
 * data eight bytes in. Devices built against it are still around, and the two
 * layouts are told apart by which length field matches the bytes received.
 */
typedef struct _UAS_SENSE_IU_DRAFT
{
    UAS_IU_HEADER Header;
    UCHAR Status;
    UCHAR ServiceResponse;
    UCHAR Length[2];
    UCHAR SenseData[ANYSIZE_ARRAY];
} UAS_SENSE_IU_DRAFT, *PUAS_SENSE_IU_DRAFT;

typedef struct _UAS_RESPONSE_IU
{
    UAS_IU_HEADER Header;
    UCHAR AdditionalInfo[3];
    UCHAR ResponseCode;
} UAS_RESPONSE_IU, *PUAS_RESPONSE_IU;

typedef struct _UAS_TASK_MANAGEMENT_IU
{
    UAS_IU_HEADER Header;
    UCHAR Function;
    UCHAR Reserved;
    UCHAR ManagedTag[2];
    UCHAR Lun[8];
} UAS_TASK_MANAGEMENT_IU, *PUAS_TASK_MANAGEMENT_IU;

#include <poppack.h>

#endif /* _UAS_H_ */
