/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UFSHCI register and UFS protocol (UPIU/UTRD) definitions
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _UFSHCI_H_
#define _UFSHCI_H_

/* UFSHCI register offsets (JEDEC JESD223). */
#define UFS_REG_CAP       0x00  /* Controller Capabilities */
#define UFS_REG_VER       0x08  /* Version */
#define UFS_REG_HCDDID    0x10  /* Host Controller Device ID */
#define UFS_REG_HCPMID    0x14  /* Manufacturer ID */
#define UFS_REG_AHIT      0x18  /* Auto-Hibernate Idle Timer */
#define UFS_REG_IS        0x20  /* Interrupt Status */
#define UFS_REG_IE        0x24  /* Interrupt Enable */
#define UFS_REG_HCS       0x30  /* Host Controller Status */
#define UFS_REG_HCE       0x34  /* Host Controller Enable */
#define UFS_REG_UECPA     0x38  /* PHY Adapter layer error */
#define UFS_REG_UECDL     0x3C  /* Data Link layer error */
#define UFS_REG_UECN      0x40  /* Network layer error */
#define UFS_REG_UECT      0x44  /* Transport layer error */
#define UFS_REG_UECDME    0x48  /* DME error */
#define UFS_REG_UTRLBA    0x50  /* UTR List Base Address */
#define UFS_REG_UTRLBAU   0x54  /* UTR List Base Address Upper */
#define UFS_REG_UTRLDBR   0x58  /* UTR List Doorbell */
#define UFS_REG_UTRLCLR   0x5C  /* UTR List Clear */
#define UFS_REG_UTRLRSR   0x60  /* UTR List Run-Stop */
#define UFS_REG_UTRLCNR   0x64  /* UTR List Completion Notification */
#define UFS_REG_UTMRLBA   0x70  /* UTMR List Base Address */
#define UFS_REG_UTMRLBAU  0x74  /* UTMR List Base Address Upper */
#define UFS_REG_UTMRLDBR  0x78  /* UTMR List Doorbell */
#define UFS_REG_UTMRLCLR  0x7C  /* UTMR List Clear */
#define UFS_REG_UTMRLRSR  0x80  /* UTMR List Run-Stop */
#define UFS_REG_UICCMD    0x90  /* UIC Command */
#define UFS_REG_UCMDARG1  0x94  /* UIC Command Argument 1 */
#define UFS_REG_UCMDARG2  0x98  /* UIC Command Argument 2 */
#define UFS_REG_UCMDARG3  0x9C  /* UIC Command Argument 3 */

/* CAP register fields. */
#define UFS_CAP_NUTRS_MASK   0x0000001F  /* transfer request slots, 0 based */
#define UFS_CAP_NUTMRS_MASK  0x00070000  /* task management slots, 0 based */
#define UFS_CAP_NUTMRS_SHIFT 16
#define UFS_CAP_AUTOH8       0x00800000  /* auto-hibernate supported */
#define UFS_CAP_64AS         0x04000000  /* 64 bit addressing supported */
#define UFS_CAP_CS           0x10000000  /* crypto supported */

/* HCS register fields. */
#define UFS_HCS_DP           0x00000001  /* device present */
#define UFS_HCS_UTRLRDY      0x00000002  /* UTR list ready */
#define UFS_HCS_UTMRLRDY     0x00000004  /* UTMR list ready */
#define UFS_HCS_UCRDY        0x00000008  /* UIC command ready */

/* HCE register fields. */
#define UFS_HCE_ENABLE       0x00000001

/* IS/IE register fields. */
#define UFS_IS_UTRCS         0x00000001  /* UTR completion */
#define UFS_IS_UDEPRI        0x00000002  /* UTMR error */
#define UFS_IS_UE            0x00000004  /* UIC error */
#define UFS_IS_UTMS          0x00000008
#define UFS_IS_UPMS          0x00000010
#define UFS_IS_UHES          0x00000020  /* hibernate enter status */
#define UFS_IS_UHXS          0x00000040  /* hibernate exit status */
#define UFS_IS_UPMES         0x00000080
#define UFS_IS_UTMRCS        0x00000200  /* UTMR completion */
#define UFS_IS_UCCS          0x00000400  /* UIC command completion */
#define UFS_IS_DFES          0x00000800  /* device fatal error */
#define UFS_IS_UTPES         0x00001000
#define UFS_IS_HCFES         0x00010000  /* host controller fatal */
#define UFS_IS_SBFES         0x00020000  /* system bus fatal */
#define UFS_IS_CEFES         0x00040000  /* crypto engine fatal */

#define UFS_IS_FATAL_MASK \
    (UFS_IS_DFES | UFS_IS_HCFES | UFS_IS_SBFES | UFS_IS_CEFES)

/* The interrupts the miniport services. */
#define UFS_IE_DEFAULT \
    (UFS_IS_UTRCS | UFS_IS_UTMRCS | UFS_IS_UCCS | UFS_IS_UE | UFS_IS_FATAL_MASK)

/* UIC command opcodes. */
#define UFS_UIC_DME_GET            0x01
#define UFS_UIC_DME_SET            0x02
#define UFS_UIC_DME_PEER_GET       0x03
#define UFS_UIC_DME_PEER_SET       0x04
#define UFS_UIC_DME_POWERON        0x10
#define UFS_UIC_DME_POWEROFF       0x11
#define UFS_UIC_DME_ENABLE         0x12
#define UFS_UIC_DME_RESET          0x14
#define UFS_UIC_DME_ENDPOINTRESET  0x15
#define UFS_UIC_DME_LINKSTARTUP    0x16
#define UFS_UIC_DME_HIBERNATE_ENTER 0x17
#define UFS_UIC_DME_HIBERNATE_EXIT  0x18

/* UCMDARG2 low byte carries the UIC result code, 0 on success. */
#define UFS_UIC_RESULT_MASK        0x000000FF

/* UPIU transaction types (byte 0). */
#define UFS_UPIU_NOP_OUT           0x00
#define UFS_UPIU_COMMAND           0x01
#define UFS_UPIU_DATA_OUT          0x02
#define UFS_UPIU_TASK_REQUEST      0x04
#define UFS_UPIU_QUERY_REQUEST     0x16
#define UFS_UPIU_NOP_IN            0x20
#define UFS_UPIU_RESPONSE          0x21
#define UFS_UPIU_DATA_IN           0x22
#define UFS_UPIU_TASK_RESPONSE     0x24
#define UFS_UPIU_READY_TO_TRANSFER 0x31
#define UFS_UPIU_QUERY_RESPONSE    0x36
#define UFS_UPIU_REJECT            0x3F

/* Command UPIU flags (byte 1). */
#define UFS_UPIU_FLAG_READ         0x40
#define UFS_UPIU_FLAG_WRITE        0x20

/* Query request function (byte 1 of a query UPIU). */
#define UFS_QUERY_FUNC_READ        0x01  /* standard read request */
#define UFS_QUERY_FUNC_WRITE       0x81  /* standard write request */

/* Query opcodes. */
#define UFS_QUERY_READ_DESC        0x01
#define UFS_QUERY_WRITE_DESC       0x02
#define UFS_QUERY_READ_ATTR        0x03
#define UFS_QUERY_WRITE_ATTR       0x04
#define UFS_QUERY_READ_FLAG        0x05
#define UFS_QUERY_SET_FLAG         0x06
#define UFS_QUERY_CLEAR_FLAG       0x07
#define UFS_QUERY_TOGGLE_FLAG      0x08

/* Flag IDNs. */
#define UFS_FLAG_DEVICE_INIT       0x01

/* Descriptor IDNs. */
#define UFS_DESC_DEVICE            0x00
#define UFS_DESC_CONFIGURATION     0x01
#define UFS_DESC_UNIT              0x02
#define UFS_DESC_GEOMETRY          0x07

/* Overall Command Status values written by the host controller to the UTRD. */
#define UFS_OCS_SUCCESS            0x00
#define UFS_OCS_INVALID_CMD_TABLE  0x01
#define UFS_OCS_INVALID_PRDT       0x02
#define UFS_OCS_MISMATCH_DATA      0x03
#define UFS_OCS_MISMATCH_RESPONSE  0x04
#define UFS_OCS_INVALID_UPIU       0x05
#define UFS_OCS_PEER_COMM_FAILURE  0x06
#define UFS_OCS_ABORTED            0x07
#define UFS_OCS_FATAL_ERROR        0x08
#define UFS_OCS_INVALID            0x0F  /* initial value, "not completed yet" */

/* UTRD data direction (DW0 bits 26:25). */
#define UFS_UTRD_DD_NODATA         0
#define UFS_UTRD_DD_WRITE          1  /* host to device */
#define UFS_UTRD_DD_READ           2  /* device to host */

/* UTRD DW0 constant: CT=1 (UFS), bit24 set. The DD field is OR'd in. */
#define UFS_UTRD_DW0_BASE          0x11000000
#define UFS_UTRD_DD_SHIFT          25

#include <pshpack1.h>

/* UTP Transfer Request Descriptor, 32 bytes. */
typedef struct _UFS_UTRD
{
    ULONG Dw0Flags;           /* CT | DD | interrupt */
    ULONG Dw1;
    ULONG Dw2Ocs;             /* byte 0 = Overall Command Status */
    ULONG Dw3;
    ULONG UcdbaLower;         /* UTP Command Descriptor base, low */
    ULONG UcdbaUpper;         /* UTP Command Descriptor base, high */
    USHORT ResponseOffset;    /* in dwords from UCD base */
    USHORT ResponseLength;    /* in dwords */
    USHORT PrdtOffset;        /* in dwords from UCD base */
    USHORT PrdtLength;        /* in dwords (0 for no data) */
} UFS_UTRD, *PUFS_UTRD;

/* PRDT entry, 16 bytes. ByteCount holds (length - 1), dword granular. */
typedef struct _UFS_PRDT_ENTRY
{
    ULONG BaseLower;
    ULONG BaseUpper;
    ULONG Reserved;
    ULONG ByteCount;          /* bits 17:0 = byteCount - 1 */
} UFS_PRDT_ENTRY, *PUFS_PRDT_ENTRY;

/*
 * Basic Command UPIU, 32 bytes, followed by the 16 byte CDB.
 * Multi-byte length fields are big-endian on the wire.
 */
typedef struct _UFS_COMMAND_UPIU
{
    UCHAR TransactionType;    /* 0x01 */
    UCHAR Flags;              /* read/write */
    UCHAR Lun;
    UCHAR TaskTag;
    UCHAR CommandSetType;     /* 0 = SCSI */
    UCHAR Reserved1;
    UCHAR Function;
    UCHAR Response;
    UCHAR TotalEhsLength;
    UCHAR Reserved2;
    USHORT DataSegmentLength; /* big-endian */
    ULONG ExpectedDataLength; /* big-endian */
    UCHAR Cdb[16];
} UFS_COMMAND_UPIU, *PUFS_COMMAND_UPIU;

/* Response UPIU, 32 bytes, optional sense data in the data segment. */
typedef struct _UFS_RESPONSE_UPIU
{
    UCHAR TransactionType;    /* 0x21 */
    UCHAR Flags;
    UCHAR Lun;
    UCHAR TaskTag;
    UCHAR CommandSetType;
    UCHAR Reserved1;
    UCHAR Response;           /* UPIU level response, 0 = target success */
    UCHAR Status;             /* SCSI status */
    UCHAR TotalEhsLength;
    UCHAR DeviceInformation;
    USHORT DataSegmentLength; /* big-endian */
    ULONG ResidualCount;      /* big-endian */
    ULONG Reserved2[4];
    USHORT SenseDataLength;   /* big-endian, first word of data segment */
    UCHAR SenseData[18];
} UFS_RESPONSE_UPIU, *PUFS_RESPONSE_UPIU;

/* Query Request/Response UPIU, 32 byte header + transaction specific fields. */
typedef struct _UFS_QUERY_UPIU
{
    UCHAR TransactionType;    /* 0x16 request, 0x36 response */
    UCHAR Flags;              /* 0x01, reserved for query (0) */
    UCHAR Reserved0;          /* 0x02 */
    UCHAR TaskTag;            /* 0x03 */
    UCHAR Reserved1;          /* 0x04 */
    UCHAR QueryFunction;      /* 0x05: 0x01 read, 0x81 write */
    UCHAR Response;           /* 0x06: response code, 0 = success (response UPIU) */
    UCHAR Reserved2;          /* 0x07 */
    UCHAR TotalEhsLength;     /* 0x08 */
    UCHAR DeviceInformation;  /* 0x09 */
    USHORT DataSegmentLength; /* 0x0A big-endian */
    UCHAR Opcode;             /* 0x0C query opcode */
    UCHAR Idn;                /* 0x0D */
    UCHAR Index;              /* 0x0E */
    UCHAR Selector;           /* 0x0F */
    USHORT Reserved3;         /* 0x10 */
    USHORT Length;            /* 0x12 big-endian, descriptor length */
    ULONG Value;              /* 0x14 attribute value (big-endian); flag in LSB at 0x17 */
    ULONG Reserved4[2];       /* 0x18 */
} UFS_QUERY_UPIU, *PUFS_QUERY_UPIU;

#include <poppack.h>

#endif /* _UFSHCI_H_ */
