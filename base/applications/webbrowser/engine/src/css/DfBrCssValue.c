/** @file
  DfBrCssValue.c - CSS value parsing: lengths, colors (named, hex,
                   rgb(), hsl(), rgba(), hsla(), modern + legacy
                   syntax), keyword helpers, and a handful of small
                   helpers used across the cascade.

  The functions here take already-tokenised Component Values (DfBrCssCv)
  and convert them to the concrete types layout / paint use
  (DfBrCssColor, DfBrCssLength, ...). They never mutate the CV list.
**/

#include "DfBrCssInternal.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Length
 * -----------------------------------------------------------------*/

b32
DfBrCssLengthIsAuto (const DfBrCssLength *L)
{
  return (L != DFBR_NULL && L->Auto) ? DFBR_TRUE : DFBR_FALSE;
}

b32
DfBrCssLengthIsZero (const DfBrCssLength *L)
{
  return (L != DFBR_NULL && !L->Auto && L->Value == 0.0f) ? DFBR_TRUE : DFBR_FALSE;
}

/* Forward declarations for the calc() evaluator, defined at the
 * bottom of the file so DfBrCssLengthToPx can dispatch to them. */
static b32 CalcEvalExpr (const DfBrCssCv *Cv,
                         f32 FontSizePx, f32 RootFontSizePx,
                         f32 ViewportW,  f32 ViewportH,
                         f32 ParentPx,   f32 *OutPx);

b32
DfBrCssLengthToPx (
  const DfBrCssLength  *L,
  f32                   FontSizePx,
  f32                   RootFontSizePx,
  f32                   ViewportW,
  f32                   ViewportH,
  f32                   ParentPx,
  f32                  *Out
  )
{
  f32  Px = 0.0f;
  b32  Ok = DFBR_TRUE;

  if (L == DFBR_NULL || L->Auto) { *Out = 0.0f; return DFBR_FALSE; }

  if (L->CalcExpr != DFBR_NULL) {
    b32 R = CalcEvalExpr (L->CalcExpr, FontSizePx, RootFontSizePx,
                          ViewportW, ViewportH, ParentPx, Out);
    return R;
  }

  switch (L->Unit) {
    case DFBR_CSS_UNIT_NONE:
    case DFBR_CSS_UNIT_PX:      Px = L->Value;                       break;
    case DFBR_CSS_UNIT_PT:      Px = L->Value * (96.0f / 72.0f);     break;
    case DFBR_CSS_UNIT_PC:      Px = L->Value * 16.0f;               break;
    case DFBR_CSS_UNIT_IN:      Px = L->Value * 96.0f;               break;
    case DFBR_CSS_UNIT_CM:      Px = L->Value * (96.0f / 2.54f);     break;
    case DFBR_CSS_UNIT_MM:      Px = L->Value * (96.0f / 25.4f);     break;
    case DFBR_CSS_UNIT_Q:       Px = L->Value * (96.0f / (25.4f * 4.0f)); break;
    case DFBR_CSS_UNIT_EM:      Px = L->Value * FontSizePx;          break;
    case DFBR_CSS_UNIT_REM:     Px = L->Value * RootFontSizePx;      break;
    case DFBR_CSS_UNIT_EX:      Px = L->Value * FontSizePx * 0.5f;   break;
    case DFBR_CSS_UNIT_CH:      Px = L->Value * FontSizePx * 0.5f;   break;
    case DFBR_CSS_UNIT_VW:      Px = L->Value * ViewportW / 100.0f;  break;
    case DFBR_CSS_UNIT_VH:      Px = L->Value * ViewportH / 100.0f;  break;
    case DFBR_CSS_UNIT_VMIN:    Px = L->Value * (ViewportW < ViewportH ? ViewportW : ViewportH) / 100.0f; break;
    case DFBR_CSS_UNIT_VMAX:    Px = L->Value * (ViewportW > ViewportH ? ViewportW : ViewportH) / 100.0f; break;
    case DFBR_CSS_UNIT_PERCENT: Px = L->Value * ParentPx / 100.0f;   break;
    default:                    Px = 0.0f; Ok = DFBR_FALSE;          break;
  }
  *Out = Px;
  return Ok;
}

b32
DfBrCssCvAsLength (
  const DfBrCssCv    *Cv,
  DfBrCssLength      *Out
  )
{
  if (Cv == DFBR_NULL || Out == DFBR_NULL) return DFBR_FALSE;

  Out->CalcExpr = DFBR_NULL;

  if (Cv->Kind == DfBrCssCvKindIdent &&
      DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "auto")) {
    Out->Auto  = DFBR_TRUE;
    Out->Value = 0.0f;
    Out->Unit  = DFBR_CSS_UNIT_NONE;
    return DFBR_TRUE;
  }

  if (Cv->Kind == DfBrCssCvKindNumber) {
    /* CSS treats the unitless 0 as a length-0 of any unit. For
     * non-zero unitless numbers layout typically still wants px
     * (common in the wild) so we allow it. */
    Out->Auto  = DFBR_FALSE;
    Out->Value = (f32)Cv->Number;
    Out->Unit  = DFBR_CSS_UNIT_PX;
    return DFBR_TRUE;
  }

  if (Cv->Kind == DfBrCssCvKindPercentage) {
    Out->Auto  = DFBR_FALSE;
    Out->Value = (f32)Cv->Number;
    Out->Unit  = DFBR_CSS_UNIT_PERCENT;
    return DFBR_TRUE;
  }

  if (Cv->Kind == DfBrCssCvKindDimension) {
    Out->Auto  = DFBR_FALSE;
    Out->Value = (f32)Cv->Number;
    Out->Unit  = Cv->UnitId;
    return DFBR_TRUE;
  }

  /* calc() / min() / max() / clamp() - parsed once, resolved later
   * (lazily) by DfBrCssLengthToPx so percent / em / vw units see
   * the actual layout context. We stash a pointer to the original
   * CV; the stylesheet's arena keeps it alive for the lifetime of
   * every DfBrCssComputedStyle that cites it. */
  if (Cv->Kind == DfBrCssCvKindFunction && Cv->Str != DFBR_NULL && (
        DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "calc")
     || DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "min")
     || DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "max")
     || DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "clamp")))
  {
    Out->Auto     = DFBR_FALSE;
    Out->Value    = 0.0f;
    Out->Unit     = DFBR_CSS_UNIT_PX;
    Out->CalcExpr = Cv;
    return DFBR_TRUE;
  }

  return DFBR_FALSE;
}

b32
DfBrCssCvIsAuto (const DfBrCssCv *Cv)
{
  if (Cv == DFBR_NULL || Cv->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  return DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "auto");
}

/* ------------------------------------------------------------------
 *  CSS3 named colors (147 entries)
 * -----------------------------------------------------------------*/

typedef struct {
  const char  *Name;   /* NUL-terminated, lowercased */
  u8           R;
  u8           G;
  u8           B;
} NamedColor;

static const NamedColor mNamedColors[] = {
  { "aliceblue",            240, 248, 255 },
  { "antiquewhite",         250, 235, 215 },
  { "aqua",                   0, 255, 255 },
  { "aquamarine",           127, 255, 212 },
  { "azure",                240, 255, 255 },
  { "beige",                245, 245, 220 },
  { "bisque",               255, 228, 196 },
  { "black",                  0,   0,   0 },
  { "blanchedalmond",       255, 235, 205 },
  { "blue",                   0,   0, 255 },
  { "blueviolet",           138,  43, 226 },
  { "brown",                165,  42,  42 },
  { "burlywood",            222, 184, 135 },
  { "cadetblue",             95, 158, 160 },
  { "chartreuse",           127, 255,   0 },
  { "chocolate",            210, 105,  30 },
  { "coral",                255, 127,  80 },
  { "cornflowerblue",       100, 149, 237 },
  { "cornsilk",             255, 248, 220 },
  { "crimson",              220,  20,  60 },
  { "cyan",                   0, 255, 255 },
  { "darkblue",               0,   0, 139 },
  { "darkcyan",               0, 139, 139 },
  { "darkgoldenrod",        184, 134,  11 },
  { "darkgray",             169, 169, 169 },
  { "darkgreen",              0, 100,   0 },
  { "darkgrey",             169, 169, 169 },
  { "darkkhaki",            189, 183, 107 },
  { "darkmagenta",          139,   0, 139 },
  { "darkolivegreen",        85, 107,  47 },
  { "darkorange",           255, 140,   0 },
  { "darkorchid",           153,  50, 204 },
  { "darkred",              139,   0,   0 },
  { "darksalmon",           233, 150, 122 },
  { "darkseagreen",         143, 188, 143 },
  { "darkslateblue",         72,  61, 139 },
  { "darkslategray",         47,  79,  79 },
  { "darkslategrey",         47,  79,  79 },
  { "darkturquoise",          0, 206, 209 },
  { "darkviolet",           148,   0, 211 },
  { "deeppink",             255,  20, 147 },
  { "deepskyblue",            0, 191, 255 },
  { "dimgray",              105, 105, 105 },
  { "dimgrey",              105, 105, 105 },
  { "dodgerblue",            30, 144, 255 },
  { "firebrick",            178,  34,  34 },
  { "floralwhite",          255, 250, 240 },
  { "forestgreen",           34, 139,  34 },
  { "fuchsia",              255,   0, 255 },
  { "gainsboro",            220, 220, 220 },
  { "ghostwhite",           248, 248, 255 },
  { "gold",                 255, 215,   0 },
  { "goldenrod",            218, 165,  32 },
  { "gray",                 128, 128, 128 },
  { "green",                  0, 128,   0 },
  { "greenyellow",          173, 255,  47 },
  { "grey",                 128, 128, 128 },
  { "honeydew",             240, 255, 240 },
  { "hotpink",              255, 105, 180 },
  { "indianred",            205,  92,  92 },
  { "indigo",                75,   0, 130 },
  { "ivory",                255, 255, 240 },
  { "khaki",                240, 230, 140 },
  { "lavender",             230, 230, 250 },
  { "lavenderblush",        255, 240, 245 },
  { "lawngreen",            124, 252,   0 },
  { "lemonchiffon",         255, 250, 205 },
  { "lightblue",            173, 216, 230 },
  { "lightcoral",           240, 128, 128 },
  { "lightcyan",            224, 255, 255 },
  { "lightgoldenrodyellow", 250, 250, 210 },
  { "lightgray",            211, 211, 211 },
  { "lightgreen",           144, 238, 144 },
  { "lightgrey",            211, 211, 211 },
  { "lightpink",            255, 182, 193 },
  { "lightsalmon",          255, 160, 122 },
  { "lightseagreen",         32, 178, 170 },
  { "lightskyblue",         135, 206, 250 },
  { "lightslategray",       119, 136, 153 },
  { "lightslategrey",       119, 136, 153 },
  { "lightsteelblue",       176, 196, 222 },
  { "lightyellow",          255, 255, 224 },
  { "lime",                   0, 255,   0 },
  { "limegreen",             50, 205,  50 },
  { "linen",                250, 240, 230 },
  { "magenta",              255,   0, 255 },
  { "maroon",               128,   0,   0 },
  { "mediumaquamarine",     102, 205, 170 },
  { "mediumblue",             0,   0, 205 },
  { "mediumorchid",         186,  85, 211 },
  { "mediumpurple",         147, 112, 219 },
  { "mediumseagreen",        60, 179, 113 },
  { "mediumslateblue",      123, 104, 238 },
  { "mediumspringgreen",      0, 250, 154 },
  { "mediumturquoise",       72, 209, 204 },
  { "mediumvioletred",      199,  21, 133 },
  { "midnightblue",          25,  25, 112 },
  { "mintcream",            245, 255, 250 },
  { "mistyrose",            255, 228, 225 },
  { "moccasin",             255, 228, 181 },
  { "navajowhite",          255, 222, 173 },
  { "navy",                   0,   0, 128 },
  { "oldlace",              253, 245, 230 },
  { "olive",                128, 128,   0 },
  { "olivedrab",            107, 142,  35 },
  { "orange",               255, 165,   0 },
  { "orangered",            255,  69,   0 },
  { "orchid",               218, 112, 214 },
  { "palegoldenrod",        238, 232, 170 },
  { "palegreen",            152, 251, 152 },
  { "paleturquoise",        175, 238, 238 },
  { "palevioletred",        219, 112, 147 },
  { "papayawhip",           255, 239, 213 },
  { "peachpuff",            255, 218, 185 },
  { "peru",                 205, 133,  63 },
  { "pink",                 255, 192, 203 },
  { "plum",                 221, 160, 221 },
  { "powderblue",           176, 224, 230 },
  { "purple",               128,   0, 128 },
  { "rebeccapurple",        102,  51, 153 },
  { "red",                  255,   0,   0 },
  { "rosybrown",            188, 143, 143 },
  { "royalblue",             65, 105, 225 },
  { "saddlebrown",          139,  69,  19 },
  { "salmon",               250, 128, 114 },
  { "sandybrown",           244, 164,  96 },
  { "seagreen",              46, 139,  87 },
  { "seashell",             255, 245, 238 },
  { "sienna",               160,  82,  45 },
  { "silver",               192, 192, 192 },
  { "skyblue",              135, 206, 235 },
  { "slateblue",            106,  90, 205 },
  { "slategray",            112, 128, 144 },
  { "slategrey",            112, 128, 144 },
  { "snow",                 255, 250, 250 },
  { "springgreen",            0, 255, 127 },
  { "steelblue",             70, 130, 180 },
  { "tan",                  210, 180, 140 },
  { "teal",                   0, 128, 128 },
  { "thistle",              216, 191, 216 },
  { "tomato",               255,  99,  71 },
  { "turquoise",             64, 224, 208 },
  { "violet",               238, 130, 238 },
  { "wheat",                245, 222, 179 },
  { "white",                255, 255, 255 },
  { "whitesmoke",           245, 245, 245 },
  { "yellow",               255, 255,   0 },
  { "yellowgreen",          154, 205,  50 }
};

b32
DfBrCssColorFromName (
  const char    *Name,
  usize          Len,
  DfBrCssColor  *Out
  )
{
  usize  i;
  if (Name == DFBR_NULL) return DFBR_FALSE;
  for (i = 0; i < DFBR_ARRAY_LEN (mNamedColors); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mNamedColors[i].Name)) {
      Out->R = mNamedColors[i].R;
      Out->G = mNamedColors[i].G;
      Out->B = mNamedColors[i].B;
      Out->A = 1.0f;
      return DFBR_TRUE;
    }
  }
  /* Transparent is spec'd separately (rgba 0,0,0,0). */
  if (DfBrCssAsciiEqZ (Name, Len, "transparent")) {
    Out->R = 0; Out->G = 0; Out->B = 0; Out->A = 0.0f;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Hex color parsing (#rgb, #rrggbb, #rgba, #rrggbbaa)
 * -----------------------------------------------------------------*/

static int HexValue (char C) {
  if (C >= '0' && C <= '9') return C - '0';
  if (C >= 'a' && C <= 'f') return 10 + (C - 'a');
  if (C >= 'A' && C <= 'F') return 10 + (C - 'A');
  return -1;
}

static b32 ColorFromHex (const char *S, usize N, DfBrCssColor *Out) {
  int  D[8];
  usize i;

  if (N != 3 && N != 4 && N != 6 && N != 8) return DFBR_FALSE;
  for (i = 0; i < N; i++) {
    D[i] = HexValue (S[i]);
    if (D[i] < 0) return DFBR_FALSE;
  }
  switch (N) {
    case 3:
      Out->R = (u8)(D[0] * 0x11);
      Out->G = (u8)(D[1] * 0x11);
      Out->B = (u8)(D[2] * 0x11);
      Out->A = 1.0f;
      return DFBR_TRUE;
    case 4:
      Out->R = (u8)(D[0] * 0x11);
      Out->G = (u8)(D[1] * 0x11);
      Out->B = (u8)(D[2] * 0x11);
      Out->A = (f32)(D[3] * 0x11) / 255.0f;
      return DFBR_TRUE;
    case 6:
      Out->R = (u8)((D[0] << 4) | D[1]);
      Out->G = (u8)((D[2] << 4) | D[3]);
      Out->B = (u8)((D[4] << 4) | D[5]);
      Out->A = 1.0f;
      return DFBR_TRUE;
    case 8:
      Out->R = (u8)((D[0] << 4) | D[1]);
      Out->G = (u8)((D[2] << 4) | D[3]);
      Out->B = (u8)((D[4] << 4) | D[5]);
      Out->A = (f32)((D[6] << 4) | D[7]) / 255.0f;
      return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  rgb / rgba / hsl / hsla functional notations
 *
 *  We accept both the "legacy" comma-separated form
 *    rgb(R, G, B) / rgba(R, G, B, A)
 *  and the "modern" whitespace-separated form
 *    rgb(R G B / A)    (slash introduces alpha)
 *
 *  R/G/B are numbers 0-255 or percentages.
 *  H is <number> or <angle>; S/L are percentages; A is <number>
 *  0-1 or <percentage>.
 * -----------------------------------------------------------------*/

static f32 Clamp01 (f32 X) { return (X < 0.0f) ? 0.0f : (X > 1.0f) ? 1.0f : X; }
static u8  Clamp8 (f32 X)  { if (X < 0) X = 0; if (X > 255) X = 255; return (u8)(X + 0.5f); }

/* Read a RGB channel from a CV: number (0-255) or percentage. */
static b32 RgbChannel (const DfBrCssCv *Cv, f32 *Out) {
  if (Cv == DFBR_NULL) return DFBR_FALSE;
  if (Cv->Kind == DfBrCssCvKindNumber) {
    *Out = (f32)Cv->Number;
    return DFBR_TRUE;
  }
  if (Cv->Kind == DfBrCssCvKindPercentage) {
    *Out = (f32)Cv->Number * 255.0f / 100.0f;
    return DFBR_TRUE;
  }
  /* Common "none" in color-4 resolves to 0 - accept for forward compat. */
  if (Cv->Kind == DfBrCssCvKindIdent &&
      DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "none")) {
    *Out = 0.0f;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* Read an alpha channel: number 0-1 or percentage. */
static b32 AlphaChannel (const DfBrCssCv *Cv, f32 *Out) {
  if (Cv == DFBR_NULL) return DFBR_FALSE;
  if (Cv->Kind == DfBrCssCvKindNumber) {
    *Out = Clamp01 ((f32)Cv->Number);
    return DFBR_TRUE;
  }
  if (Cv->Kind == DfBrCssCvKindPercentage) {
    *Out = Clamp01 ((f32)Cv->Number / 100.0f);
    return DFBR_TRUE;
  }
  if (Cv->Kind == DfBrCssCvKindIdent &&
      DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "none")) {
    *Out = 0.0f;
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* Read a hue: number (degrees) or dimension deg/rad/grad/turn. */
static b32 HueChannel (const DfBrCssCv *Cv, f32 *OutDeg) {
  if (Cv == DFBR_NULL) return DFBR_FALSE;
  if (Cv->Kind == DfBrCssCvKindNumber) {
    *OutDeg = (f32)Cv->Number;
    return DFBR_TRUE;
  }
  if (Cv->Kind == DfBrCssCvKindDimension) {
    switch (Cv->UnitId) {
      case DFBR_CSS_UNIT_DEG:  *OutDeg = (f32)Cv->Number; return DFBR_TRUE;
      case DFBR_CSS_UNIT_RAD:  *OutDeg = (f32)(Cv->Number * (180.0 / 3.14159265358979323846)); return DFBR_TRUE;
      case DFBR_CSS_UNIT_GRAD: *OutDeg = (f32)(Cv->Number * 0.9);   return DFBR_TRUE;
      case DFBR_CSS_UNIT_TURN: *OutDeg = (f32)(Cv->Number * 360.0); return DFBR_TRUE;
      default: return DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

static f32 HueToRgb (f32 P, f32 Q, f32 T) {
  if (T < 0.0f) T += 1.0f;
  if (T > 1.0f) T -= 1.0f;
  if (T < 1.0f / 6.0f) return P + (Q - P) * 6.0f * T;
  if (T < 1.0f / 2.0f) return Q;
  if (T < 2.0f / 3.0f) return P + (Q - P) * (2.0f / 3.0f - T) * 6.0f;
  return P;
}

static void HslToRgb (f32 H, f32 S, f32 L, u8 *Or, u8 *Og, u8 *Ob) {
  /* H in [0,360), S,L in [0,1] */
  f32 Q;
  f32 P;
  f32 R;
  f32 G;
  f32 B;
  f32 Hn;

  while (H < 0.0f)    H += 360.0f;
  while (H >= 360.0f) H -= 360.0f;
  Hn = H / 360.0f;

  if (S == 0.0f) {
    R = G = B = L;
  } else {
    Q = (L < 0.5f) ? (L * (1.0f + S)) : (L + S - L * S);
    P = 2.0f * L - Q;
    R = HueToRgb (P, Q, Hn + 1.0f / 3.0f);
    G = HueToRgb (P, Q, Hn);
    B = HueToRgb (P, Q, Hn - 1.0f / 3.0f);
  }
  *Or = Clamp8 (R * 255.0f);
  *Og = Clamp8 (G * 255.0f);
  *Ob = Clamp8 (B * 255.0f);
}

/* Collect up to 4 argument CVs from a function's child list,
 * accepting either commas or whitespace as separators, and a final
 * `/` separator before the alpha channel. Whitespace-only leading
 * and internal CVs (after Kind==Whitespace filtering) are ignored. */
static void ReadFnArgs (
  const DfBrCssCv      *Head,
  const DfBrCssCv      *Args[4],
  b32                  *OutUsedSlash,
  i32                  *OutCount
  )
{
  i32  Count = 0;
  const DfBrCssCv *C;
  b32  SawSlash = DFBR_FALSE;

  Args[0] = Args[1] = Args[2] = Args[3] = DFBR_NULL;

  for (C = Head; C != DFBR_NULL && Count < 4; C = C->Next) {
    if (C->Kind == DfBrCssCvKindWhitespace || C->Kind == DfBrCssCvKindComma) continue;
    if (C->Kind == DfBrCssCvKindDelim && C->StrLen == 1 && C->Str[0] == '/') {
      SawSlash = DFBR_TRUE;
      continue;
    }
    Args[Count++] = C;
  }
  *OutUsedSlash = SawSlash;
  *OutCount     = Count;
}

static b32 ParseRgbFn (const DfBrCssCv *Fn, DfBrCssColor *Out) {
  const DfBrCssCv *A[4];
  i32              N;
  b32              Slash;
  f32              R, G, B;
  f32              Alpha = 1.0f;

  ReadFnArgs (Fn->Children, A, &Slash, &N);
  if (N < 3) return DFBR_FALSE;
  if (!RgbChannel (A[0], &R)) return DFBR_FALSE;
  if (!RgbChannel (A[1], &G)) return DFBR_FALSE;
  if (!RgbChannel (A[2], &B)) return DFBR_FALSE;
  if (N >= 4) {
    if (!AlphaChannel (A[3], &Alpha)) return DFBR_FALSE;
  }
  (void)Slash;
  Out->R = Clamp8 (R);
  Out->G = Clamp8 (G);
  Out->B = Clamp8 (B);
  Out->A = Alpha;
  return DFBR_TRUE;
}

static b32 ParseHslFn (const DfBrCssCv *Fn, DfBrCssColor *Out) {
  const DfBrCssCv *A[4];
  i32              N;
  b32              Slash;
  f32              H, S, L;
  f32              Alpha = 1.0f;
  u8               R, G, B;

  ReadFnArgs (Fn->Children, A, &Slash, &N);
  if (N < 3) return DFBR_FALSE;
  if (!HueChannel (A[0], &H)) return DFBR_FALSE;
  if (A[1] == DFBR_NULL) return DFBR_FALSE;
  if (A[1]->Kind == DfBrCssCvKindPercentage) {
    S = (f32)A[1]->Number / 100.0f;
  } else if (A[1]->Kind == DfBrCssCvKindNumber) {
    S = (f32)A[1]->Number;
  } else {
    return DFBR_FALSE;
  }
  if (A[2] == DFBR_NULL) return DFBR_FALSE;
  if (A[2]->Kind == DfBrCssCvKindPercentage) {
    L = (f32)A[2]->Number / 100.0f;
  } else if (A[2]->Kind == DfBrCssCvKindNumber) {
    L = (f32)A[2]->Number;
  } else {
    return DFBR_FALSE;
  }
  if (N >= 4) {
    if (!AlphaChannel (A[3], &Alpha)) return DFBR_FALSE;
  }
  (void)Slash;

  S = Clamp01 (S);
  L = Clamp01 (L);
  HslToRgb (H, S, L, &R, &G, &B);
  Out->R = R;
  Out->G = G;
  Out->B = B;
  Out->A = Alpha;
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------
 *  color-mix(in <colorspace>, <color1> <pct1>?, <color2> <pct2>?)
 *
 *  CSS Color 5 §2. We implement a minimal srgb-space mix that's
 *  sufficient for the overwhelmingly common idiom seen in modern CSS
 *  reset / theme files:
 *
 *    color-mix(in srgb, var(--accent) 40%, transparent)
 *
 *  Lab/LCh/OKLab color spaces are quietly treated as srgb; the visual
 *  difference is negligible for the translucent-tint use cases this
 *  function shows up in (borders, hover backgrounds, soft badges).
 *
 *  Percentage handling follows the spec:
 *    - If no percentage is given for a color, it defaults to 50%.
 *    - If exactly one percentage is given, the other is (100% - p).
 *    - If both are given and they don't sum to 100%, the alpha is
 *      scaled by (p1 + p2) / 100 and the weights are renormalised.
 *    - If both are 0%, the function is invalid (we return false).
 * -----------------------------------------------------------------*/

static b32
ParseColorWithOptPct (const DfBrCssCv  *Head,
                      DfBrCssColor     *OutColor,
                      f32              *OutPct,      /* -1.0 when omitted */
                      const DfBrCssCv **OutTail)
{
  const DfBrCssCv *C = Head;
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  if (C == DFBR_NULL) return DFBR_FALSE;

  if (!DfBrCssCvAsColor (C, OutColor, &C)) {
    return DFBR_FALSE;
  }
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;

  if (C != DFBR_NULL && C->Kind == DfBrCssCvKindPercentage) {
    *OutPct = (f32)C->Number;
    C = C->Next;
    while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  } else {
    *OutPct = -1.0f;
  }
  *OutTail = C;
  return DFBR_TRUE;
}

static b32
ParseColorMixFn (const DfBrCssCv *Fn, DfBrCssColor *Out)
{
  const DfBrCssCv *C = Fn->Children;
  DfBrCssColor     C1, C2;
  f32              P1, P2;
  f32              W1, W2, Sum, AlphaScale;
  f32              Ar, Ag, Ab, Aa;
  f32              Br, Bg, Bb, Ba;

  /* "in <colorspace>" - we accept any named colorspace and mix in
   * srgb. The ident after "in" is skipped. */
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  if (C == DFBR_NULL || C->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  if (!DfBrCssAsciiEqZ (C->Str, C->StrLen, "in")) return DFBR_FALSE;
  C = C->Next;
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  if (C == DFBR_NULL || C->Kind != DfBrCssCvKindIdent) return DFBR_FALSE;
  /* Consume the colorspace ident (srgb / srgb-linear / lab / ...). */
  C = C->Next;

  /* Optional hue-interpolation modifier: "shorter hue" / "longer hue"
   * etc. Skip any idents here until we hit the comma. */
  while (C != DFBR_NULL &&
         (C->Kind == DfBrCssCvKindWhitespace ||
          C->Kind == DfBrCssCvKindIdent)) {
    C = C->Next;
  }
  if (C == DFBR_NULL || C->Kind != DfBrCssCvKindComma) return DFBR_FALSE;
  C = C->Next;

  if (!ParseColorWithOptPct (C, &C1, &P1, &C)) return DFBR_FALSE;
  if (C == DFBR_NULL || C->Kind != DfBrCssCvKindComma) return DFBR_FALSE;
  C = C->Next;
  if (!ParseColorWithOptPct (C, &C2, &P2, &C)) return DFBR_FALSE;

  /* Resolve percentages. */
  if (P1 < 0.0f && P2 < 0.0f) { W1 = 50.0f; W2 = 50.0f; }
  else if (P1 < 0.0f)         { W2 = P2; W1 = 100.0f - W2; }
  else if (P2 < 0.0f)         { W1 = P1; W2 = 100.0f - W1; }
  else                        { W1 = P1; W2 = P2; }

  if (W1 < 0.0f) W1 = 0.0f;
  if (W2 < 0.0f) W2 = 0.0f;
  Sum = W1 + W2;
  if (Sum <= 0.0f) return DFBR_FALSE;
  AlphaScale = (Sum < 100.0f) ? (Sum / 100.0f) : 1.0f;
  W1 /= Sum;
  W2 /= Sum;

  /* Premultiplied alpha mix in srgb space. `transparent` should
   * contribute no RGB, which premultiplication gives us for free. */
  Ar = (f32)C1.R * C1.A;
  Ag = (f32)C1.G * C1.A;
  Ab = (f32)C1.B * C1.A;
  Aa = C1.A;
  Br = (f32)C2.R * C2.A;
  Bg = (f32)C2.G * C2.A;
  Bb = (f32)C2.B * C2.A;
  Ba = C2.A;

  {
    f32 OutAlpha = (Aa * W1 + Ba * W2) * AlphaScale;
    f32 Premul   = Aa * W1 + Ba * W2;
    f32 R, G, B;
    if (Premul <= 0.0f) {
      R = G = B = 0.0f;
    } else {
      R = (Ar * W1 + Br * W2) / Premul;
      G = (Ag * W1 + Bg * W2) / Premul;
      B = (Ab * W1 + Bb * W2) / Premul;
    }
    Out->R = Clamp8 (R);
    Out->G = Clamp8 (G);
    Out->B = Clamp8 (B);
    Out->A = Clamp01 (OutAlpha);
  }
  return DFBR_TRUE;
}

/* ------------------------------------------------------------------
 *  Public: parse the leading value run as a <color>
 * -----------------------------------------------------------------*/

b32
DfBrCssCvAsColor (
  const DfBrCssCv    *Cv,
  DfBrCssColor       *Out,
  const DfBrCssCv   **OutTail
  )
{
  if (Cv == DFBR_NULL) return DFBR_FALSE;

  /* Hash: #RGB / #RRGGBB / #RGBA / #RRGGBBAA. */
  if (Cv->Kind == DfBrCssCvKindHash) {
    if (ColorFromHex (Cv->Str, Cv->StrLen, Out)) {
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
    return DFBR_FALSE;
  }

  /* Named color / `transparent` / `currentcolor`. */
  if (Cv->Kind == DfBrCssCvKindIdent) {
    /* currentcolor is a sentinel that resolves to the Color property
     * at cascade time. We encode it as R=G=B=0, A=-1 to mark it;
     * callers that want the sentinel explicitly should use
     * DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "currentcolor"). For now
     * we simply report black + caller decides. */
    if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "currentcolor")) {
      Out->R = 0; Out->G = 0; Out->B = 0; Out->A = 1.0f;
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
    if (DfBrCssColorFromName (Cv->Str, Cv->StrLen, Out)) {
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
    return DFBR_FALSE;
  }

  /* Function: rgb / rgba / hsl / hsla. */
  if (Cv->Kind == DfBrCssCvKindFunction) {
    if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "rgb") ||
        DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "rgba")) {
      if (!ParseRgbFn (Cv, Out)) return DFBR_FALSE;
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
    if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "hsl") ||
        DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "hsla")) {
      if (!ParseHslFn (Cv, Out)) return DFBR_FALSE;
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
    if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "color-mix")) {
      if (!ParseColorMixFn (Cv, Out)) return DFBR_FALSE;
      if (OutTail != DFBR_NULL) *OutTail = Cv->Next;
      return DFBR_TRUE;
    }
  }

  return DFBR_FALSE;
}

b32
DfBrCssColorFromCv (
  const DfBrCssCv    *Cv,
  DfBrCssColor       *Out
  )
{
  return DfBrCssCvAsColor (Cv, Out, DFBR_NULL);
}

/* ------------------------------------------------------------------
 *  calc() / min() / max() / clamp() evaluator
 *
 *  Evaluates a deferred length expression against the current layout
 *  context. The CV tree comes straight from the parser; operators are
 *  the CSS-level `+`, `-`, `*`, `/` delim tokens separated by
 *  whitespace, so we run a Pratt-style expression parser:
 *
 *    expression := term { ("+" | "-") term }
 *    term       := factor { ("*" | "/") factor }
 *    factor     := number | dimension | percentage
 *                | "(" expression ")"                // anon-paren fn
 *                | "calc" "(" expression ")"
 *                | "min" "(" expression ("," expression)+ ")"
 *                | "max" "(" expression ("," expression)+ ")"
 *                | "clamp" "(" expression "," expression "," expression ")"
 *
 *  Percentages inside multiplication resolve through ParentPx before
 *  being folded; that matches both Blink and Ladybird's behaviour.
 * -----------------------------------------------------------------*/

typedef struct {
  const DfBrCssCv *Cur;        /* current token */
  f32              FontSizePx;
  f32              RootFontSizePx;
  f32              ViewportW;
  f32              ViewportH;
  f32              ParentPx;
  b32              Error;
} CalcCtx;

static const DfBrCssCv *
CalcSkipWs (const DfBrCssCv *C) {
  while (C != DFBR_NULL && C->Kind == DfBrCssCvKindWhitespace) C = C->Next;
  return C;
}

static void CalcAdvance (CalcCtx *Ctx) {
  if (Ctx->Cur != DFBR_NULL) Ctx->Cur = CalcSkipWs (Ctx->Cur->Next);
}

static b32 CalcEvalLeafLen (const DfBrCssCv *Cv, CalcCtx *Ctx, f32 *Out) {
  DfBrCssLength L;
  if (!DfBrCssCvAsLength (Cv, &L) || L.CalcExpr != DFBR_NULL || L.Auto) {
    return DFBR_FALSE;
  }
  return DfBrCssLengthToPx (&L,
                            Ctx->FontSizePx, Ctx->RootFontSizePx,
                            Ctx->ViewportW,  Ctx->ViewportH,
                            Ctx->ParentPx,   Out);
}

static b32 CalcExpr (CalcCtx *Ctx, f32 *Out);

static b32 CalcFactor (CalcCtx *Ctx, f32 *Out) {
  const DfBrCssCv *C = Ctx->Cur;
  if (C == DFBR_NULL) { Ctx->Error = DFBR_TRUE; return DFBR_FALSE; }

  /* Unary +/- */
  if (C->Kind == DfBrCssCvKindDelim && C->StrLen == 1
      && (C->Str[0] == '+' || C->Str[0] == '-'))
  {
    char Op = C->Str[0];
    f32  Inner;
    CalcAdvance (Ctx);
    if (!CalcFactor (Ctx, &Inner)) return DFBR_FALSE;
    *Out = (Op == '-') ? -Inner : Inner;
    return DFBR_TRUE;
  }

  /* Parenthesised subexpression (anonymous paren function) or nested
   * calc(). Both arrive as a Function CV; anonymous parens have an
   * empty name. */
  if (C->Kind == DfBrCssCvKindFunction && C->Str != DFBR_NULL) {
    b32 IsCalc  = DfBrCssAsciiEqZ (C->Str, C->StrLen, "calc");
    b32 IsMin   = DfBrCssAsciiEqZ (C->Str, C->StrLen, "min");
    b32 IsMax   = DfBrCssAsciiEqZ (C->Str, C->StrLen, "max");
    b32 IsClamp = DfBrCssAsciiEqZ (C->Str, C->StrLen, "clamp");
    b32 IsParen = (C->StrLen == 0);

    if (IsParen || IsCalc) {
      CalcCtx Sub;
      Sub             = *Ctx;
      Sub.Cur         = CalcSkipWs (C->Children);
      Sub.Error       = DFBR_FALSE;
      if (!CalcExpr (&Sub, Out)) { Ctx->Error = DFBR_TRUE; return DFBR_FALSE; }
      if (Sub.Cur != DFBR_NULL) { Ctx->Error = DFBR_TRUE; return DFBR_FALSE; }
      CalcAdvance (Ctx);
      return DFBR_TRUE;
    }

    if (IsMin || IsMax || IsClamp) {
      /* Comma-separated argument list, each arg is itself an
       * expression. We walk the child list ourselves, splitting on
       * top-level commas. */
      const DfBrCssCv *Arg = CalcSkipWs (C->Children);
      f32  Vals[8];
      int  N = 0;
      while (Arg != DFBR_NULL && N < 8) {
        CalcCtx Sub;
        const DfBrCssCv *EndOfArg = Arg;
        /* Find the end of this arg (next comma at top level). */
        while (EndOfArg != DFBR_NULL && EndOfArg->Kind != DfBrCssCvKindComma) {
          EndOfArg = EndOfArg->Next;
        }
        /* Temporarily snip the list - can't mutate, so run a Sub that
         * stops by checking Arg pointer. Simpler: we copy the Arg
         * chain up to the comma by scanning once with an inner
         * evaluator that stops when it hits a comma. */
        Sub             = *Ctx;
        Sub.Cur         = Arg;
        Sub.Error       = DFBR_FALSE;
        /* Evaluate until Sub.Cur reaches a comma or becomes NULL. */
        if (!CalcExpr (&Sub, &Vals[N])) { Ctx->Error = DFBR_TRUE; return DFBR_FALSE; }
        N++;
        /* CalcExpr stops at a non-additive/non-mul token; we expect
         * it stopped either at NULL or at a comma. */
        Arg = Sub.Cur;
        if (Arg != DFBR_NULL && Arg->Kind == DfBrCssCvKindComma) {
          Arg = CalcSkipWs (Arg->Next);
          continue;
        }
        break;
      }
      if (N == 0) { Ctx->Error = DFBR_TRUE; return DFBR_FALSE; }

      if (IsClamp) {
        f32 Mn = (N >= 1) ? Vals[0] : 0.0f;
        f32 Vl = (N >= 2) ? Vals[1] : Mn;
        f32 Mx = (N >= 3) ? Vals[2] : Vl;
        if (Vl < Mn) Vl = Mn;
        if (Vl > Mx) Vl = Mx;
        *Out = Vl;
      } else {
        int I; f32 R = Vals[0];
        for (I = 1; I < N; I++) {
          if (IsMin) { if (Vals[I] < R) R = Vals[I]; }
          else       { if (Vals[I] > R) R = Vals[I]; }
        }
        *Out = R;
      }
      CalcAdvance (Ctx);
      return DFBR_TRUE;
    }

    /* Unknown function inside calc: fail. */
    Ctx->Error = DFBR_TRUE;
    return DFBR_FALSE;
  }

  /* Leaf: number / dimension / percentage. */
  if (!CalcEvalLeafLen (C, Ctx, Out)) {
    /* Bare numbers are also OK inside calc() as dimensionless
     * multipliers. CalcEvalLeafLen already handles that via the
     * unit-less-number path in DfBrCssCvAsLength. */
    Ctx->Error = DFBR_TRUE;
    return DFBR_FALSE;
  }
  CalcAdvance (Ctx);
  return DFBR_TRUE;
}

static b32 CalcTerm (CalcCtx *Ctx, f32 *Out) {
  f32 Lhs;
  if (!CalcFactor (Ctx, &Lhs)) return DFBR_FALSE;
  for (;;) {
    const DfBrCssCv *C = Ctx->Cur;
    if (C == DFBR_NULL) break;
    if (C->Kind != DfBrCssCvKindDelim || C->StrLen != 1) break;
    if (C->Str[0] != '*' && C->Str[0] != '/') break;
    {
      char Op = C->Str[0];
      f32  Rhs;
      CalcAdvance (Ctx);
      if (!CalcFactor (Ctx, &Rhs)) return DFBR_FALSE;
      if (Op == '*') Lhs = Lhs * Rhs;
      else           Lhs = (Rhs == 0.0f) ? 0.0f : (Lhs / Rhs);
    }
  }
  *Out = Lhs;
  return DFBR_TRUE;
}

static b32 CalcExpr (CalcCtx *Ctx, f32 *Out) {
  f32 Lhs;
  if (!CalcTerm (Ctx, &Lhs)) return DFBR_FALSE;
  for (;;) {
    const DfBrCssCv *C = Ctx->Cur;
    if (C == DFBR_NULL) break;
    if (C->Kind == DfBrCssCvKindComma) break;
    if (C->Kind != DfBrCssCvKindDelim || C->StrLen != 1) break;
    if (C->Str[0] != '+' && C->Str[0] != '-') break;
    {
      char Op = C->Str[0];
      f32  Rhs;
      CalcAdvance (Ctx);
      if (!CalcTerm (Ctx, &Rhs)) return DFBR_FALSE;
      if (Op == '+') Lhs = Lhs + Rhs;
      else           Lhs = Lhs - Rhs;
    }
  }
  *Out = Lhs;
  return DFBR_TRUE;
}

static b32
CalcEvalExpr (const DfBrCssCv *Cv,
              f32 FontSizePx, f32 RootFontSizePx,
              f32 ViewportW,  f32 ViewportH,
              f32 ParentPx,   f32 *OutPx)
{
  CalcCtx Ctx;
  f32     Result = 0.0f;

  if (Cv == DFBR_NULL || Cv->Kind != DfBrCssCvKindFunction) {
    *OutPx = 0.0f;
    return DFBR_FALSE;
  }

  Ctx.Cur            = CalcSkipWs (Cv->Children);
  Ctx.FontSizePx     = FontSizePx;
  Ctx.RootFontSizePx = RootFontSizePx;
  Ctx.ViewportW      = ViewportW;
  Ctx.ViewportH      = ViewportH;
  Ctx.ParentPx       = ParentPx;
  Ctx.Error          = DFBR_FALSE;

  /* calc() has a single expression; min/max/clamp dispatch through
   * the CalcFactor function-path when they appear as the root of a
   * computed-style length. */
  if (DfBrCssAsciiEqZ (Cv->Str, Cv->StrLen, "calc")) {
    if (!CalcExpr (&Ctx, &Result) || Ctx.Error) {
      *OutPx = 0.0f;
      return DFBR_FALSE;
    }
  } else {
    /* Re-enter through CalcFactor so min/max/clamp reuse the same
     * argument walker. Fabricate a throwaway expression containing
     * just this single function CV. */
    CalcCtx Outer;
    DfBrCssCv Faux;

    memset (&Faux, 0, sizeof (Faux));
    Faux.Kind     = Cv->Kind;
    Faux.Str      = Cv->Str;
    Faux.StrLen   = Cv->StrLen;
    Faux.Children = Cv->Children;
    Faux.Next     = DFBR_NULL;

    Outer     = Ctx;
    Outer.Cur = &Faux;
    if (!CalcExpr (&Outer, &Result) || Outer.Error) {
      *OutPx = 0.0f;
      return DFBR_FALSE;
    }
  }

  *OutPx = Result;
  return DFBR_TRUE;
}
