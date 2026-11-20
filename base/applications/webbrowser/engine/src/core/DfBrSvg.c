/** @file
  DfBrSvg.c - minimal SVG 1.1 rasterizer.

  Architecture (unchanged from the UEFI-era SvgLite this was ported from):
    1. A tiny XML scanner walks the source producing element-open/close +
       attribute callbacks. No DOM: we execute a recursive descent with a
       state stack of cascading paint properties and transform matrices.
    2. Shapes flatten into a unified point buffer with per-subpath break
       markers. Paths with curves are subdivided adaptively (De Casteljau)
       until each chord is within 0.5 px of the ideal curve.
    3. Filled shapes feed a scanline polygon filler (non-zero winding via
       even-odd crossing count). Stroked shapes feed a thick-line
       rasterizer that converts each segment to an oriented rectangle.

  The port: UEFI-flavored primitives (CHAR16, EFIAPI, AllocatePool,
  EFI_STATUS, EFI_GRAPHICS_OUTPUT_BLT_PIXEL, ...) are gone. In their
  place the module uses the universal types in DfBrTypes.h and plain
  ASCII input. Output is a BGRA DfBrBitmap pre-composited over white.
**/

#include "DfBrSvg.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Allocation helpers. Centralized so a UEFI port can swap in
 * AllocatePool / AllocateZeroPool / FreePool without touching the
 * rasterizer body.
 * ------------------------------------------------------------------ */
static void *SvgAlloc  (usize N) { return malloc (N); }
static void *SvgZalloc (usize N) { void *P = malloc (N); if (P) memset (P, 0, N); return P; }
static void  SvgFree   (void *P) { free (P); }

/* ------------------------------------------------------------------ *
 * Small math helpers - no libm dependency so the module compiles in
 * freestanding environments (UEFI, kernels, WASM).
 * ------------------------------------------------------------------ */
static f32 Fabs32 (f32 V) { return (V < 0.0f) ? -V : V; }

static f32 Sqrt32 (f32 V) {
  /* Newton-Raphson. 8 iters is plenty for stroke half-widths / edge
   * lengths - we're not doing orbital mechanics. */
  f32 X;
  i32 I;
  if (V <= 0.0f) return 0.0f;
  X = V;
  for (I = 0; I < 8; I++) {
    X = 0.5f * (X + V / X);
  }
  return X;
}

static f32 Sin32 (f32 A) {
  /* Bhaskara I's approximation (~0.001 max error). Range-reduce to
   * [-PI, PI] first. */
  const f32 Pi   = 3.14159265f;
  const f32 TwoP = 6.28318531f;
  f32 Sign = 1.0f;
  f32 Num, Den;
  while (A >  Pi) A -= TwoP;
  while (A < -Pi) A += TwoP;
  if (A < 0.0f) { A = -A; Sign = -1.0f; }
  Num = 16.0f * A * (Pi - A);
  Den = 5.0f * Pi * Pi - 4.0f * A * (Pi - A);
  return Sign * (Num / Den);
}

static f32 Cos32 (f32 A) {
  return Sin32 (A + 1.57079633f);
}

/* ------------------------------------------------------------------ *
 * Paint state (cascaded through <g> / element nesting).
 * ------------------------------------------------------------------ */
typedef struct {
  u8  R, G, B;
  b32 Set;    ///< DFBR_TRUE once a parent or self assigned a concrete color.
  b32 None;   ///< DFBR_TRUE means "paint is explicitly `none`" (skip).
} SvgColor;

typedef struct {
  f32 A, B, C, D, E, F;   ///< [a c e; b d f] so (x',y') = M * (x,y,1).
} Mat3;

typedef struct {
  SvgColor  Fill;
  SvgColor  Stroke;
  f32       StrokeWidth;
  f32       Opacity;
  Mat3      M;
} SvgState;

static void MatIdent (Mat3 *M) {
  M->A = 1.0f; M->B = 0.0f;
  M->C = 0.0f; M->D = 1.0f;
  M->E = 0.0f; M->F = 0.0f;
}

static void MatMul (Mat3 *R, const Mat3 *L, const Mat3 *Rm) {
  /* R = L * Rm, using a temporary so R can alias L or Rm. */
  Mat3 T;
  T.A = L->A * Rm->A + L->C * Rm->B;
  T.B = L->B * Rm->A + L->D * Rm->B;
  T.C = L->A * Rm->C + L->C * Rm->D;
  T.D = L->B * Rm->C + L->D * Rm->D;
  T.E = L->A * Rm->E + L->C * Rm->F + L->E;
  T.F = L->B * Rm->E + L->D * Rm->F + L->F;
  *R = T;
}

static void MatApply (const Mat3 *M, f32 X, f32 Y, f32 *Xo, f32 *Yo) {
  *Xo = M->A * X + M->C * Y + M->E;
  *Yo = M->B * X + M->D * Y + M->F;
}

/* ------------------------------------------------------------------ *
 * Named CSS colors (the subset the UEFI browser validated against real
 * logo/icon SVGs).
 * ------------------------------------------------------------------ */
typedef struct {
  const char *Name;
  u8          R, G, B;
} NamedColor;

static const NamedColor mNamedColors[] = {
  { "black",       0x00, 0x00, 0x00 },
  { "white",       0xFF, 0xFF, 0xFF },
  { "red",         0xFF, 0x00, 0x00 },
  { "green",       0x00, 0x80, 0x00 },
  { "lime",        0x00, 0xFF, 0x00 },
  { "blue",        0x00, 0x00, 0xFF },
  { "yellow",      0xFF, 0xFF, 0x00 },
  { "cyan",        0x00, 0xFF, 0xFF },
  { "aqua",        0x00, 0xFF, 0xFF },
  { "magenta",     0xFF, 0x00, 0xFF },
  { "fuchsia",     0xFF, 0x00, 0xFF },
  { "silver",      0xC0, 0xC0, 0xC0 },
  { "gray",        0x80, 0x80, 0x80 },
  { "grey",        0x80, 0x80, 0x80 },
  { "maroon",      0x80, 0x00, 0x00 },
  { "olive",       0x80, 0x80, 0x00 },
  { "purple",      0x80, 0x00, 0x80 },
  { "teal",        0x00, 0x80, 0x80 },
  { "navy",        0x00, 0x00, 0x80 },
  { "orange",      0xFF, 0xA5, 0x00 },
  { "pink",        0xFF, 0xC0, 0xCB },
  { "brown",       0xA5, 0x2A, 0x2A },
  { "transparent", 0x00, 0x00, 0x00 },
  { DFBR_NULL,     0x00, 0x00, 0x00 }
};

static b32 StrEqCiLen (const char *A, const char *B, usize Len) {
  usize I;
  for (I = 0; I < Len; I++) {
    char Ca = A[I];
    char Cb = B[I];
    if (Ca >= 'A' && Ca <= 'Z') Ca = (char)(Ca - 'A' + 'a');
    if (Cb >= 'A' && Cb <= 'Z') Cb = (char)(Cb - 'A' + 'a');
    if (Ca != Cb) return DFBR_FALSE;
  }
  return (b32)(B[Len] == '\0');
}

static b32 IsSpaceC (char C) {
  return (b32)(C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f');
}

static i32 HexDig (char C) {
  if (C >= '0' && C <= '9') return (i32)(C - '0');
  if (C >= 'a' && C <= 'f') return (i32)(C - 'a' + 10);
  if (C >= 'A' && C <= 'F') return (i32)(C - 'A' + 10);
  return -1;
}

static void SkipSep (const char **Pp) {
  const char *P = *Pp;
  while (*P != '\0' && (IsSpaceC (*P) || *P == ',')) P++;
  *Pp = P;
}

/** Parse a signed decimal (optionally exponential) number from `*Pp`.
    Returns DFBR_TRUE if any digits were consumed; advances `*Pp` past them. **/
static b32 ParseNum (const char **Pp, f32 *Out) {
  const char *P         = *Pp;
  f32         Sign      = 1.0f;
  f32         Whole     = 0.0f;
  f32         Frac      = 0.0f;
  f32         FracScale = 1.0f;
  b32         AnyDigit  = DFBR_FALSE;
  i32         Exp       = 0;
  i32         ExpSign   = 1;
  f32         Val;

  if      (*P == '+') { P++; }
  else if (*P == '-') { Sign = -1.0f; P++; }

  while (*P >= '0' && *P <= '9') {
    Whole = Whole * 10.0f + (f32)(*P - '0');
    AnyDigit = DFBR_TRUE;
    P++;
  }
  if (*P == '.') {
    P++;
    while (*P >= '0' && *P <= '9') {
      Frac = Frac * 10.0f + (f32)(*P - '0');
      FracScale *= 10.0f;
      AnyDigit = DFBR_TRUE;
      P++;
    }
  }
  if (!AnyDigit) return DFBR_FALSE;
  Val = Sign * (Whole + Frac / FracScale);
  if (*P == 'e' || *P == 'E') {
    P++;
    if      (*P == '+') { P++; }
    else if (*P == '-') { ExpSign = -1; P++; }
    while (*P >= '0' && *P <= '9') {
      Exp = Exp * 10 + (i32)(*P - '0');
      P++;
    }
    /* Exponent as repeated multiply. SVG data rarely uses large exponents. */
    {
      i32 I;
      f32 Scale = 1.0f;
      for (I = 0; I < Exp; I++) Scale *= 10.0f;
      if (ExpSign > 0) Val *= Scale;
      else             Val /= Scale;
    }
  }
  *Out = Val;
  *Pp  = P;
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------ *
 * Color parsing - #rgb, #rrggbb, rgb(r,g,b), named, "none",
 * "transparent", "currentColor".
 * ------------------------------------------------------------------ */
static void ParseColor (const char *S, SvgColor *Out) {
  usize       Len = 0;
  const char *P;
  usize       I;

  while (S[Len] != '\0') Len++;
  while (Len > 0 && IsSpaceC (S[Len - 1])) Len--;
  P = S;
  while (*P != '\0' && IsSpaceC (*P)) { P++; Len--; }

  Out->Set  = DFBR_FALSE;
  Out->None = DFBR_FALSE;
  Out->R = Out->G = Out->B = 0;

  if (Len == 0) return;

  if (Len == 4 && StrEqCiLen (P, "none", 4)) {
    Out->None = DFBR_TRUE;
    Out->Set  = DFBR_TRUE;
    return;
  }
  if (Len == 11 && StrEqCiLen (P, "transparent", 11)) {
    Out->None = DFBR_TRUE;
    Out->Set  = DFBR_TRUE;
    return;
  }
  if (Len == 12 && StrEqCiLen (P, "currentColor", 12)) {
    /* No host `color` property plumbing; fall back to black to match
     * the historical default. */
    Out->R = 0; Out->G = 0; Out->B = 0; Out->Set = DFBR_TRUE;
    return;
  }

  if (*P == '#') {
    const char *H  = P + 1;
    usize       Hl = Len - 1;
    i32         V[6];
    if (Hl == 3) {
      for (I = 0; I < 3; I++) {
        V[I] = HexDig (H[I]);
        if (V[I] < 0) return;
      }
      Out->R = (u8)((V[0] * 17) & 0xFF);
      Out->G = (u8)((V[1] * 17) & 0xFF);
      Out->B = (u8)((V[2] * 17) & 0xFF);
      Out->Set = DFBR_TRUE;
      return;
    } else if (Hl == 6) {
      for (I = 0; I < 6; I++) {
        V[I] = HexDig (H[I]);
        if (V[I] < 0) return;
      }
      Out->R = (u8)(((V[0] << 4) | V[1]) & 0xFF);
      Out->G = (u8)(((V[2] << 4) | V[3]) & 0xFF);
      Out->B = (u8)(((V[4] << 4) | V[5]) & 0xFF);
      Out->Set = DFBR_TRUE;
      return;
    }
    return;
  }

  if (Len > 4 && StrEqCiLen (P, "rgb", 3)) {
    const char *Q = P + 3;
    f32 R = 0, G = 0, B = 0;
    while (*Q != '\0' && (IsSpaceC (*Q) || *Q == '(')) Q++;
    SkipSep (&Q);
    if (!ParseNum (&Q, &R)) return;
    SkipSep (&Q);
    if (!ParseNum (&Q, &G)) return;
    SkipSep (&Q);
    if (!ParseNum (&Q, &B)) return;
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    Out->R = (u8)R; Out->G = (u8)G; Out->B = (u8)B;
    Out->Set = DFBR_TRUE;
    return;
  }

  for (I = 0; mNamedColors[I].Name != DFBR_NULL; I++) {
    usize Nl = 0;
    while (mNamedColors[I].Name[Nl] != '\0') Nl++;
    if (Nl == Len && StrEqCiLen (P, mNamedColors[I].Name, Len)) {
      Out->R = mNamedColors[I].R;
      Out->G = mNamedColors[I].G;
      Out->B = mNamedColors[I].B;
      Out->Set = DFBR_TRUE;
      if (Nl == 11 && StrEqCiLen ("transparent", mNamedColors[I].Name, 11)) {
        Out->None = DFBR_TRUE;
      }
      return;
    }
  }
}

/* ------------------------------------------------------------------ *
 * transform="..." parser.
 * ------------------------------------------------------------------ */
static void ParseTransformInto (const char *Str, Mat3 *M) {
  const char *P = Str;
  while (*P != '\0') {
    const char *N;
    usize NameLen;
    f32   V[6] = { 0, 0, 0, 0, 0, 0 };
    usize Nv   = 0;
    Mat3  X;

    while (*P != '\0' && IsSpaceC (*P)) P++;
    if (*P == '\0') break;

    N = P;
    while (*P != '\0' && *P != '(' && !IsSpaceC (*P) && *P != ',') P++;
    NameLen = (usize)(P - N);
    while (*P != '\0' && (IsSpaceC (*P) || *P == ',')) P++;
    if (*P != '(') break;
    P++;

    while (*P != '\0' && *P != ')' && Nv < 6) {
      f32 Tmp = 0;
      SkipSep (&P);
      if (*P == ')') break;
      if (!ParseNum (&P, &Tmp)) break;
      V[Nv++] = Tmp;
    }
    while (*P != '\0' && *P != ')') P++;
    if (*P == ')') P++;

    MatIdent (&X);
    if (NameLen == 9 && StrEqCiLen (N, "translate", 9)) {
      X.E = V[0];
      X.F = (Nv >= 2) ? V[1] : 0.0f;
    } else if (NameLen == 5 && StrEqCiLen (N, "scale", 5)) {
      X.A = V[0];
      X.D = (Nv >= 2) ? V[1] : V[0];
    } else if (NameLen == 6 && StrEqCiLen (N, "rotate", 6)) {
      f32 Deg = V[0];
      f32 Rad = Deg * 3.14159265f / 180.0f;
      f32 Cs  = Cos32 (Rad);
      f32 Sn  = Sin32 (Rad);
      if (Nv >= 3) {
        /* rotate(angle cx cy) = translate(cx,cy) rotate(angle) translate(-cx,-cy) */
        Mat3 T1, Rot, T2, Tmp;
        MatIdent (&T1);  T1.E =  V[1]; T1.F =  V[2];
        MatIdent (&Rot); Rot.A = Cs; Rot.B = Sn; Rot.C = -Sn; Rot.D = Cs;
        MatIdent (&T2);  T2.E = -V[1]; T2.F = -V[2];
        MatMul (&Tmp, &T1, &Rot);
        MatMul (&X,   &Tmp, &T2);
      } else {
        X.A = Cs; X.B = Sn; X.C = -Sn; X.D = Cs;
      }
    } else if (NameLen == 6 && StrEqCiLen (N, "matrix", 6) && Nv >= 6) {
      X.A = V[0]; X.B = V[1]; X.C = V[2];
      X.D = V[3]; X.E = V[4]; X.F = V[5];
    } else if (NameLen == 5 && StrEqCiLen (N, "skewX", 5)) {
      f32 Rr = V[0] * 3.14159265f / 180.0f;
      f32 Cc = Cos32 (Rr);
      if (Cc != 0.0f) X.C = Sin32 (Rr) / Cc;
    } else if (NameLen == 5 && StrEqCiLen (N, "skewY", 5)) {
      f32 Rr = V[0] * 3.14159265f / 180.0f;
      f32 Cc = Cos32 (Rr);
      if (Cc != 0.0f) X.B = Sin32 (Rr) / Cc;
    } else {
      continue;
    }
    MatMul (M, M, &X);
  }
}

/* ------------------------------------------------------------------ *
 * Polyline buffer (subpath aware).
 * ------------------------------------------------------------------ */
typedef struct { f32 X; f32 Y; } Vec2;

typedef struct {
  Vec2   *Pts;
  usize   PtCap;
  usize   PtCount;

  usize  *Breaks;
  b32    *Closed;
  usize   SubCap;
  usize   SubCount;

  b32     InSub;
} Poly;

static void PolyInit (Poly *P) { memset (P, 0, sizeof (*P)); }

static void PolyFree (Poly *P) {
  if (P->Pts    != DFBR_NULL) SvgFree (P->Pts);
  if (P->Breaks != DFBR_NULL) SvgFree (P->Breaks);
  if (P->Closed != DFBR_NULL) SvgFree (P->Closed);
  memset (P, 0, sizeof (*P));
}

static b32 PolyReserveSub (Poly *P, usize More) {
  if (P->SubCount + More + 1 > P->SubCap) {
    usize  Nc = (P->SubCap == 0) ? 16 : P->SubCap * 2;
    usize *Nb;
    b32   *Nc2;
    while (Nc < P->SubCount + More + 1) Nc *= 2;
    Nb  = (usize *)SvgAlloc (Nc * sizeof (usize));
    Nc2 = (b32   *)SvgAlloc (Nc * sizeof (b32));
    if (Nb == DFBR_NULL || Nc2 == DFBR_NULL) {
      if (Nb  != DFBR_NULL) SvgFree (Nb);
      if (Nc2 != DFBR_NULL) SvgFree (Nc2);
      return DFBR_FALSE;
    }
    if (P->Breaks != DFBR_NULL) {
      memcpy (Nb,  P->Breaks, P->SubCount * sizeof (usize));
      memcpy (Nc2, P->Closed, P->SubCount * sizeof (b32));
      SvgFree (P->Breaks);
      SvgFree (P->Closed);
    }
    P->Breaks = Nb;
    P->Closed = Nc2;
    P->SubCap = Nc;
  }
  return DFBR_TRUE;
}

static b32 PolyReservePts (Poly *P, usize More) {
  if (P->PtCount + More > P->PtCap) {
    usize Nc = (P->PtCap == 0) ? 32 : P->PtCap * 2;
    Vec2 *Nv;
    while (Nc < P->PtCount + More) Nc *= 2;
    Nv = (Vec2 *)SvgAlloc (Nc * sizeof (Vec2));
    if (Nv == DFBR_NULL) return DFBR_FALSE;
    if (P->Pts != DFBR_NULL) {
      memcpy (Nv, P->Pts, P->PtCount * sizeof (Vec2));
      SvgFree (P->Pts);
    }
    P->Pts   = Nv;
    P->PtCap = Nc;
  }
  return DFBR_TRUE;
}

static void PolyBegin (Poly *P) {
  if (!PolyReserveSub (P, 1)) return;
  P->Breaks[P->SubCount] = P->PtCount;
  P->Closed[P->SubCount] = DFBR_FALSE;
  P->SubCount++;
  P->InSub = DFBR_TRUE;
}

static void PolyEnd (Poly *P, b32 Closed) {
  usize Start;
  if (!P->InSub) return;
  Start = P->Breaks[P->SubCount - 1];
  if (P->PtCount <= Start) {
    P->SubCount--;
    P->InSub = DFBR_FALSE;
    return;
  }
  P->Closed[P->SubCount - 1] = Closed;
  P->InSub = DFBR_FALSE;
}

static void PolyAdd (Poly *P, f32 X, f32 Y) {
  if (!PolyReservePts (P, 1)) return;
  P->Pts[P->PtCount].X = X;
  P->Pts[P->PtCount].Y = Y;
  P->PtCount++;
}

static void PolyMoveTo (Poly *P, f32 X, f32 Y) {
  PolyEnd (P, DFBR_FALSE);
  PolyBegin (P);
  PolyAdd (P, X, Y);
}

static void PolyLineTo (Poly *P, f32 X, f32 Y) {
  if (!P->InSub) PolyBegin (P);
  PolyAdd (P, X, Y);
}

/* ------------------------------------------------------------------ *
 * Bezier flattening.
 * ------------------------------------------------------------------ */
static void FlattenCubic (
  Poly *P,
  f32 X0, f32 Y0, f32 X1, f32 Y1,
  f32 X2, f32 Y2, f32 X3, f32 Y3,
  u32 Depth
  )
{
  f32 Dx  = X3 - X0;
  f32 Dy  = Y3 - Y0;
  f32 D1  = Fabs32 ((X1 - X3) * Dy - (Y1 - Y3) * Dx);
  f32 D2  = Fabs32 ((X2 - X3) * Dy - (Y2 - Y3) * Dx);
  f32 Tol = 0.5f * Sqrt32 (Dx * Dx + Dy * Dy);
  f32 X01, Y01, X12, Y12, X23, Y23, Xa, Ya, Xb, Yb, Xc, Yc;
  if (Tol < 0.5f) Tol = 0.5f;
  if (Depth >= 12 || (D1 + D2) <= Tol) {
    PolyLineTo (P, X3, Y3);
    return;
  }
  X01 = 0.5f * (X0 + X1); Y01 = 0.5f * (Y0 + Y1);
  X12 = 0.5f * (X1 + X2); Y12 = 0.5f * (Y1 + Y2);
  X23 = 0.5f * (X2 + X3); Y23 = 0.5f * (Y2 + Y3);
  Xa  = 0.5f * (X01 + X12); Ya  = 0.5f * (Y01 + Y12);
  Xb  = 0.5f * (X12 + X23); Yb  = 0.5f * (Y12 + Y23);
  Xc  = 0.5f * (Xa  + Xb);  Yc  = 0.5f * (Ya  + Yb);
  FlattenCubic (P, X0, Y0, X01, Y01, Xa,  Ya,  Xc, Yc, Depth + 1);
  FlattenCubic (P, Xc, Yc, Xb,  Yb,  X23, Y23, X3, Y3, Depth + 1);
}

static void FlattenQuad (Poly *P, f32 X0, f32 Y0, f32 X1, f32 Y1, f32 X2, f32 Y2) {
  /* Promote quadratic to cubic and reuse the cubic flattener. */
  f32 Cx1 = X0 + (2.0f / 3.0f) * (X1 - X0);
  f32 Cy1 = Y0 + (2.0f / 3.0f) * (Y1 - Y0);
  f32 Cx2 = X2 + (2.0f / 3.0f) * (X1 - X2);
  f32 Cy2 = Y2 + (2.0f / 3.0f) * (Y1 - Y2);
  FlattenCubic (P, X0, Y0, Cx1, Cy1, Cx2, Cy2, X2, Y2, 0);
}

/* ------------------------------------------------------------------ *
 * Path "d" parser. Produces a flattened polyline via the Poly buffer.
 * ------------------------------------------------------------------ */
static void ParsePathD (const char *D, Poly *P) {
  const char *Q   = D;
  f32   Cx  = 0, Cy  = 0;
  f32   Sx  = 0, Sy  = 0;
  f32   Pcx = 0, Pcy = 0;
  char  Prev = 0;
  char  Cmd  = 0;

  while (*Q != '\0') {
    char C;
    b32  Rel;
    char U;
    f32  X, Y, X1, Y1, X2, Y2, Rx, Ry, Rot, Lg, Sw;

    SkipSep (&Q);
    if (*Q == '\0') break;

    C = *Q;
    if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z')) {
      Cmd = C;
      Q++;
      SkipSep (&Q);
    }

    Rel = (b32)(Cmd >= 'a' && Cmd <= 'z');
    U   = Cmd;
    if (U >= 'a' && U <= 'z') U = (char)(U - 'a' + 'A');

    (void)Rx; (void)Ry; (void)Rot; (void)Lg; (void)Sw;

    switch (U) {
      case 'M':
        if (!ParseNum (&Q, &X)) return;
        SkipSep (&Q);
        if (!ParseNum (&Q, &Y)) return;
        if (Rel) { X += Cx; Y += Cy; }
        PolyMoveTo (P, X, Y);
        Cx = X; Cy = Y; Sx = X; Sy = Y;
        Cmd  = Rel ? 'l' : 'L';
        Prev = U;
        Pcx  = Cx; Pcy = Cy;
        break;

      case 'L':
        if (!ParseNum (&Q, &X)) return;
        SkipSep (&Q);
        if (!ParseNum (&Q, &Y)) return;
        if (Rel) { X += Cx; Y += Cy; }
        PolyLineTo (P, X, Y);
        Cx = X; Cy = Y;
        Prev = U;
        Pcx  = Cx; Pcy = Cy;
        break;

      case 'H':
        if (!ParseNum (&Q, &X)) return;
        if (Rel) X += Cx;
        PolyLineTo (P, X, Cy);
        Cx = X;
        Prev = U;
        Pcx  = Cx; Pcy = Cy;
        break;

      case 'V':
        if (!ParseNum (&Q, &Y)) return;
        if (Rel) Y += Cy;
        PolyLineTo (P, Cx, Y);
        Cy = Y;
        Prev = U;
        Pcx  = Cx; Pcy = Cy;
        break;

      case 'Z':
        PolyLineTo (P, Sx, Sy);
        PolyEnd (P, DFBR_TRUE);
        Cx = Sx; Cy = Sy;
        Prev = U;
        Pcx  = Cx; Pcy = Cy;
        Cmd = 0;
        break;

      case 'C':
        if (!ParseNum (&Q, &X1)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y1)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &X2)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y2)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &X )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y )) return;
        if (Rel) { X1 += Cx; Y1 += Cy; X2 += Cx; Y2 += Cy; X += Cx; Y += Cy; }
        if (!P->InSub) PolyBegin (P);
        if (P->PtCount == P->Breaks[P->SubCount - 1]) PolyAdd (P, Cx, Cy);
        FlattenCubic (P, Cx, Cy, X1, Y1, X2, Y2, X, Y, 0);
        Cx = X; Cy = Y;
        Pcx = X2; Pcy = Y2;
        Prev = U;
        break;

      case 'S':
        if (!ParseNum (&Q, &X2)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y2)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &X )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y )) return;
        if (Rel) { X2 += Cx; Y2 += Cy; X += Cx; Y += Cy; }
        if (Prev == 'C' || Prev == 'S') {
          X1 = 2.0f * Cx - Pcx; Y1 = 2.0f * Cy - Pcy;
        } else {
          X1 = Cx; Y1 = Cy;
        }
        if (!P->InSub) PolyBegin (P);
        if (P->PtCount == P->Breaks[P->SubCount - 1]) PolyAdd (P, Cx, Cy);
        FlattenCubic (P, Cx, Cy, X1, Y1, X2, Y2, X, Y, 0);
        Cx = X; Cy = Y;
        Pcx = X2; Pcy = Y2;
        Prev = U;
        break;

      case 'Q':
        if (!ParseNum (&Q, &X1)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y1)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &X )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y )) return;
        if (Rel) { X1 += Cx; Y1 += Cy; X += Cx; Y += Cy; }
        if (!P->InSub) PolyBegin (P);
        if (P->PtCount == P->Breaks[P->SubCount - 1]) PolyAdd (P, Cx, Cy);
        FlattenQuad (P, Cx, Cy, X1, Y1, X, Y);
        Cx = X; Cy = Y;
        Pcx = X1; Pcy = Y1;
        Prev = U;
        break;

      case 'T':
        if (!ParseNum (&Q, &X)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y)) return;
        if (Rel) { X += Cx; Y += Cy; }
        if (Prev == 'Q' || Prev == 'T') {
          X1 = 2.0f * Cx - Pcx; Y1 = 2.0f * Cy - Pcy;
        } else {
          X1 = Cx; Y1 = Cy;
        }
        if (!P->InSub) PolyBegin (P);
        if (P->PtCount == P->Breaks[P->SubCount - 1]) PolyAdd (P, Cx, Cy);
        FlattenQuad (P, Cx, Cy, X1, Y1, X, Y);
        Cx = X; Cy = Y;
        Pcx = X1; Pcy = Y1;
        Prev = U;
        break;

      case 'A':
        /* Elliptical arc: a full implementation would compute the
         * center from endpoints + rx/ry/rot/largeArc/sweep. We
         * approximate with a straight line - visible loss only on
         * logos that use arcs, which is uncommon. */
        if (!ParseNum (&Q, &Rx )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Ry )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Rot)) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Lg )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Sw )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &X  )) return; SkipSep (&Q);
        if (!ParseNum (&Q, &Y  )) return;
        if (Rel) { X += Cx; Y += Cy; }
        PolyLineTo (P, X, Y);
        Cx = X; Cy = Y;
        Pcx = Cx; Pcy = Cy;
        Prev = U;
        break;

      default:
        /* Unknown command: bail to avoid an infinite loop on
         * malformed data. */
        return;
    }
  }
  PolyEnd (P, DFBR_FALSE);
}

/* ------------------------------------------------------------------ *
 * Rasterizer.
 * ------------------------------------------------------------------ */
typedef struct {
  DfBrColor *Pixels;
  u32        W;
  u32        H;
} Rast;

static void RastBlend (Rast *R, i32 X, i32 Y, u8 Cr, u8 Cg, u8 Cb, u8 A) {
  DfBrColor *Px;
  if (X < 0 || Y < 0 || (u32)X >= R->W || (u32)Y >= R->H) return;
  if (A == 0) return;
  Px = &R->Pixels[(usize)Y * R->W + (usize)X];
  if (A == 255 || Px->A == 0) {
    Px->B = Cb;
    Px->G = Cg;
    Px->R = Cr;
    if (A > Px->A) Px->A = A;
    return;
  }
  {
    u32 A1 = (u32)A;
    u32 Aa = 255 - A1;
    Px->B = (u8)((A1 * Cb + Aa * Px->B) / 255);
    Px->G = (u8)((A1 * Cg + Aa * Px->G) / 255);
    Px->R = (u8)((A1 * Cr + Aa * Px->R) / 255);
    if (A > Px->A) Px->A = A;
  }
}

static void RastSpan (Rast *R, i32 X0, i32 X1, i32 Y, u8 Cr, u8 Cg, u8 Cb, u8 A) {
  i32 X;
  if (X0 > X1) { X = X0; X0 = X1; X1 = X; }
  for (X = X0; X < X1; X++) RastBlend (R, X, Y, Cr, Cg, Cb, A);
}

/* ------------------------------------------------------------------ *
 * Polygon fill (even-odd crossing count).
 * ------------------------------------------------------------------ */
typedef struct {
  f32 YMin, YMax;
  f32 X, Slope;
} Edge;

static void SortF (f32 *A, usize N) {
  usize I, J;
  for (I = 1; I < N; I++) {
    f32 V = A[I];
    J = I;
    while (J > 0 && A[J - 1] > V) {
      A[J] = A[J - 1];
      J--;
    }
    A[J] = V;
  }
}

#define MAX_CROSS 64

static void FillPoly (
  Rast *R, const Mat3 *M, const Poly *P,
  u8 Cr, u8 Cg, u8 Cb, u8 Alpha
  )
{
  usize  Sub;
  Vec2  *Tp;
  f32    YMinF = 1.0e30f, YMaxF = -1.0e30f;
  usize  TotalEdges = 0;
  Edge  *Edges;
  usize  Ne = 0;
  i32    ScanMin, ScanMax;
  f32    Cross[MAX_CROSS];
  i32    Y;
  usize  I;

  if (P->SubCount == 0 || P->PtCount == 0) return;

  Tp = (Vec2 *)SvgAlloc (P->PtCount * sizeof (Vec2));
  if (Tp == DFBR_NULL) return;
  for (I = 0; I < P->PtCount; I++) {
    f32 Xo, Yo;
    MatApply (M, P->Pts[I].X, P->Pts[I].Y, &Xo, &Yo);
    Tp[I].X = Xo;
    Tp[I].Y = Yo;
    if (Yo < YMinF) YMinF = Yo;
    if (Yo > YMaxF) YMaxF = Yo;
  }

  for (Sub = 0; Sub < P->SubCount; Sub++) {
    usize S = P->Breaks[Sub];
    usize E = (Sub + 1 < P->SubCount) ? P->Breaks[Sub + 1] : P->PtCount;
    if (E - S >= 2) TotalEdges += (E - S);
  }
  if (TotalEdges == 0) { SvgFree (Tp); return; }
  Edges = (Edge *)SvgAlloc (TotalEdges * sizeof (Edge));
  if (Edges == DFBR_NULL) { SvgFree (Tp); return; }

  for (Sub = 0; Sub < P->SubCount; Sub++) {
    usize S = P->Breaks[Sub];
    usize E = (Sub + 1 < P->SubCount) ? P->Breaks[Sub + 1] : P->PtCount;
    usize K;
    if (E - S < 2) continue;
    for (K = S; K < E; K++) {
      usize Kn = (K + 1 < E) ? (K + 1) : S;
      f32   X0 = Tp[K].X,  Y0 = Tp[K].Y;
      f32   X1 = Tp[Kn].X, Y1 = Tp[Kn].Y;
      Edge *Ed;
      if (Y0 == Y1) continue;
      Ed = &Edges[Ne++];
      if (Y0 < Y1) {
        Ed->YMin = Y0; Ed->YMax = Y1;
        Ed->Slope = (X1 - X0) / (Y1 - Y0);
        Ed->X     = X0;
      } else {
        Ed->YMin = Y1; Ed->YMax = Y0;
        Ed->Slope = (X0 - X1) / (Y0 - Y1);
        Ed->X     = X1;
      }
    }
  }

  ScanMin = (i32)YMinF;
  ScanMax = (i32)YMaxF;
  if (ScanMin < 0)          ScanMin = 0;
  if (ScanMax > (i32)R->H)  ScanMax = (i32)R->H;

  for (Y = ScanMin; Y < ScanMax; Y++) {
    f32   Fy = (f32)Y + 0.5f;
    usize Nc = 0;
    usize K;
    for (I = 0; I < Ne; I++) {
      Edge *Ed = &Edges[I];
      if (Fy < Ed->YMin || Fy >= Ed->YMax) continue;
      if (Nc >= MAX_CROSS) break;
      Cross[Nc++] = Ed->X + Ed->Slope * (Fy - Ed->YMin);
    }
    if (Nc < 2) continue;
    SortF (Cross, Nc);
    for (K = 0; K + 1 < Nc; K += 2) {
      i32 Xa = (i32)Cross[K];
      i32 Xb = (i32)(Cross[K + 1] + 0.999f);
      if (Xa == Xb && Cross[K + 1] > Cross[K]) Xb = Xa + 1;
      RastSpan (R, Xa, Xb, Y, Cr, Cg, Cb, Alpha);
    }
  }
  SvgFree (Edges);
  SvgFree (Tp);
}

/* ------------------------------------------------------------------ *
 * Stroke: each segment -> oriented rectangle -> filled as a poly.
 * No miter/bevel joins; adjacent thick segments overlap slightly but
 * that's rarely visible at our target icon resolutions.
 * ------------------------------------------------------------------ */
static void StrokeSegment (
  Rast *R, const Mat3 *M,
  f32 X0, f32 Y0, f32 X1, f32 Y1,
  f32 Width, u8 Cr, u8 Cg, u8 Cb, u8 Alpha
  )
{
  f32 Dx  = X1 - X0;
  f32 Dy  = Y1 - Y0;
  f32 Len = Sqrt32 (Dx * Dx + Dy * Dy);
  f32 Nx, Ny;
  Poly Q;
  if (Len < 0.001f) return;
  Nx = -Dy / Len * (Width * 0.5f);
  Ny =  Dx / Len * (Width * 0.5f);

  PolyInit (&Q);
  PolyMoveTo (&Q, X0 + Nx, Y0 + Ny);
  PolyLineTo (&Q, X1 + Nx, Y1 + Ny);
  PolyLineTo (&Q, X1 - Nx, Y1 - Ny);
  PolyLineTo (&Q, X0 - Nx, Y0 - Ny);
  PolyEnd    (&Q, DFBR_TRUE);
  FillPoly (R, M, &Q, Cr, Cg, Cb, Alpha);
  PolyFree (&Q);
}

static void StrokePoly (
  Rast *R, const Mat3 *M, const Poly *P,
  f32 Width, u8 Cr, u8 Cg, u8 Cb, u8 Alpha
  )
{
  usize Sub;
  if (Width <= 0.0f) return;
  for (Sub = 0; Sub < P->SubCount; Sub++) {
    usize S = P->Breaks[Sub];
    usize E = (Sub + 1 < P->SubCount) ? P->Breaks[Sub + 1] : P->PtCount;
    usize K;
    if (E - S < 2) continue;
    for (K = S; K + 1 < E; K++) {
      StrokeSegment (R, M,
                     P->Pts[K].X,     P->Pts[K].Y,
                     P->Pts[K + 1].X, P->Pts[K + 1].Y,
                     Width, Cr, Cg, Cb, Alpha);
    }
    if (P->Closed[Sub] && E - S >= 3) {
      StrokeSegment (R, M,
                     P->Pts[E - 1].X, P->Pts[E - 1].Y,
                     P->Pts[S].X,     P->Pts[S].Y,
                     Width, Cr, Cg, Cb, Alpha);
    }
  }
}

static void DrawPoly (Rast *R, const SvgState *St, const Poly *P) {
  u8 Alpha = (u8)(St->Opacity * 255.0f);
  if (Alpha == 0) return;
  if (St->Fill.Set && !St->Fill.None) {
    FillPoly (R, &St->M, P, St->Fill.R, St->Fill.G, St->Fill.B, Alpha);
  }
  if (St->Stroke.Set && !St->Stroke.None && St->StrokeWidth > 0.0f) {
    StrokePoly (R, &St->M, P, St->StrokeWidth,
                St->Stroke.R, St->Stroke.G, St->Stroke.B, Alpha);
  }
}

/* ------------------------------------------------------------------ *
 * Shape emitters.
 * ------------------------------------------------------------------ */
static void EmitRect (Rast *R, const SvgState *St, f32 X, f32 Y, f32 W, f32 H) {
  Poly P;
  if (W <= 0 || H <= 0) return;
  PolyInit (&P);
  PolyMoveTo (&P, X,     Y);
  PolyLineTo (&P, X + W, Y);
  PolyLineTo (&P, X + W, Y + H);
  PolyLineTo (&P, X,     Y + H);
  PolyEnd    (&P, DFBR_TRUE);
  DrawPoly (R, St, &P);
  PolyFree (&P);
}

static void EmitEllipse (Rast *R, const SvgState *St, f32 Cx, f32 Cy, f32 Rx, f32 Ry) {
  Poly  P;
  usize N    = 48;
  f32   Step = 6.28318531f / (f32)N;
  usize I;
  if (Rx <= 0 || Ry <= 0) return;
  PolyInit (&P);
  for (I = 0; I < N; I++) {
    f32 A = (f32)I * Step;
    f32 X = Cx + Rx * Cos32 (A);
    f32 Y = Cy + Ry * Sin32 (A);
    if (I == 0) PolyMoveTo (&P, X, Y);
    else        PolyLineTo (&P, X, Y);
  }
  PolyEnd (&P, DFBR_TRUE);
  DrawPoly (R, St, &P);
  PolyFree (&P);
}

static void EmitLine (Rast *R, const SvgState *St, f32 X1, f32 Y1, f32 X2, f32 Y2) {
  Poly     P;
  SvgState S2;
  PolyInit (&P);
  PolyMoveTo (&P, X1, Y1);
  PolyLineTo (&P, X2, Y2);
  PolyEnd    (&P, DFBR_FALSE);
  S2 = *St;
  memset (&S2.Fill, 0, sizeof (S2.Fill));
  DrawPoly (R, &S2, &P);
  PolyFree (&P);
}

static void EmitPointList (Rast *R, const SvgState *St, const char *Points, b32 Closed) {
  Poly        P;
  const char *Q = Points;
  b32         First = DFBR_TRUE;
  PolyInit (&P);
  while (*Q != '\0') {
    f32 X, Y;
    SkipSep (&Q);
    if (*Q == '\0') break;
    if (!ParseNum (&Q, &X)) break;
    SkipSep (&Q);
    if (!ParseNum (&Q, &Y)) break;
    if (First) { PolyMoveTo (&P, X, Y); First = DFBR_FALSE; }
    else         PolyLineTo (&P, X, Y);
  }
  PolyEnd (&P, Closed);
  DrawPoly (R, St, &P);
  PolyFree (&P);
}

static void EmitPath (Rast *R, const SvgState *St, const char *D) {
  Poly P;
  PolyInit (&P);
  ParsePathD (D, &P);
  DrawPoly (R, St, &P);
  PolyFree (&P);
}

/* ------------------------------------------------------------------ *
 * Mini XML scanner + recursive descent.
 * ------------------------------------------------------------------ */
typedef struct {
  const char *Src;
  usize       Pos;
  usize       Len;
  Rast       *R;
} Sx;

static void SxSkipSpace (Sx *X) {
  while (X->Pos < X->Len && IsSpaceC (X->Src[X->Pos])) X->Pos++;
}

static b32 SxSkipComment (Sx *X) {
  if (X->Pos + 4 > X->Len) return DFBR_FALSE;
  if (X->Src[X->Pos]     != '<') return DFBR_FALSE;
  if (X->Src[X->Pos + 1] != '!') return DFBR_FALSE;
  if (X->Src[X->Pos + 2] != '-') return DFBR_FALSE;
  if (X->Src[X->Pos + 3] != '-') return DFBR_FALSE;
  X->Pos += 4;
  while (X->Pos + 2 < X->Len) {
    if (X->Src[X->Pos] == '-' && X->Src[X->Pos + 1] == '-' && X->Src[X->Pos + 2] == '>') {
      X->Pos += 3;
      return DFBR_TRUE;
    }
    X->Pos++;
  }
  X->Pos = X->Len;
  return DFBR_TRUE;
}

static b32 SxSkipPiOrDoctype (Sx *X) {
  if (X->Pos + 2 > X->Len) return DFBR_FALSE;
  if (X->Src[X->Pos]     != '<') return DFBR_FALSE;
  if (X->Src[X->Pos + 1] != '?' && X->Src[X->Pos + 1] != '!') return DFBR_FALSE;
  X->Pos += 2;
  while (X->Pos < X->Len && X->Src[X->Pos] != '>') X->Pos++;
  if (X->Pos < X->Len) X->Pos++;
  return DFBR_TRUE;
}

static b32 SxReadName (Sx *X, char *Buf, usize BufCap) {
  usize I = 0;
  while (X->Pos < X->Len) {
    char C = X->Src[X->Pos];
    if (IsSpaceC (C) || C == '>' || C == '/' || C == '=') break;
    if (I + 1 < BufCap) Buf[I++] = C;
    X->Pos++;
  }
  Buf[I] = '\0';
  return (b32)(I > 0);
}

static b32 SxReadAttrValue (Sx *X, char **Out) {
  char  Quote;
  usize Start, End, L, K, Si, Di;
  char *B;

  SxSkipSpace (X);
  if (X->Pos >= X->Len) return DFBR_FALSE;
  Quote = X->Src[X->Pos];
  if (Quote != '"' && Quote != '\'') return DFBR_FALSE;
  X->Pos++;
  Start = X->Pos;
  while (X->Pos < X->Len && X->Src[X->Pos] != Quote) X->Pos++;
  End = X->Pos;
  if (X->Pos < X->Len) X->Pos++;
  L = End - Start;
  B = (char *)SvgAlloc (L + 1);
  if (B == DFBR_NULL) return DFBR_FALSE;
  for (K = 0; K < L; K++) B[K] = X->Src[Start + K];
  B[L] = '\0';

  /* Resolve the XML entities commonly seen in SVG attribute values. */
  Si = 0; Di = 0;
  while (Si < L) {
    if (B[Si] == '&') {
      if (Si + 4 < L && B[Si+1]=='a' && B[Si+2]=='m' && B[Si+3]=='p' && B[Si+4]==';') { B[Di++] = '&';  Si += 5; continue; }
      if (Si + 3 < L && B[Si+1]=='l' && B[Si+2]=='t' && B[Si+3]==';')                  { B[Di++] = '<';  Si += 4; continue; }
      if (Si + 3 < L && B[Si+1]=='g' && B[Si+2]=='t' && B[Si+3]==';')                  { B[Di++] = '>';  Si += 4; continue; }
      if (Si + 5 < L && B[Si+1]=='q' && B[Si+2]=='u' && B[Si+3]=='o' && B[Si+4]=='t' && B[Si+5]==';') { B[Di++] = '"';  Si += 6; continue; }
      if (Si + 5 < L && B[Si+1]=='a' && B[Si+2]=='p' && B[Si+3]=='o' && B[Si+4]=='s' && B[Si+5]==';') { B[Di++] = '\''; Si += 6; continue; }
    }
    B[Di++] = B[Si++];
  }
  B[Di] = '\0';
  *Out = B;
  return DFBR_TRUE;
}

static void ApplyAttr (SvgState *St, const char *Name, const char *Val) {
  if (strcmp (Name, "fill") == 0) {
    ParseColor (Val, &St->Fill);
  } else if (strcmp (Name, "stroke") == 0) {
    ParseColor (Val, &St->Stroke);
  } else if (strcmp (Name, "stroke-width") == 0) {
    const char *P = Val;
    f32         W = 0;
    if (ParseNum (&P, &W) && W > 0) St->StrokeWidth = W;
  } else if (strcmp (Name, "opacity") == 0 ||
             strcmp (Name, "fill-opacity") == 0 ||
             strcmp (Name, "stroke-opacity") == 0) {
    const char *P = Val;
    f32         O = 1.0f;
    if (ParseNum (&P, &O)) {
      if (O < 0) O = 0; if (O > 1) O = 1;
      St->Opacity = O;
    }
  } else if (strcmp (Name, "transform") == 0) {
    ParseTransformInto (Val, &St->M);
  } else if (strcmp (Name, "style") == 0) {
    /* Tiny CSS declaration-list parser that recurses back into
     * ApplyAttr so property-name dispatch stays in one place. */
    const char *P = Val;
    while (*P != '\0') {
      const char *Ns, *Vs;
      usize Nl, Vl, I;
      char  PropBuf[32];
      char  ValBuf[256];

      while (*P != '\0' && (IsSpaceC (*P) || *P == ';')) P++;
      if (*P == '\0') break;
      Ns = P;
      while (*P != '\0' && *P != ':' && *P != ';') P++;
      Nl = (usize)(P - Ns);
      while (Nl > 0 && IsSpaceC (Ns[Nl - 1])) Nl--;
      if (*P != ':') { while (*P != '\0' && *P != ';') P++; continue; }
      P++;
      while (*P != '\0' && IsSpaceC (*P)) P++;
      Vs = P;
      while (*P != '\0' && *P != ';') P++;
      Vl = (usize)(P - Vs);
      while (Vl > 0 && IsSpaceC (Vs[Vl - 1])) Vl--;
      if (Nl >= sizeof (PropBuf)) continue;
      if (Vl >= sizeof (ValBuf))  continue;
      for (I = 0; I < Nl; I++) PropBuf[I] = Ns[I];
      PropBuf[Nl] = '\0';
      for (I = 0; I < Vl; I++) ValBuf[I] = Vs[I];
      ValBuf[Vl] = '\0';
      ApplyAttr (St, PropBuf, ValBuf);
    }
  }
}

static void SxWalkElement (Sx *X, const SvgState *Parent);

static void SxHandleElement (Sx *X, const char *Tag, const SvgState *Parent) {
  SvgState St = *Parent;

  f32   Ax = 0, Ay = 0, Aw = 0, Ah = 0, Acx = 0, Acy = 0, Ar = 0;
  f32   Arx = 0, Ary = 0, Ax2 = 0, Ay2 = 0;
  char *Points = DFBR_NULL;
  char *PathD  = DFBR_NULL;
  b32   SelfClose = DFBR_FALSE;
  b32   IsContainer;

  while (X->Pos < X->Len) {
    char        Ch;
    char        Name[48];
    char       *Val;
    const char *Vp;

    SxSkipSpace (X);
    if (X->Pos >= X->Len) break;
    Ch = X->Src[X->Pos];
    if (Ch == '/') { SelfClose = DFBR_TRUE; X->Pos++; continue; }
    if (Ch == '>') { X->Pos++; break; }

    if (!SxReadName (X, Name, sizeof (Name))) { X->Pos++; continue; }
    SxSkipSpace (X);
    if (X->Pos >= X->Len || X->Src[X->Pos] != '=') continue;
    X->Pos++;
    Val = DFBR_NULL;
    if (!SxReadAttrValue (X, &Val)) continue;

    Vp = Val;
    if      (strcmp (Name, "x")  == 0) ParseNum (&Vp, &Ax);
    else if (strcmp (Name, "y")  == 0) ParseNum (&Vp, &Ay);
    else if (strcmp (Name, "width")  == 0) ParseNum (&Vp, &Aw);
    else if (strcmp (Name, "height") == 0) ParseNum (&Vp, &Ah);
    else if (strcmp (Name, "cx") == 0) ParseNum (&Vp, &Acx);
    else if (strcmp (Name, "cy") == 0) ParseNum (&Vp, &Acy);
    else if (strcmp (Name, "r")  == 0) ParseNum (&Vp, &Ar);
    else if (strcmp (Name, "rx") == 0) ParseNum (&Vp, &Arx);
    else if (strcmp (Name, "ry") == 0) ParseNum (&Vp, &Ary);
    else if (strcmp (Name, "x1") == 0) ParseNum (&Vp, &Ax);
    else if (strcmp (Name, "y1") == 0) ParseNum (&Vp, &Ay);
    else if (strcmp (Name, "x2") == 0) ParseNum (&Vp, &Ax2);
    else if (strcmp (Name, "y2") == 0) ParseNum (&Vp, &Ay2);
    else if (strcmp (Name, "points") == 0) { if (Points == DFBR_NULL) { Points = Val; Val = DFBR_NULL; } }
    else if (strcmp (Name, "d")      == 0) { if (PathD  == DFBR_NULL) { PathD  = Val; Val = DFBR_NULL; } }
    else {
      ApplyAttr (&St, Name, Val);
    }
    if (Val != DFBR_NULL) SvgFree (Val);
  }

  if      (strcmp (Tag, "rect")   == 0) EmitRect (X->R, &St, Ax, Ay, Aw, Ah);
  else if (strcmp (Tag, "circle") == 0) EmitEllipse (X->R, &St, Acx, Acy, Ar, Ar);
  else if (strcmp (Tag, "ellipse") == 0) EmitEllipse (X->R, &St, Acx, Acy, Arx, Ary);
  else if (strcmp (Tag, "line")   == 0) EmitLine (X->R, &St, Ax, Ay, Ax2, Ay2);
  else if (strcmp (Tag, "polygon")  == 0 && Points != DFBR_NULL) EmitPointList (X->R, &St, Points, DFBR_TRUE);
  else if (strcmp (Tag, "polyline") == 0 && Points != DFBR_NULL) EmitPointList (X->R, &St, Points, DFBR_FALSE);
  else if (strcmp (Tag, "path")   == 0 && PathD != DFBR_NULL) EmitPath (X->R, &St, PathD);

  if (Points != DFBR_NULL) SvgFree (Points);
  if (PathD  != DFBR_NULL) SvgFree (PathD);

  IsContainer = (b32)(strcmp (Tag, "g")   == 0 ||
                      strcmp (Tag, "svg") == 0 ||
                      strcmp (Tag, "a")   == 0);

  if (SelfClose) return;

  if (IsContainer) {
    while (X->Pos < X->Len) {
      SxSkipSpace (X);
      if (X->Pos >= X->Len) break;
      if (X->Src[X->Pos] != '<') { X->Pos++; continue; }

      if (X->Pos + 1 < X->Len && X->Src[X->Pos + 1] == '/') {
        usize Save = X->Pos;
        char  CloseName[48];
        X->Pos += 2;
        if (!SxReadName (X, CloseName, sizeof (CloseName))) {
          X->Pos = X->Len;
          return;
        }
        while (X->Pos < X->Len && X->Src[X->Pos] != '>') X->Pos++;
        if (X->Pos < X->Len) X->Pos++;
        if (strcmp (CloseName, Tag) == 0) return;
        X->Pos = Save;
        return;
      }
      SxWalkElement (X, &St);
    }
    return;
  }

  /* Non-container leaf not self-closed: consume until matching close. */
  while (X->Pos < X->Len) {
    SxSkipSpace (X);
    if (X->Pos >= X->Len) break;
    if (X->Src[X->Pos] != '<') { X->Pos++; continue; }
    if (X->Pos + 1 < X->Len && X->Src[X->Pos + 1] == '/') {
      char CloseName[48];
      X->Pos += 2;
      SxReadName (X, CloseName, sizeof (CloseName));
      while (X->Pos < X->Len && X->Src[X->Pos] != '>') X->Pos++;
      if (X->Pos < X->Len) X->Pos++;
      return;
    }
    {
      /* Unknown nested tag in a leaf - walk it anyway so we consume
       * the right byte range, but with the leaf's state. */
      SvgState Ignored = St;
      SxWalkElement (X, &Ignored);
    }
  }
}

static void SxWalkElement (Sx *X, const SvgState *Parent) {
  char  Tag[48];
  usize I;

  while (X->Pos < X->Len) {
    SxSkipSpace (X);
    if (X->Pos >= X->Len) return;
    if (X->Src[X->Pos] != '<') { X->Pos++; continue; }

    if (X->Pos + 3 < X->Len && X->Src[X->Pos + 1] == '!' &&
        X->Src[X->Pos + 2] == '-' && X->Src[X->Pos + 3] == '-') {
      SxSkipComment (X);
      continue;
    }
    if (X->Pos + 1 < X->Len &&
        (X->Src[X->Pos + 1] == '?' || X->Src[X->Pos + 1] == '!')) {
      SxSkipPiOrDoctype (X);
      continue;
    }
    break;
  }

  if (X->Pos >= X->Len || X->Src[X->Pos] != '<') return;
  X->Pos++;
  if (!SxReadName (X, Tag, sizeof (Tag))) return;
  for (I = 0; Tag[I] != '\0'; I++) {
    if (Tag[I] >= 'A' && Tag[I] <= 'Z') Tag[I] = (char)(Tag[I] - 'A' + 'a');
  }
  SxHandleElement (X, Tag, Parent);
}

/* ------------------------------------------------------------------ *
 * Root discovery: find the outer <svg> and extract width/height/viewBox.
 * ------------------------------------------------------------------ */
static b32 FindRootSvg (
  const char *Src, usize Len,
  usize *OutStartPos,
  f32 *OutW,  f32 *OutH,
  f32 *OutVbX, f32 *OutVbY,
  f32 *OutVbW, f32 *OutVbH,
  b32 *OutHasVb
  )
{
  usize I = 0;
  usize P;
  *OutW = 0; *OutH = 0;
  *OutVbX = 0; *OutVbY = 0; *OutVbW = 0; *OutVbH = 0;
  *OutHasVb = DFBR_FALSE;

  while (I + 4 < Len) {
    if (Src[I] == '<' &&
        (Src[I+1] == 's' || Src[I+1] == 'S') &&
        (Src[I+2] == 'v' || Src[I+2] == 'V') &&
        (Src[I+3] == 'g' || Src[I+3] == 'G') &&
        (IsSpaceC (Src[I+4]) || Src[I+4] == '>' || Src[I+4] == '/')) {
      break;
    }
    I++;
  }
  if (I + 4 >= Len) return DFBR_FALSE;

  P = I + 4;
  while (P < Len && Src[P] != '>') {
    char  Name[32];
    usize Ni = 0;
    char  Q;
    usize Vs, Ve;

    while (P < Len && IsSpaceC (Src[P])) P++;
    if (P >= Len || Src[P] == '>' || Src[P] == '/') break;
    while (P < Len && !IsSpaceC (Src[P]) && Src[P] != '=' && Src[P] != '>' && Src[P] != '/') {
      if (Ni + 1 < sizeof (Name)) Name[Ni++] = Src[P];
      P++;
    }
    Name[Ni] = '\0';
    while (P < Len && IsSpaceC (Src[P])) P++;
    if (P >= Len || Src[P] != '=') continue;
    P++;
    while (P < Len && IsSpaceC (Src[P])) P++;
    Q = (P < Len) ? Src[P] : 0;
    if (Q != '"' && Q != '\'') continue;
    P++;
    Vs = P;
    while (P < Len && Src[P] != Q) P++;
    Ve = P;
    if (P < Len) P++;

    if (strcmp (Name, "width") == 0 && Vs < Ve) {
      char  Tmp[32];
      usize K;
      usize L0 = Ve - Vs;
      f32   V = 0;
      const char *Pp;
      if (L0 >= 31) L0 = 31;
      for (K = 0; K < L0; K++) Tmp[K] = Src[Vs + K];
      Tmp[L0] = '\0';
      Pp = Tmp;
      ParseNum (&Pp, &V);
      *OutW = V;
    } else if (strcmp (Name, "height") == 0 && Vs < Ve) {
      char  Tmp[32];
      usize K;
      usize L0 = Ve - Vs;
      f32   V = 0;
      const char *Pp;
      if (L0 >= 31) L0 = 31;
      for (K = 0; K < L0; K++) Tmp[K] = Src[Vs + K];
      Tmp[L0] = '\0';
      Pp = Tmp;
      ParseNum (&Pp, &V);
      *OutH = V;
    } else if (strcmp (Name, "viewBox") == 0 ||
               strcmp (Name, "viewbox") == 0 ||
               strcmp (Name, "VIEWBOX") == 0) {
      char  Tmp[128];
      usize K;
      usize L0 = Ve - Vs;
      f32   A, B, C, D;
      const char *Pp;
      if (L0 >= 127) L0 = 127;
      for (K = 0; K < L0; K++) Tmp[K] = Src[Vs + K];
      Tmp[L0] = '\0';
      Pp = Tmp;
      SkipSep (&Pp);
      if (ParseNum (&Pp, &A)) { SkipSep (&Pp);
      if (ParseNum (&Pp, &B)) { SkipSep (&Pp);
      if (ParseNum (&Pp, &C)) { SkipSep (&Pp);
      if (ParseNum (&Pp, &D)) {
        *OutVbX = A; *OutVbY = B; *OutVbW = C; *OutVbH = D;
        *OutHasVb = DFBR_TRUE;
      } } } }
    }
  }
  if (P < Len && Src[P] == '/') P++;
  if (P < Len && Src[P] == '>') P++;
  *OutStartPos = I;
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------ *
 * Public entry point.
 * ------------------------------------------------------------------ */
DfBrResult
DfBrSvgRender (
  const char     *Source,
  usize           SourceLen,
  u32             TargetW,
  u32             TargetH,
  DfBrSvgBitmap  *Out
  )
{
  usize     RootPos = 0;
  f32       SvgW = 0, SvgH = 0;
  f32       VbX = 0, VbY = 0, VbW = 0, VbH = 0;
  b32       HasVb = DFBR_FALSE;
  u32       OutW, OutH;
  usize     Bytes;
  DfBrColor *Pixels;
  Rast      R;
  SvgState  St;
  Mat3      Initial;
  f32       Cw, Ch, Ox, Oy;
  Sx        X;
  usize     I;

  if (Source == DFBR_NULL || Out == DFBR_NULL) return DFBR_ERR_INVALID_ARG;

  if (SourceLen == 0) {
    while (Source[SourceLen] != '\0') SourceLen++;
  }

  memset (Out, 0, sizeof (*Out));

  if (!FindRootSvg (Source, SourceLen, &RootPos,
                    &SvgW, &SvgH, &VbX, &VbY, &VbW, &VbH, &HasVb)) {
    return DFBR_ERR_NOT_FOUND;
  }

  OutW = TargetW;
  OutH = TargetH;
  if (OutW == 0) {
    if      (SvgW > 0) OutW = (u32)SvgW;
    else if (HasVb)    OutW = (u32)VbW;
    else               OutW = 300;
  }
  if (OutH == 0) {
    if      (SvgH > 0) OutH = (u32)SvgH;
    else if (HasVb)    OutH = (u32)VbH;
    else               OutH = 150;
  }
  if (OutW == 0) OutW = 1;
  if (OutH == 0) OutH = 1;
  if (OutW > 2048) OutW = 2048;
  if (OutH > 2048) OutH = 2048;

  MatIdent (&Initial);
  Cw = HasVb ? VbW : (SvgW > 0 ? SvgW : (f32)OutW);
  Ch = HasVb ? VbH : (SvgH > 0 ? SvgH : (f32)OutH);
  Ox = HasVb ? VbX : 0.0f;
  Oy = HasVb ? VbY : 0.0f;
  if (Cw > 0 && Ch > 0) {
    /* preserveAspectRatio="meet" */
    f32 Sx_ = (f32)OutW / Cw;
    f32 Sy_ = (f32)OutH / Ch;
    f32 S   = (Sx_ < Sy_) ? Sx_ : Sy_;
    f32 Tx  = ((f32)OutW - S * Cw) * 0.5f - S * Ox;
    f32 Ty  = ((f32)OutH - S * Ch) * 0.5f - S * Oy;
    Initial.A = S; Initial.D = S;
    Initial.E = Tx; Initial.F = Ty;
  }

  Bytes  = (usize)OutW * OutH * sizeof (DfBrColor);
  Pixels = (DfBrColor *)SvgZalloc (Bytes);
  if (Pixels == DFBR_NULL) return DFBR_ERR_OUT_OF_MEMORY;

  R.Pixels = Pixels;
  R.W      = OutW;
  R.H      = OutH;

  memset (&St, 0, sizeof (St));
  St.Fill.R = 0; St.Fill.G = 0; St.Fill.B = 0; St.Fill.Set = DFBR_TRUE; St.Fill.None = DFBR_FALSE;
  St.Stroke.Set = DFBR_FALSE; St.Stroke.None = DFBR_TRUE;
  St.StrokeWidth = 1.0f;
  St.Opacity     = 1.0f;
  St.M           = Initial;

  X.Src = Source;
  X.Pos = RootPos;
  X.Len = SourceLen;
  X.R   = &R;

  SxWalkElement (&X, &St);

  /* Composite onto white. Our immediate blit targets (DX9 surface,
   * GDI DIB, EFI GOP) all ignore alpha, so premultiplying against
   * white gives a clean icon on typical page backgrounds. */
  for (I = 0; I < (usize)OutW * OutH; I++) {
    u8 A = Pixels[I].A;
    if (A == 0) {
      Pixels[I].B = 0xFF;
      Pixels[I].G = 0xFF;
      Pixels[I].R = 0xFF;
    } else if (A < 255) {
      u32 Aa = 255u - A;
      Pixels[I].B = (u8)((A * Pixels[I].B + Aa * 0xFFu) / 255u);
      Pixels[I].G = (u8)((A * Pixels[I].G + Aa * 0xFFu) / 255u);
      Pixels[I].R = (u8)((A * Pixels[I].R + Aa * 0xFFu) / 255u);
    }
    Pixels[I].A = 0xFF;
  }

  Out->Bmp.Width      = OutW;
  Out->Bmp.Height     = OutH;
  Out->Bmp.Pixels     = Pixels;
  Out->Bmp.OwnsPixels = DFBR_TRUE;
  return DFBR_OK;
}

void
DfBrSvgFree (
  DfBrSvgBitmap  *Bmp
  )
{
  if (Bmp == DFBR_NULL) return;
  if (Bmp->Bmp.Pixels != DFBR_NULL && Bmp->Bmp.OwnsPixels) {
    SvgFree (Bmp->Bmp.Pixels);
  }
  memset (Bmp, 0, sizeof (*Bmp));
}
