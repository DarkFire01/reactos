/** @file
  DfBrLayoutArena.c - chunked bump allocator for layout boxes + aux
                      strings. Same pattern as DfBrHtml / DfBrCss.
**/

#include "DfBrLayoutInternal.h"

#include <stdlib.h>
#include <string.h>

#define DFBR_LAYOUT_CHUNK_MIN 4096

static DfBrLayoutArenaChunk *
AllocChunk (usize Bytes) {
  usize                  Cap = (Bytes > DFBR_LAYOUT_CHUNK_MIN) ? Bytes : DFBR_LAYOUT_CHUNK_MIN;
  DfBrLayoutArenaChunk  *C   = (DfBrLayoutArenaChunk *)malloc (sizeof (*C));
  if (C == DFBR_NULL) return DFBR_NULL;
  C->Data = (u8 *)malloc (Cap);
  if (C->Data == DFBR_NULL) { free (C); return DFBR_NULL; }
  C->Used     = 0;
  C->Capacity = Cap;
  C->Next     = DFBR_NULL;
  return C;
}

void *
DfBrLayoutArenaAlloc (DfBrLayoutArena *A, usize Bytes) {
  DfBrLayoutArenaChunk *C;
  void                 *P;
  usize                 Aligned = (Bytes + 7u) & ~((usize)7);

  if (A == DFBR_NULL || Aligned == 0) return DFBR_NULL;

  C = A->Current;
  if (C == DFBR_NULL || C->Used + Aligned > C->Capacity) {
    DfBrLayoutArenaChunk *N = AllocChunk (Aligned);
    if (N == DFBR_NULL) return DFBR_NULL;
    if (A->First == DFBR_NULL) A->First = N;
    else                        A->Current->Next = N;
    A->Current = N;
    C = N;
  }

  P = C->Data + C->Used;
  C->Used += Aligned;
  memset (P, 0, Bytes);
  return P;
}

char *
DfBrLayoutArenaStrdupN (DfBrLayoutArena *A, const char *Src, usize Len) {
  char *Dst;
  if (A == DFBR_NULL) return DFBR_NULL;
  Dst = (char *)DfBrLayoutArenaAlloc (A, Len + 1);
  if (Dst == DFBR_NULL) return DFBR_NULL;
  if (Src != DFBR_NULL && Len > 0) memcpy (Dst, Src, Len);
  Dst[Len] = '\0';
  return Dst;
}

void
DfBrLayoutArenaDispose (DfBrLayoutArena *A) {
  DfBrLayoutArenaChunk *C;
  if (A == DFBR_NULL) return;
  C = A->First;
  while (C != DFBR_NULL) {
    DfBrLayoutArenaChunk *N = C->Next;
    free (C->Data);
    free (C);
    C = N;
  }
  A->First = DFBR_NULL;
  A->Current = DFBR_NULL;
}
