/** @file
  DfBrCanvas.c - pixel backing + rasterizer for <canvas>. See header
                 for design rationale.

  The implementation is deliberately tiny and self-contained. We
  malloc pixel arrays (can't arena-allocate them since the canvas can
  be resized) and register each backing on the document so
  DfBrHtmlDocumentDestroy can free them in a single pass.

  No SIMD, no fast paths - the goal is correctness and "enough to
  render the pixels an actual page draws". If something like a
  colour-picker game eats perf here we can revisit with row fill
  intrinsics. Real-world sites we target (feature-detect only or
  simple icon canvases) never hit the hot path.
**/

#include "DfBrCanvas.h"
#include "DfBrHtmlInternal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 *  Document-level registration
 *
 *  Each canvas node owns exactly one DfBrCanvasBacking stored in its
 *  ScriptData slot. Backings are chained via DocNext off
 *  DfBrHtmlDocument::CanvasListHead so teardown is a single walk.
 * -----------------------------------------------------------------*/

static void
RegisterWithDoc (DfBrHtmlDocument *Doc, DfBrCanvasBacking *B)
{
  if (Doc == DFBR_NULL || B == DFBR_NULL) return;
  B->DocNext = (DfBrCanvasBacking *)Doc->CanvasListHead;
  Doc->CanvasListHead = B;
}

DfBrCanvasBacking *
DfBrCanvasGet (
  const DfBrHtmlNode *Node
  )
{
  if (Node == DFBR_NULL) return DFBR_NULL;
  if (Node->Kind != DFBR_HTML_NODE_ELEMENT) return DFBR_NULL;
  return (DfBrCanvasBacking *)Node->ScriptData;
}

DfBrCanvasBacking *
DfBrCanvasEnsure (
  DfBrHtmlNode     *Node,
  DfBrHtmlDocument *Doc,
  u32               Width,
  u32               Height
  )
{
  DfBrCanvasBacking *B;
  u32                NewW;
  u32                NewH;

  if (Node == DFBR_NULL) return DFBR_NULL;
  if (Node->Kind != DFBR_HTML_NODE_ELEMENT) return DFBR_NULL;
  if (Node->Tag  != DFBR_HTML_TAG_CANVAS)   return DFBR_NULL;

  B = (DfBrCanvasBacking *)Node->ScriptData;

  /* HTML spec defaults when no attribute set. */
  NewW = (Width  > 0) ? Width  : (B ? B->Width  : 300);
  NewH = (Height > 0) ? Height : (B ? B->Height : 150);
  /* Guard against hostile inputs. 32 Mpx keeps a single backing under
   * 128 MB which is a comfortable ceiling for a stub renderer. */
  if (NewW > 16384u) NewW = 16384u;
  if (NewH > 16384u) NewH = 16384u;
  if (NewW == 0) NewW = 1;
  if (NewH == 0) NewH = 1;

  if (B == DFBR_NULL) {
    B = (DfBrCanvasBacking *)calloc (1, sizeof (*B));
    if (B == DFBR_NULL) return DFBR_NULL;
    B->Width  = NewW;
    B->Height = NewH;
    B->Pixels = (u8 *)calloc ((usize)NewW * NewH, 4);
    if (B->Pixels == DFBR_NULL) { free (B); return DFBR_NULL; }
    Node->ScriptData = B;
    RegisterWithDoc (Doc, B);
    return B;
  }

  if (B->Width == NewW && B->Height == NewH) return B;

  /* Resize: the spec says changing width/height clears the canvas to
   * transparent black. Simpler and matches what authors expect. */
  {
    u8 *NewPixels = (u8 *)calloc ((usize)NewW * NewH, 4);
    if (NewPixels == DFBR_NULL) return B;  /* keep old on OOM */
    free (B->Pixels);
    B->Pixels = NewPixels;
    B->Width  = NewW;
    B->Height = NewH;
  }
  return B;
}

void
DfBrCanvasDestroyAll (
  DfBrHtmlDocument *Doc
  )
{
  DfBrCanvasBacking *B;
  DfBrCanvasBacking *Next;

  if (Doc == DFBR_NULL) return;
  B = (DfBrCanvasBacking *)Doc->CanvasListHead;
  while (B != DFBR_NULL) {
    Next = B->DocNext;
    if (B->Pixels != DFBR_NULL) { free (B->Pixels); B->Pixels = DFBR_NULL; }
    free (B);
    B = Next;
  }
  Doc->CanvasListHead = DFBR_NULL;
}

/* ==================================================================
 *  Color parsing
 *
 *  Supported syntaxes:
 *
 *    #rgb          3-digit hex            (short form, each nibble x17)
 *    #rgba         4-digit hex            (3-digit + alpha)
 *    #rrggbb       6-digit hex
 *    #rrggbbaa     8-digit hex
 *    rgb(R, G, B)                         (R,G,B: 0..255 int or 0..100%)
 *    rgba(R, G, B, A)                     (A: 0..1)
 *    transparent
 *    <named>                              tiny curated list
 *
 *  We intentionally keep the named-colour table short. Real-world
 *  canvas code almost always uses hex or rgb(a); named colours beyond
 *  the CSS3 primary set are rare enough that "unknown -> black" is
 *  fine. Authors who notice can file tickets.
 * ================================================================*/

typedef struct { const char *Name; u8 R, G, B; } NamedColor;

static const NamedColor mNamed[] = {
  { "black",        0,   0,   0   },
  { "silver",       192, 192, 192 },
  { "gray",         128, 128, 128 },
  { "grey",         128, 128, 128 },
  { "white",        255, 255, 255 },
  { "maroon",       128, 0,   0   },
  { "red",          255, 0,   0   },
  { "purple",       128, 0,   128 },
  { "fuchsia",      255, 0,   255 },
  { "magenta",      255, 0,   255 },
  { "green",        0,   128, 0   },
  { "lime",         0,   255, 0   },
  { "olive",        128, 128, 0   },
  { "yellow",       255, 255, 0   },
  { "navy",         0,   0,   128 },
  { "blue",         0,   0,   255 },
  { "teal",         0,   128, 128 },
  { "aqua",         0,   255, 255 },
  { "cyan",         0,   255, 255 },
  { "orange",       255, 165, 0   },
  { "pink",         255, 192, 203 },
  { "brown",        165, 42,  42  },
  { "gold",         255, 215, 0   },
  { "indigo",       75,  0,   130 },
  { "violet",       238, 130, 238 }
};

static b32
HexNibble (char C, u32 *Out)
{
  if (C >= '0' && C <= '9') { *Out = (u32)(C - '0');          return DFBR_TRUE; }
  if (C >= 'a' && C <= 'f') { *Out = (u32)(C - 'a' + 10);     return DFBR_TRUE; }
  if (C >= 'A' && C <= 'F') { *Out = (u32)(C - 'A' + 10);     return DFBR_TRUE; }
  return DFBR_FALSE;
}

static b32
ParseHex (const char *S, usize Len, u8 *R, u8 *G, u8 *B, u8 *A)
{
  u32 v[8];
  usize i;
  if (Len != 3 && Len != 4 && Len != 6 && Len != 8) return DFBR_FALSE;
  for (i = 0; i < Len; i++) {
    if (!HexNibble (S[i], &v[i])) return DFBR_FALSE;
  }
  if (Len == 3 || Len == 4) {
    *R = (u8)((v[0] << 4) | v[0]);
    *G = (u8)((v[1] << 4) | v[1]);
    *B = (u8)((v[2] << 4) | v[2]);
    *A = (Len == 4) ? (u8)((v[3] << 4) | v[3]) : 255;
  } else {
    *R = (u8)((v[0] << 4) | v[1]);
    *G = (u8)((v[2] << 4) | v[3]);
    *B = (u8)((v[4] << 4) | v[5]);
    *A = (Len == 8) ? (u8)((v[6] << 4) | v[7]) : 255;
  }
  return DFBR_TRUE;
}

static void
SkipWs (const char **P, const char *End)
{
  while (*P < End &&
         (**P == ' ' || **P == '\t' || **P == '\n' || **P == '\r'))
    (*P)++;
}

static b32
ReadComp (const char **P, const char *End, f32 *Out, b32 AllowPercent)
{
  char        Buf[32];
  usize       N = 0;
  const char *Q;
  char       *EndPtr = DFBR_NULL;
  f32         V;
  b32         Pct = DFBR_FALSE;

  SkipWs (P, End);
  Q = *P;
  while (Q < End && ((*Q >= '0' && *Q <= '9') || *Q == '.' || *Q == '-' || *Q == '+')) {
    if (N + 1 >= sizeof (Buf)) return DFBR_FALSE;
    Buf[N++] = *Q++;
  }
  if (N == 0) return DFBR_FALSE;
  Buf[N] = '\0';
  *P = Q;
  V = (f32)strtod (Buf, &EndPtr);
  if (EndPtr == Buf) return DFBR_FALSE;
  SkipWs (P, End);
  if (AllowPercent && *P < End && **P == '%') { Pct = DFBR_TRUE; (*P)++; SkipWs (P, End); }
  if (Pct) V = V * 255.0f / 100.0f;
  *Out = V;
  return DFBR_TRUE;
}

static b32
ExpectChar (const char **P, const char *End, char C)
{
  SkipWs (P, End);
  if (*P >= End || **P != C) return DFBR_FALSE;
  (*P)++;
  SkipWs (P, End);
  return DFBR_TRUE;
}

static u8
ClampByte (f32 V)
{
  if (V < 0.0f)   V = 0.0f;
  if (V > 255.0f) V = 255.0f;
  return (u8)(V + 0.5f);
}

static u8
ClampAlpha (f32 V)
{
  if (V < 0.0f) V = 0.0f;
  if (V > 1.0f) V = 1.0f;
  return (u8)(V * 255.0f + 0.5f);
}

b32
DfBrCanvasParseColor (
  const char *Str,
  u8         *OutR,
  u8         *OutG,
  u8         *OutB,
  u8         *OutA
  )
{
  const char *P;
  const char *End;
  usize       Len;
  usize       i;

  if (OutR) *OutR = 0;
  if (OutG) *OutG = 0;
  if (OutB) *OutB = 0;
  if (OutA) *OutA = 255;

  if (Str == DFBR_NULL) return DFBR_FALSE;
  Len = strlen (Str);
  if (Len == 0) return DFBR_FALSE;

  /* trim leading / trailing whitespace */
  P   = Str;
  End = Str + Len;
  while (P < End && (*P == ' ' || *P == '\t')) P++;
  while (End > P && (End[-1] == ' ' || End[-1] == '\t')) End--;
  if (P >= End) return DFBR_FALSE;

  if (*P == '#') {
    u8 R, G, B, A;
    if (!ParseHex (P + 1, (usize)(End - P - 1), &R, &G, &B, &A)) return DFBR_FALSE;
    if (OutR) *OutR = R; if (OutG) *OutG = G;
    if (OutB) *OutB = B; if (OutA) *OutA = A;
    return DFBR_TRUE;
  }

  {
    /* lowercase the functional form's prefix for comparison */
    char Head[8];
    usize HL = (usize)(End - P);
    if (HL >= sizeof (Head)) HL = sizeof (Head) - 1;
    for (i = 0; i < HL; i++) {
      char C = P[i];
      if (C >= 'A' && C <= 'Z') C = (char)(C - 'A' + 'a');
      Head[i] = C;
    }
    Head[HL] = '\0';

    if (HL >= 4 && (memcmp (Head, "rgb(", 4) == 0 ||
                    memcmp (Head, "rgba", 4) == 0))
    {
      f32          R, G, B, A = 1.0f;
      const char  *Q       = P + ((memcmp (Head, "rgba", 4) == 0) ? 5 : 4);
      b32          WantA   = (memcmp (Head, "rgba", 4) == 0);
      if (!ReadComp (&Q, End, &R, DFBR_TRUE)) return DFBR_FALSE;
      if (!ExpectChar (&Q, End, ','))         return DFBR_FALSE;
      if (!ReadComp (&Q, End, &G, DFBR_TRUE)) return DFBR_FALSE;
      if (!ExpectChar (&Q, End, ','))         return DFBR_FALSE;
      if (!ReadComp (&Q, End, &B, DFBR_TRUE)) return DFBR_FALSE;
      if (WantA) {
        if (!ExpectChar (&Q, End, ','))        return DFBR_FALSE;
        if (!ReadComp (&Q, End, &A, DFBR_FALSE)) return DFBR_FALSE;
      }
      if (!ExpectChar (&Q, End, ')'))         return DFBR_FALSE;
      if (OutR) *OutR = ClampByte (R);
      if (OutG) *OutG = ClampByte (G);
      if (OutB) *OutB = ClampByte (B);
      if (OutA) *OutA = ClampAlpha (A);
      return DFBR_TRUE;
    }

    if (HL == 11 && memcmp (Head, "transparent", 7) == 0) {
      /* Head is truncated at 7 chars; do a full compare against the
       * source range. */
    }
  }

  /* transparent + named lookup (case-insensitive). */
  {
    char  Norm[32];
    usize NL = (usize)(End - P);
    if (NL >= sizeof (Norm)) return DFBR_FALSE;
    for (i = 0; i < NL; i++) {
      char C = P[i];
      if (C >= 'A' && C <= 'Z') C = (char)(C - 'A' + 'a');
      Norm[i] = C;
    }
    Norm[NL] = '\0';
    if (strcmp (Norm, "transparent") == 0) {
      if (OutA) *OutA = 0;
      return DFBR_TRUE;
    }
    for (i = 0; i < sizeof (mNamed) / sizeof (mNamed[0]); i++) {
      if (strcmp (Norm, mNamed[i].Name) == 0) {
        if (OutR) *OutR = mNamed[i].R;
        if (OutG) *OutG = mNamed[i].G;
        if (OutB) *OutB = mNamed[i].B;
        if (OutA) *OutA = 255;
        return DFBR_TRUE;
      }
    }
  }
  return DFBR_FALSE;
}

/* ==================================================================
 *  Rasterizer
 * ================================================================*/

static void
ClipRect (
  const DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H,
  i32 *Px0, i32 *Py0, i32 *Px1, i32 *Py1
  )
{
  i32 X0 = (i32)(X + 0.5f);
  i32 Y0 = (i32)(Y + 0.5f);
  i32 X1 = (i32)(X + W + 0.5f);
  i32 Y1 = (i32)(Y + H + 0.5f);
  if (X0 < 0) X0 = 0;
  if (Y0 < 0) Y0 = 0;
  if ((u32)X1 > B->Width)  X1 = (i32)B->Width;
  if ((u32)Y1 > B->Height) Y1 = (i32)B->Height;
  *Px0 = X0; *Py0 = Y0; *Px1 = X1; *Py1 = Y1;
}

/* Source-over blend of a single RGBA8 pixel in the canvas's
 * non-premultiplied storage format.
 *
 *   out.a = src.a + dst.a * (1 - src.a)
 *   out.c = ( src.c * src.a + dst.c * dst.a * (1 - src.a) ) / out.a
 *
 * Browsers expose canvas pixels non-premultiplied via
 * getImageData, so we have to undo the classic premultiplied
 * source-over formula at the end. Doing the math in integers
 * (scaled by 255) keeps the result byte-exact with the JS
 * readback. */
static void
BlendPixel (u8 *Dst, u8 R, u8 G, u8 Bl, u8 A)
{
  u32 InvA;
  u32 DstA;
  u32 OutA;
  u32 DstRContrib, DstGContrib, DstBContrib;
  u32 SrcRContrib, SrcGContrib, SrcBContrib;

  if (A == 0) return;
  if (A == 255) {
    Dst[0] = R; Dst[1] = G; Dst[2] = Bl; Dst[3] = 255;
    return;
  }

  DstA = Dst[3];
  if (DstA == 0) {
    /* Empty destination: the composited pixel is just the source.
     * Fast path plus sidestep of divide-by-zero for OutA == 0. */
    Dst[0] = R; Dst[1] = G; Dst[2] = Bl; Dst[3] = A;
    return;
  }

  InvA = 255u - A;
  /* out_a * 255 = A*255 + DstA * InvA */
  OutA = A * 255u + DstA * InvA;
  /* Weighted contributions; both normalised to *255 at this stage. */
  SrcRContrib = (u32)R  * A * 255u;
  SrcGContrib = (u32)G  * A * 255u;
  SrcBContrib = (u32)Bl * A * 255u;
  DstRContrib = (u32)Dst[0] * DstA * InvA;
  DstGContrib = (u32)Dst[1] * DstA * InvA;
  DstBContrib = (u32)Dst[2] * DstA * InvA;
  /* Final rgb = (SrcContrib + DstContrib) / (OutA * 255).
   * We have OutA in units of *255, so divide by OutA only. */
  Dst[0] = (u8)(((SrcRContrib + DstRContrib) + OutA / 2u) / OutA);
  Dst[1] = (u8)(((SrcGContrib + DstGContrib) + OutA / 2u) / OutA);
  Dst[2] = (u8)(((SrcBContrib + DstBContrib) + OutA / 2u) / OutA);
  Dst[3] = (u8)((OutA + 127u) / 255u);
}

void
DfBrCanvasFillCircle (
  DfBrCanvasBacking *B,
  f32 CX, f32 CY, f32 Rad,
  u8 R, u8 G, u8 Bl, u8 A
  )
{
  i32 X0, Y0, X1, Y1, Px, Py;
  f32 Rsq;
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL) return;
  if (!(Rad > 0.0f)) return;
  Rsq = Rad * Rad;
  ClipRect (B, CX - Rad, CY - Rad, 2.0f * Rad, 2.0f * Rad, &X0, &Y0, &X1, &Y1);
  if (X0 >= X1 || Y0 >= Y1) return;
  for (Py = Y0; Py < Y1; Py++) {
    u8 *Row = B->Pixels + (usize)Py * B->Width * 4;
    f32 Dy = ((f32)Py + 0.5f) - CY;
    for (Px = X0; Px < X1; Px++) {
      f32 Dx = ((f32)Px + 0.5f) - CX;
      if (Dx * Dx + Dy * Dy <= Rsq) {
        BlendPixel (Row + (usize)Px * 4, R, G, Bl, A);
      }
    }
  }
}

void
DfBrCanvasFillRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H,
  u8 R, u8 G, u8 Bl, u8 A
  )
{
  i32 X0, Y0, X1, Y1, Px, Py;
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL) return;
  if (W <= 0.0f || H <= 0.0f) return;
  ClipRect (B, X, Y, W, H, &X0, &Y0, &X1, &Y1);
  if (X0 >= X1 || Y0 >= Y1) return;
  for (Py = Y0; Py < Y1; Py++) {
    u8 *Row = B->Pixels + (usize)Py * B->Width * 4;
    for (Px = X0; Px < X1; Px++) {
      BlendPixel (Row + (usize)Px * 4, R, G, Bl, A);
    }
  }
}

void
DfBrCanvasClearRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H
  )
{
  i32 X0, Y0, X1, Y1, Py;
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL) return;
  if (W <= 0.0f || H <= 0.0f) return;
  ClipRect (B, X, Y, W, H, &X0, &Y0, &X1, &Y1);
  if (X0 >= X1 || Y0 >= Y1) return;
  for (Py = Y0; Py < Y1; Py++) {
    u8 *Row = B->Pixels + (usize)Py * B->Width * 4 + (usize)X0 * 4;
    memset (Row, 0, (usize)(X1 - X0) * 4);
  }
}

void
DfBrCanvasStrokeRect (
  DfBrCanvasBacking *B,
  f32 X, f32 Y, f32 W, f32 H,
  f32 LineWidth,
  u8 R, u8 G, u8 Bl, u8 A
  )
{
  f32 Lw;
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL) return;
  Lw = (LineWidth >= 1.0f) ? LineWidth : 1.0f;
  /* Four edge strips. Center the stroke on the rect edges. */
  DfBrCanvasFillRect (B, X - Lw * 0.5f, Y - Lw * 0.5f, W + Lw, Lw, R, G, Bl, A);
  DfBrCanvasFillRect (B, X - Lw * 0.5f, Y + H - Lw * 0.5f, W + Lw, Lw, R, G, Bl, A);
  DfBrCanvasFillRect (B, X - Lw * 0.5f, Y + Lw * 0.5f, Lw, H - Lw, R, G, Bl, A);
  DfBrCanvasFillRect (B, X + W - Lw * 0.5f, Y + Lw * 0.5f, Lw, H - Lw, R, G, Bl, A);
}

void
DfBrCanvasFillAll (
  DfBrCanvasBacking *B,
  u8 R, u8 G, u8 Bl, u8 A
  )
{
  if (B == DFBR_NULL) return;
  DfBrCanvasFillRect (B, 0.0f, 0.0f, (f32)B->Width, (f32)B->Height, R, G, Bl, A);
}

void
DfBrCanvasPutRgba (
  DfBrCanvasBacking *B,
  i32 DestX, i32 DestY,
  const u8 *Src,
  u32 SrcW, u32 SrcH
  )
{
  i32 X0, Y0, X1, Y1, Py;
  u32 SrcOffX, SrcOffY;
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL || Src == DFBR_NULL) return;
  if (SrcW == 0 || SrcH == 0) return;
  X0 = DestX; Y0 = DestY;
  X1 = DestX + (i32)SrcW; Y1 = DestY + (i32)SrcH;
  SrcOffX = 0; SrcOffY = 0;
  if (X0 < 0) { SrcOffX = (u32)(-X0); X0 = 0; }
  if (Y0 < 0) { SrcOffY = (u32)(-Y0); Y0 = 0; }
  if ((u32)X1 > B->Width)  X1 = (i32)B->Width;
  if ((u32)Y1 > B->Height) Y1 = (i32)B->Height;
  if (X0 >= X1 || Y0 >= Y1) return;
  for (Py = Y0; Py < Y1; Py++) {
    u8       *Dst = B->Pixels + (usize)Py * B->Width * 4 + (usize)X0 * 4;
    const u8 *Row = Src +
      (usize)(SrcOffY + (u32)(Py - Y0)) * SrcW * 4 + (usize)SrcOffX * 4;
    memcpy (Dst, Row, (usize)(X1 - X0) * 4);
  }
}

void
DfBrCanvasReadRgba (
  const DfBrCanvasBacking *B,
  i32 SrcX, i32 SrcY,
  u8 *Dst,
  u32 DstW, u32 DstH
  )
{
  u32 Py, Px;
  if (Dst == DFBR_NULL || DstW == 0 || DstH == 0) return;
  memset (Dst, 0, (usize)DstW * DstH * 4);
  if (B == DFBR_NULL || B->Pixels == DFBR_NULL) return;
  for (Py = 0; Py < DstH; Py++) {
    i32 Sy = SrcY + (i32)Py;
    if (Sy < 0 || (u32)Sy >= B->Height) continue;
    for (Px = 0; Px < DstW; Px++) {
      i32 Sx = SrcX + (i32)Px;
      u8 *D;
      const u8 *S;
      if (Sx < 0 || (u32)Sx >= B->Width) continue;
      D = Dst    + (usize)(Py * DstW + Px) * 4;
      S = B->Pixels + (usize)((u32)Sy * B->Width + (u32)Sx) * 4;
      D[0] = S[0]; D[1] = S[1]; D[2] = S[2]; D[3] = S[3];
    }
  }
}
