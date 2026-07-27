/*
 * PROJECT:     ReactOS d3dx10
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GUID definitions for the WIC formats texture.c names
 *
 * The GUID_WICPixelFormat* constants live in cpp_quote() blocks in
 * wincodec.idl, so widl never emits them into the uuid import library the way
 * it does for interface and coclass IIDs -- every consumer has to instantiate
 * them itself. windowscodecs and gdiplus each carry a file exactly like this
 * one for the same reason.
 *
 * This is a ReactOS addition, not part of the Wine sources in this directory.
 */

/* DO NOT USE THE PRECOMPILED HEADER FOR THIS FILE! */

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <objbase.h>
#include <initguid.h>
#include <wincodecsdk.h>

/* NO CODE HERE, THIS IS JUST REQUIRED FOR THE GUID DEFINITIONS */
