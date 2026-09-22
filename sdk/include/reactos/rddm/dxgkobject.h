/*
 * PROJECT:     ReactOS Display Driver Model (DxgKrnl_ms)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     dxgkrnl object-model skeleton (DXGGLOBAL / DXGADAPTER / ...)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * The Win10 dxgkrnl object model, ported from Reference/win10/dxgkrnl.{c,h}.
 * Phase 0 carries only forward declarations and the porting-discipline macro; the real
 * struct layouts (DXGADAPTER 4339 refs, DXGDEVICE 4312, ...) land in Phase 1, seeded from
 * the reference .h layouts. See RDDM-WDDM-ROADMAP.md §6.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RXGK_UNPORTED() — placed at the body of a function whose decompiled callees are not yet
 * translated. It keeps the tree linking at every commit (roadmap §5.5): logs once and
 * returns STATUS_NOT_IMPLEMENTED so a half-ported path fails loudly instead of silently.
 * Use RXGK_UNPORTED_VOID() in void functions.
 */
#define RXGK_UNPORTED()                                                       \
    do {                                                                      \
        DPRINT1("DxgKrnl_ms: UNPORTED %s\n", __FUNCTION__);                   \
        return STATUS_NOT_IMPLEMENTED;                                        \
    } while (0)

#define RXGK_UNPORTED_VOID()                                                  \
    do {                                                                      \
        DPRINT1("DxgKrnl_ms: UNPORTED %s\n", __FUNCTION__);                   \
        return;                                                               \
    } while (0)

/* Object-model forward declarations (layouts filled in Phase 1). */
typedef struct _DXGGLOBAL      DXGGLOBAL,      *PDXGGLOBAL;
typedef struct _DXGADAPTER     DXGADAPTER,     *PDXGADAPTER;
typedef struct _DXGPROCESS     DXGPROCESS,     *PDXGPROCESS;
typedef struct _DXGDEVICE      DXGDEVICE,      *PDXGDEVICE;
typedef struct _DXGCONTEXT     DXGCONTEXT,     *PDXGCONTEXT;
typedef struct _DXGALLOCATION  DXGALLOCATION,  *PDXGALLOCATION;
typedef struct _DXGRESOURCE    DXGRESOURCE,    *PDXGRESOURCE;
typedef struct _DXGSYNCOBJECT  DXGSYNCOBJECT,  *PDXGSYNCOBJECT;
typedef struct _DXGPAGINGQUEUE DXGPAGINGQUEUE, *PDXGPAGINGQUEUE;

#ifdef __cplusplus
}
#endif
