/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synaptics RMI4 touch controller as a mouse class pointer port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <kbdmou.h>
#include <ntddmou.h>
#include <qcom/geniif.h>
#include <qcom/tlmmif.h>
#include <qcom/rpmhif.h>

#define QCTOUCH_POOL_TAG            'cTcQ'

/* RMI4 function numbers this driver drives */
#define RMI_FUNCTION_F01            0x01
#define RMI_FUNCTION_F12            0x12

/* Page select lives at the top of every 256 byte page */
#define RMI_PAGE_SELECT             0xFF
#define RMI_PAGE_SIZE               0x100
#define RMI_PAGE_MASK               0xFF00

/* Page descriptor table, scanned from the top down in six byte entries */
#define RMI_PDT_ENTRY_SIZE          6
#define RMI_PDT_SCAN_START          0xE9
#define RMI_PDT_SCAN_END            0x05
#define RMI_PDT_INT_SOURCE_MASK     0x07

/* F01 device control and status */
#define RMI_F01_CMD_RESET           0x01
#define RMI_F01_CTRL0_SLEEP_MASK    0x03
#define RMI_F01_CTRL0_NOSLEEP       0x04
#define RMI_F01_CTRL0_CONFIGURED    0x80
#define RMI_F01_STATUS_UNCONFIGURED 0x80

/* F12 object types reported in the first byte of each finger slot */
#define RMI_F12_OBJECT_FINGER       0x01
#define RMI_F12_OBJECT_GLOVED       0x06
#define RMI_F12_FINGER_BYTES        8

/* F12 register descriptors: general info, then query, control and data */
#define RMI_F12_QUERY_GENERAL       0
#define RMI_F12_QUERY_HAS_DESC      0x01
#define RMI_F12_CTRL_SENSOR_TUNING  8
#define RMI_F12_DATA_SENSED_OBJECTS 1

/* A parsed register descriptor keeps only what this driver reads */
#define RMI_REG_DESC_MAX_REGS       64

typedef struct _RMI_REG_ITEM
{
    ULONG Register;
    ULONG Size;
    ULONG SubpacketCount;
    BOOLEAN HasSubpacket0;
} RMI_REG_ITEM, *PRMI_REG_ITEM;

typedef struct _RMI_REG_DESC
{
    ULONG Count;
    RMI_REG_ITEM Items[RMI_REG_DESC_MAX_REGS];
} RMI_REG_DESC, *PRMI_REG_DESC;

/* A function picked up by the descriptor table scan */
typedef struct _RMI_FUNCTION
{
    BOOLEAN Found;
    USHORT QueryBase;
    USHORT CommandBase;
    USHORT ControlBase;
    USHORT DataBase;
    ULONG InterruptCount;
    ULONG InterruptPosition;
} RMI_FUNCTION, *PRMI_FUNCTION;

/*
 * TLMM tile bases. SDM845 splits the pin controller into tiles, each pin lives
 * at TileOffset + Number * 0x1000 inside the mapped window.
 */
#define QCTOUCH_TLMM_EAST   0x00100000
#define QCTOUCH_TLMM_NORTH  0x00500000
#define QCTOUCH_TLMM_SOUTH  0x00900000

/* The TLMM pins and the I2C wiring of one supported board */
typedef struct _QCTOUCH_BOARD
{
    PCSTR Manufacturer;
    PCSTR Product;
    ULONG EngineBase;
    ULONG ClockBase;
    ULONG ClockVoteOffset;
    ULONG ClockVoteMask;
    ULONG EngineBranchOffset;
    ULONG EngineRcgOffset;
    ULONG BusSpeedHz;
    USHORT SlaveAddress;
    ULONG TlmmBase;
    ULONG TlmmLength;
    ULONG PowerPin;
    ULONG PowerTile;
    ULONG InterruptPin;
    ULONG InterruptTile;
    ULONG ResetPin;
    ULONG ResetTile;
    ULONG SdaPin;
    ULONG SclPin;
    ULONG I2cTile;
    ULONG I2cFunction;
    ULONG StartupDelayMs;
    PCSTR VddResource;
    ULONG VddMicrovolts;
    ULONG VddMinMicrovolts;
    ULONG VddStepMicrovolts;
    ULONG VddMode;
} QCTOUCH_BOARD, *PQCTOUCH_BOARD;

typedef const QCTOUCH_BOARD *PCQCTOUCH_BOARD;

typedef struct _QCTOUCH_DEVICE
{
    PDEVICE_OBJECT DeviceObject;
    PCQCTOUCH_BOARD Board;

    PQCOM_GENI_I2C I2c;
    PQCOM_TLMM Tlmm;
    UCHAR Page;

    RMI_FUNCTION F01;
    RMI_FUNCTION F12;
    ULONG InterruptRegisters;

    /* F12 data packet layout worked out from the register descriptors */
    ULONG DataPacketSize;
    ULONG Data1Offset;
    ULONG FingerCount;
    ULONG MaxX;
    ULONG MaxY;

    /* mouclass connection and the reader thread */
    CONNECT_DATA ClassConnect;
    MOUSE_ATTRIBUTES Attributes;
    PKTHREAD Thread;
    KEVENT StopEvent;
    BOOLEAN FingerDown;
    LONG LastX;
    LONG LastY;
} QCTOUCH_DEVICE, *PQCTOUCH_DEVICE;

#define QCDBG_COMPONENT "qcomtouch"
#include <qcom/qcomdbg.h>

/* rmi4.c */
NTSTATUS
NTAPI
RmiReadBlock(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length);

NTSTATUS
NTAPI
RmiWriteBlock(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length);

NTSTATUS
NTAPI
RmiScanFunctions(
    _In_ PQCTOUCH_DEVICE Device);

NTSTATUS
NTAPI
RmiConfigure(
    _In_ PQCTOUCH_DEVICE Device);

BOOLEAN
NTAPI
RmiReadFinger(
    _In_ PQCTOUCH_DEVICE Device,
    _Out_ PULONG X,
    _Out_ PULONG Y,
    _Out_ PBOOLEAN Down);
