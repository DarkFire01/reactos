/*
 * PROJECT:     ReactOS Intel I21X Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hardware specific definitions
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmai.com>
 */
#pragma once

#define IEEE_802_ADDR_LENGTH 6

#define MAX_RESET_ATTEMPTS 10
#define MAX_EEPROM_READ_ATTEMPTS 10000

#define MAXIMUM_MULTICAST_ADDRESSES 16

/* Ethernet frame header */
typedef struct _ETH_HEADER
{
    UCHAR Destination[IEEE_802_ADDR_LENGTH];
    UCHAR Source[IEEE_802_ADDR_LENGTH];
    USHORT PayloadType;
} ETH_HEADER, *PETH_HEADER;

/* registers - these are all dwords */
#define  I211_REG_IOADDR 0x0000
#define  I211_REG_IODATA 0x0004
/* 0x0008 - 0x001F are reserved */

#define I211_REG_CTRL 0x0000
#define I211_REG_STATUS 0x0008
#define I211_REG_CTRL_EXT 0x0018
#define I211_REG_MDIC 0x0020
#define I211_REG_FCAL 0x0028
#define I211_REG_FCAH 0x002C
#define I211_REG_FCT 0x0030
#define I211_REG_CONNSW 0x0034
#define I211_REG_VET 0x0038
#define I211_REG_MDICNFG 0x0E04
#define I211_REG_FCTTV 0x0170
#define I211_REG_LEDCTL 0x0E00
#define I211_REG_I2CCMD 0x1028
#define I211_REG_I2CPARAMS 0x102C
#define I211_REG_WDSTP 0x1040
#define I211_REG_WDSWSTS 0x1044
#define I211_REG_FRTIMER 0x1048
#define I211_REG_TCP 0x104C
#define I211_REG_DCA_ID 0x5B70
#define I211_REG_SWSM 0x5B50
#define I211_REG_FWSM 0x5B54
#define I211_REG_SW_FW_SYNC 0x5B5C
#define I211_REG_IPCNFG 0x0E38
#define I211_REG_PHPM 0x0E14

/* security */
#define I211_REG_EEC 0x12010
#define I211_REG_EEMNGCTL 0x12030
#define I211_REG_INVM_DATA 0x12120 /* 64 entries */
#define I211_REG_INVM_LOCK 0x12220 /* 32 entries */
#define I211_REG_INVM_PROTECT 0x12324

/* interrupts */
#define I211_REG_ICR 0x1500
#define I211_REG_ICS 0x1504
#define I211_REG_IMS 0x1508
#define I211_REG_IMC 0x150C
#define I211_REG_IAM 0x1510
#define I211_REG_EICS 0x1520
#define I211_REG_EIMS 0x1524
#define I211_REG_EIMC 0x1528
#define I211_REG_EIAC 0x152C
#define I211_REG_EIAM 0x1530
#define I211_REG_EICR 0x1580
#define I211_REG_IVAR 0x1700 /* 0x1700 - 0x170C */
#define I211_REG_IVAR_MISC 0x1740
#define I211_REG_EITR 0x1680 /* 0x1680 - 0x16A0 */
#define I211_REG_GPIE 0x1514
#define I211_REG_PBACL 0x5B68

/* receive pg 258*/
/* transmit pg 259*/

/* Filters pg 260 */
/* Per Queue Statistics pg 260 */
/* Statistics */
/* Wake Up and Proxying */
/* PCIe */
/* Memory Error Detection */
/* Power Management Registers */
/* Diagnostic */
/* PCS */
/* Time Sync */

/* Registers Sturcts */

/* 8.2.1 - Device Control Register */
typedef struct _I211_REGISTER_CTRL
{
    struct
    {
        ULONG FD                : 1;
        ULONG RsvdZ1            : 1;
        ULONG GIOMstrDis        : 1;
        ULONG RsvdZ2            : 1;
        ULONG SLU               : 1;
        ULONG ILOS              : 1;
        ULONG SPEED             : 2;
        ULONG RsvdZ3            : 1;
        ULONG FRCSPD            : 1;
        ULONG FRCDPLX           : 1;
        ULONG RsvdZ4            : 2;
        ULONG SDP0_GPIEN        : 1;
        ULONG SDP1_GPIEN        : 1;
        ULONG SDP0_DATA         : 1;
        ULONG SDP1_DATA         : 1;
        ULONG ADVD3WUC          : 1;
        ULONG SDP0_WDE          : 1;
        ULONG SDP0_IODIR        : 1;
        ULONG SDP1_IODIR        : 1;
        ULONG RsvdZ5            : 2;
        ULONG RST               : 1;
        ULONG RFCE              : 1;
        ULONG TFCE              : 1;
        ULONG DEV_RST           : 1;
        ULONG VME               : 1;
        ULONG PHY_RST           : 1;

    };
} I211_REGISTER_CTRL, *PI211_REGISTER_CTRL;
C_ASSERT(sizeof(I211_REGISTER_CTR) == 16);