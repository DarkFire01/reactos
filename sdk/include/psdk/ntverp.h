/*
 * PROJECT:         ReactOS
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            include/psdk/ntverp.h
 * PURPOSE:         Master Version File.
 *                  This file should be modified only by the official builder
 *                  to update VERSION, VER_PRODUCTVERSION, VER_PRODUCTVERSION_
 *                  STR and VER_PRODUCTBETA_STR values.
 *                  The VER_PRODUCTBUILD lines must contain the product
 *                  comments and end with the build#<CR><LF>.
 *                  The VER_PRODUCTBETA_STR lines must contain the product
 *                  comments and end with "somestring"<CR><LF>.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

#if 0
//Windows 11 target really shouldn't be built, it does boot though :)
//
// Windows NT Build 17764.2061
//
#define VER_PRODUCTBUILD                    22000
#define VER_PRODUCTBUILD_QFE                71
//
// Windows NT Version 10.1
//
#define VER_PRODUCTMAJORVERSION             11
#define VER_PRODUCTMINORVERSION             0
#define VER_PRODUCTVERSION_W                (0x0B00)
#define VER_PRODUCTVERSION_DW               (0x0B000000 | VER_PRODUCTBUILD)
#endif


//Windows 10 target really shouldn't be built, it does boot though :)
//
// Windows NT Build 17764.2061
//
#define VER_PRODUCTBUILD                    17764
#define VER_PRODUCTBUILD_QFE                2061
//
// Windows NT Version 10.1
//
#define VER_PRODUCTMAJORVERSION             10
#define VER_PRODUCTMINORVERSION             0
#define VER_PRODUCTVERSION_W                (0x0A00)
#define VER_PRODUCTVERSION_DW               (0x0A000000 | VER_PRODUCTBUILD)


#if 0
//
// Windows NT 6.3.9600.19846
//
#define VER_PRODUCTBUILD                    9600
#define VER_PRODUCTBUILD_QFE                19846
#define VER_PRODUCTMAJORVERSION             6
#define VER_PRODUCTMINORVERSION             3
#define VER_PRODUCTVERSION_W                (0x0603)
#define VER_PRODUCTVERSION_DW               (0x06030000 | VER_PRODUCTBUILD)
#endif

#if 0
//
// Windows NT 6.1.7601.17514
//
#define VER_PRODUCTBUILD                    7601
#define VER_PRODUCTBUILD_QFE                17514
#define VER_PRODUCTMAJORVERSION             6
#define VER_PRODUCTMINORVERSION             1
#define VER_PRODUCTVERSION_W                (0x0601)
#define VER_PRODUCTVERSION_DW               (0x06010000 | VER_PRODUCTBUILD)
#endif
#if 0
//
// Windows NT 6.0.6002.18005
//
#define VER_PRODUCTBUILD                    6002
#define VER_PRODUCTBUILD_QFE                18005
#define VER_PRODUCTMAJORVERSION             6
#define VER_PRODUCTMINORVERSION             0
#define VER_PRODUCTVERSION_W                (0x0600)
#define VER_PRODUCTVERSION_DW               (0x06000000 | VER_PRODUCTBUILD)

#endif

#if 0
//
// Windows NT Build 3790.3959
//
#define VER_PRODUCTBUILD                    3790
#define VER_PRODUCTBUILD_QFE                3959

//
// Windows NT Version 5.2
//
#define VER_PRODUCTMAJORVERSION             5
#define VER_PRODUCTMINORVERSION             2
#define VER_PRODUCTVERSION_W                (0x0502)
#define VER_PRODUCTVERSION_DW               (0x05020000 | VER_PRODUCTBUILD)

#endif
    
//
// Not a beta
//
#define VER_PRODUCTBETA_STR                 ""

//
// ANSI String Generating Macros
//
#define VER_PRODUCTVERSION_MAJORMINOR2(x,y) \
    #x "." #y
#define VER_PRODUCTVERSION_MAJORMINOR1(x,y) \
    VER_PRODUCTVERSION_MAJORMINOR2(x, y)
#define VER_PRODUCTVERSION_STRING           \
    VER_PRODUCTVERSION_MAJORMINOR1(VER_PRODUCTMAJORVERSION, VER_PRODUCTMINORVERSION)

//
// Unicode String Generating Macros
//
#define LVER_PRODUCTVERSION_MAJORMINOR2(x,y)\
    L#x L"." L#y
#define LVER_PRODUCTVERSION_MAJORMINOR1(x,y)\
    LVER_PRODUCTVERSION_MAJORMINOR2(x, y)
#define LVER_PRODUCTVERSION_STRING          \
    LVER_PRODUCTVERSION_MAJORMINOR1(VER_PRODUCTMAJORVERSION, VER_PRODUCTMINORVERSION)

//
// Full Product Version
//
#define VER_PRODUCTVERSION                  \
    VER_PRODUCTMAJORVERSION,VER_PRODUCTMINORVERSION,VER_PRODUCTBUILD,VER_PRODUCTBUILD_QFE

//
// Padding for ANSI Version String
//
#if     (VER_PRODUCTBUILD < 10)
#define VER_BPAD "000"
#elif   (VER_PRODUCTBUILD < 100)
#define VER_BPAD "00"
#elif   (VER_PRODUCTBUILD < 1000)
#define VER_BPAD "0"
#else
#define VER_BPAD
#endif

//
// Padding for Unicode Version String
//
#if     (VER_PRODUCTBUILD < 10)
#define LVER_BPAD L"000"
#elif   (VER_PRODUCTBUILD < 100)
#define LVER_BPAD L"00"
#elif   (VER_PRODUCTBUILD < 1000)
#define LVER_BPAD L"0"
#else
#define LVER_BPAD
#endif

//
// ANSI Product Version String
//
#define VER_PRODUCTVERSION_STR2(x,y)        \
    VER_PRODUCTVERSION_STRING "." VER_BPAD #x "." #y
#define VER_PRODUCTVERSION_STR1(x,y)        \
    VER_PRODUCTVERSION_STR2(x, y)
#define VER_PRODUCTVERSION_STR              \
    VER_PRODUCTVERSION_STR1(VER_PRODUCTBUILD, VER_PRODUCTBUILD_QFE)

//
// Unicode Product Version String
//
#define LVER_PRODUCTVERSION_STR2(x,y)       \
    LVER_PRODUCTVERSION_STRING L"." LVER_BPAD L#x L"." L#y
#define LVER_PRODUCTVERSION_STR1(x,y)       \
    LVER_PRODUCTVERSION_STR2(x, y)
#define LVER_PRODUCTVERSION_STR             \
    LVER_PRODUCTVERSION_STR1(VER_PRODUCTBUILD, VER_PRODUCTBUILD_QFE)

//
// Debug Flag
//
#if DBG
#define VER_DEBUG                           VS_FF_DEBUG
#else
#define VER_DEBUG                           0
#endif

//
// Beta Flag
//
#if BETA
#define VER_PRERELEASE                      VS_FF_PRERELEASE
#else
#define VER_PRERELEASE                      0
#endif

//
// Internal Flag
//
#if OFFICIAL_BUILD
#define VER_PRIVATE                         0
#else
#define VER_PRIVATE                         VS_FF_PRIVATEBUILD
#endif

//
// Other Flags
//
#define VER_FILEFLAGSMASK                   VS_FFI_FILEFLAGSMASK
#define VER_FILEOS                          VOS_NT_WINDOWS32
#define VER_FILEFLAGS                       (VER_PRERELEASE | \
                                             VER_DEBUG | \
                                             VER_PRIVATE)

//
// Company and Trademarks
//
#define VER_COMPANYNAME_STR                 \
    "ReactOS(R) Foundation"
#define VER_PRODUCTNAME_STR                 \
    "ReactOS(R) Operating System"
#define VER_LEGALTRADEMARKS_STR             \
    "ReactOS(R) is a registered trademark of the ReactOS Foundation."
