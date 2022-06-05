

//a macro used to build up a valid method
#define EncodeMethod(subchannel,command,nparam) ((nparam<<18)+(subchannel<<13)+command)
#define VIDEO_BASE 0xFD000000
//Access macros (volatile means C optimizer must not consider useless to read again Ram)
#define VIDEOREG(x)   (*(volatile unsigned int*)(VIDEO_BASE + (x)))
#define VIDEOREG16(x) (*(volatile unsigned short*)(VIDEO_BASE + (x)))
#define VIDEOREG8(x)  (*(volatile unsigned char*)(VIDEO_BASE + (x)))


#define INSTANCE_MEM_MAXSIZE                0x5000  //20Kb

#define ADDR_SYSMEM                 1
#define ADDR_FBMEM                  2
#define ADDR_AGPMEM                 3


#define GPU_IRQ                     3

#define XTAL_16MHZ                  16.6667f
#define DW_XTAL_16MHZ                   16666666

#define MAX_EXTRA_BUFFERS               8

#define MAXRAM                      0x03FFAFFF

#define NONE                        -1

#define TICKSTIMEOUT                    100 //if Dma doesn't react in that time, send a warning

#define PB_SETOUTER                 0xB2A
#define PB_SETNOISE                 0xBAA
#define PB_FINISHED                 0xFAB
#if 0
static  DWORD32         pb_3DGrCtxInst[2]={0,0};//Adress of the two 3D graphic contexts (addr=inst<<4+NV_PRAMIN)
static  DWORD32         pb_GrCtxTableInst;  //Adress of the table that points at the two graphic contexts
static  DWORD32         pb_GrCtxInst[2];    //Adress of the two graphic contexts (addr=inst<<4+NV_PRAMIN)
static  int         pb_GrCtxID;     //Current context ID : 0,1 or NONE
#endif