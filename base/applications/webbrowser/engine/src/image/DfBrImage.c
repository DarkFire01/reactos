/** @file
  DfBrImage.c - data: URL parsing + stb_image decoding.

  Why we hand-roll the data: URL parser
  =====================================
  The WHATWG URL spec has a very specific take on `data:`, but for the
  browser's current needs (author-supplied `<img src="data:...">`
  payloads) we only need a small subset:

      data:[<mediatype>][;base64],<payload>

  We ignore the mediatype, because stb_image sniffs the byte magic
  itself (PNG/JPG/GIF/...). We do honor the `;base64` marker, and
  fall back to percent-decoding when it is absent. Parameters other
  than `;base64` (e.g. `;charset=utf-8`) are skipped over.

  We deliberately *don't* depend on quickjs or any URL library - the
  whole file is pure C, allocates via malloc/free, and fits the
  portability story (UEFI, SDL, X11 later).
**/

#include "DfBrImage.h"
#include "../net/DfBrNet.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* stb_image configuration: trim the encoders we don't need and keep
 * the decoders we do. Setting STB_IMAGE_IMPLEMENTATION exactly once
 * here is what makes the single-header library produce real symbols. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
/* stb_image prints ugly printf spam on failure in some builds; the
 * define below muzzles it for our static link. */
#define STBI_ASSERT(x) ((void)0)
#include "../../3rdparty/stb/stb_image.h"

#include <math.h>
#ifndef DFBR_NO_WEBP
#include "src/webp/decode.h"
#endif

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

enum {
  DFBR_PIX_OWNER_STB  = 0,
  DFBR_PIX_OWNER_WEBP = 1,
  DFBR_PIX_OWNER_LIBC = 2,
};

/* ---- base64 ------------------------------------------------------ */

static const i8 Base64Table[256] = {
  /* 0x00 */ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  /* 0x10 */ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  /* 0x20 */ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  /* 0x30 */ 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
  /* 0x40 */ -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  /* 0x50 */ 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  /* 0x60 */ -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  /* 0x70 */ 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  /* 0x80-0xff */
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static u8 *
Base64Decode (const char *Src, usize Len, usize *OutLen)
{
  u8   *Buf;
  usize Cap = (Len / 4) * 3 + 3;
  usize N   = 0;
  i32   Acc = 0;
  i32   Bits = 0;
  usize I;

  Buf = (u8 *)malloc (Cap);
  if (Buf == DFBR_NULL) return DFBR_NULL;

  for (I = 0; I < Len; I++) {
    u8 C = (u8)Src[I];
    i8 V;
    /* Whitespace (and the `=` pad) simply ends accumulation of a
     * character; per RFC 4648 we're allowed to ignore newlines. */
    if (C == '=' || C == ' ' || C == '\n' || C == '\r' || C == '\t') continue;
    V = Base64Table[C];
    if (V < 0) {
      free (Buf);
      return DFBR_NULL;
    }
    Acc = (Acc << 6) | V;
    Bits += 6;
    if (Bits >= 8) {
      Bits -= 8;
      Buf[N++] = (u8)((Acc >> Bits) & 0xFF);
    }
  }
  *OutLen = N;
  return Buf;
}

/* ---- percent decode --------------------------------------------- */

static i32
HexDigit (char C)
{
  if (C >= '0' && C <= '9') return C - '0';
  if (C >= 'a' && C <= 'f') return 10 + (C - 'a');
  if (C >= 'A' && C <= 'F') return 10 + (C - 'A');
  return -1;
}

static u8 *
PercentDecode (const char *Src, usize Len, usize *OutLen)
{
  u8   *Buf  = (u8 *)malloc (Len + 1);
  usize N    = 0;
  usize I;
  if (Buf == DFBR_NULL) return DFBR_NULL;

  for (I = 0; I < Len; I++) {
    char C = Src[I];
    if (C == '%' && I + 2 < Len) {
      i32 H = HexDigit (Src[I + 1]);
      i32 L = HexDigit (Src[I + 2]);
      if (H >= 0 && L >= 0) {
        Buf[N++] = (u8)((H << 4) | L);
        I += 2;
        continue;
      }
    }
    Buf[N++] = (u8)C;
  }
  *OutLen = N;
  return Buf;
}

/* ---- WebP / SVG (after stb) ------------------------------------- */

static b32
IsWebpChunk (const u8 *Data, usize Len)
{
  return Len >= 12 && Data[0] == 'R' && Data[1] == 'I' && Data[2] == 'F' &&
         Data[3] == 'F' && Data[8] == 'W' && Data[9] == 'E' && Data[10] == 'B' &&
         Data[11] == 'P';
}

static b32
SniffSvg (const u8 *Data, usize Len)
{
  usize i = 0;
  if (Len >= 3 && Data[0] == 0xEFu && Data[1] == 0xBBu && Data[2] == 0xBFu) {
    i = 3;
  }
  while (i < Len &&
         (Data[i] == ' ' || Data[i] == '\t' || Data[i] == '\n' || Data[i] == '\r')) {
    i++;
  }
  if (i >= Len || Data[i] != '<') return DFBR_FALSE;
  i++;
  if (i + 3u <= Len && (Data[i] == 's' || Data[i] == 'S') &&
      (Data[i + 1] == 'v' || Data[i + 1] == 'V') &&
      (Data[i + 2] == 'g' || Data[i + 2] == 'G')) {
    return DFBR_TRUE;
  }
  if (i + 4u <= Len && Data[i] == '?' && (Data[i + 1] == 'x' || Data[i + 1] == 'X') &&
      (Data[i + 2] == 'm' || Data[i + 2] == 'M') &&
      (Data[i + 3] == 'l' || Data[i + 3] == 'L')) {
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

static b32
ContentTypeIsSvg (const char *Ct)
{
  const char *P;
  if (Ct == DFBR_NULL || Ct[0] == '\0') return DFBR_FALSE;
  for (P = Ct; *P != '\0'; P++) {
    if ((*P == 'i' || *P == 'I') && (P[1] == 'm' || P[1] == 'M') &&
        (P[2] == 'a' || P[2] == 'A') && (P[3] == 'g' || P[3] == 'G') &&
        (P[4] == 'e' || P[4] == 'E') && P[5] == '/' && (P[6] == 's' || P[6] == 'S') &&
        (P[7] == 'v' || P[7] == 'V') && (P[8] == 'g' || P[8] == 'G')) {
      return DFBR_TRUE;
    }
  }
  return DFBR_FALSE;
}

#ifndef DFBR_NO_WEBP
static b32
DecodeWebp (const u8 *Data, usize Len, DfBrImageDecoded *Out)
{
  int  W = 0, H = 0;
  u8  *P;
  if (!IsWebpChunk (Data, Len)) return DFBR_FALSE;
  if (!WebPGetInfo (Data, Len, &W, &H) || W <= 0 || H <= 0) return DFBR_FALSE;
  P = WebPDecodeRGBA (Data, Len, &W, &H);
  if (P == DFBR_NULL || W <= 0 || H <= 0) return DFBR_FALSE;
  Out->Pixels      = P;
  Out->Width       = (u32)W;
  Out->Height      = (u32)H;
  Out->PixelsOwner = DFBR_PIX_OWNER_WEBP;
  return DFBR_TRUE;
}
#endif

#ifndef DFBR_SVG_MAX_SIDE
#define DFBR_SVG_MAX_SIDE 2048
#endif

static b32
DecodeSvgRaster (const u8 *Data, usize Len, DfBrImageDecoded *Out)
{
  char *          Mut;
  NSVGimage *     Svg;
  NSVGrasterizer *Rast;
  u8 *            Pix;
  float           Sw, Sh, Scale;
  int             W, H;

  Mut = (char *)malloc (Len + 1);
  if (Mut == DFBR_NULL) return DFBR_FALSE;
  memcpy (Mut, Data, Len);
  Mut[Len] = '\0';
  Svg = nsvgParse (Mut, "px", 96.0f);
  free (Mut);
  if (Svg == DFBR_NULL) return DFBR_FALSE;
  Sw = Svg->width;
  Sh = Svg->height;
  if (Sw < 1.0f) Sw = 100.0f;
  if (Sh < 1.0f) Sh = 100.0f;
  Scale = 1.0f;
  W     = (int)ceilf (Sw * Scale);
  H     = (int)ceilf (Sh * Scale);
  while ((W > DFBR_SVG_MAX_SIDE || H > DFBR_SVG_MAX_SIDE) && Scale > 0.02f) {
    Scale *= 0.5f;
    W = (int)ceilf (Sw * Scale);
    H = (int)ceilf (Sh * Scale);
  }
  if (W < 1) W = 1;
  if (H < 1) H = 1;
  Pix = (u8 *)malloc ((usize)W * (usize)H * 4u);
  if (Pix == DFBR_NULL) {
    nsvgDelete (Svg);
    return DFBR_FALSE;
  }
  Rast = nsvgCreateRasterizer ();
  if (Rast == DFBR_NULL) {
    free (Pix);
    nsvgDelete (Svg);
    return DFBR_FALSE;
  }
  nsvgRasterize (Rast, Svg, 0.0f, 0.0f, Scale, Pix, W, H, W * 4);
  nsvgDeleteRasterizer (Rast);
  nsvgDelete (Svg);
  Out->Pixels      = Pix;
  Out->Width       = (u32)W;
  Out->Height      = (u32)H;
  Out->PixelsOwner = DFBR_PIX_OWNER_LIBC;
  return DFBR_TRUE;
}

static b32
ImageDecodeWithMime (const u8 *Data, usize Len, const char *ContentType,
                     DfBrImageDecoded *Out)
{
  i32 W = 0, H = 0, Comp = 0;
  u8 *P;

  if (Data == DFBR_NULL || Len == 0 || Out == DFBR_NULL) return DFBR_FALSE;

  P = stbi_load_from_memory (Data, (i32)Len, &W, &H, &Comp, 4);
  if (P != DFBR_NULL && W > 0 && H > 0) {
    Out->Pixels      = P;
    Out->Width       = (u32)W;
    Out->Height      = (u32)H;
    Out->PixelsOwner = DFBR_PIX_OWNER_STB;
    return DFBR_TRUE;
  }

#ifndef DFBR_NO_WEBP
  if (DecodeWebp (Data, Len, Out)) return DFBR_TRUE;
#endif

  if (ContentTypeIsSvg (ContentType) || SniffSvg (Data, Len)) {
    if (DecodeSvgRaster (Data, Len, Out)) return DFBR_TRUE;
  }

  return DFBR_FALSE;
}

/* ---- public ----------------------------------------------------- */

b32
DfBrImageDecode (const u8 *Data, usize Len, DfBrImageDecoded *Out)
{
  return ImageDecodeWithMime (Data, Len, DFBR_NULL, Out);
}

b32
DfBrImageDecodeDataUrl (const char *Url, DfBrImageDecoded *Out)
{
  const char *P;
  const char *Comma;
  const char *Meta;
  usize       PayloadLen;
  u8         *Decoded;
  usize       DecodedLen = 0;
  b32         IsBase64 = DFBR_FALSE;
  b32         Ok;

  if (Url == DFBR_NULL || Out == DFBR_NULL) return DFBR_FALSE;

  /* Accept "data:" case-insensitively. Spaces-before-scheme are
   * tolerated because authored HTML is ... creative. */
  while (*Url == ' ' || *Url == '\t') Url++;
  if ((Url[0] != 'd' && Url[0] != 'D') ||
      (Url[1] != 'a' && Url[1] != 'A') ||
      (Url[2] != 't' && Url[2] != 'T') ||
      (Url[3] != 'a' && Url[3] != 'A') ||
      Url[4] != ':') {
    return DFBR_FALSE;
  }
  P = Url + 5;

  Comma = strchr (P, ',');
  if (Comma == DFBR_NULL) return DFBR_FALSE;

  /* Scan metadata params for ";base64". We don't care about
   * mediatype; stb_image sniffs its own format. */
  for (Meta = P; Meta < Comma; Meta++) {
    if (*Meta == ';' &&
        (Comma - Meta) >= 7 &&
        (Meta[1] == 'b' || Meta[1] == 'B') &&
        (Meta[2] == 'a' || Meta[2] == 'A') &&
        (Meta[3] == 's' || Meta[3] == 'S') &&
        (Meta[4] == 'e' || Meta[4] == 'E') &&
        Meta[5] == '6' && Meta[6] == '4' &&
        (Meta + 7 == Comma || Meta[7] == ',' || Meta[7] == ';')) {
      IsBase64 = DFBR_TRUE;
      break;
    }
  }

  PayloadLen = strlen (Comma + 1);
  if (IsBase64) {
    Decoded = Base64Decode (Comma + 1, PayloadLen, &DecodedLen);
  } else {
    Decoded = PercentDecode (Comma + 1, PayloadLen, &DecodedLen);
  }
  if (Decoded == DFBR_NULL) return DFBR_FALSE;

  Ok = DfBrImageDecode (Decoded, DecodedLen, Out);
  free (Decoded);
  return Ok;
}

void
DfBrImageFree (DfBrImageDecoded *Img)
{
  if (Img == DFBR_NULL || Img->Pixels == DFBR_NULL) return;
#ifndef DFBR_NO_WEBP
  if (Img->PixelsOwner == DFBR_PIX_OWNER_WEBP) {
    WebPFree (Img->Pixels);
  } else
#endif
  if (Img->PixelsOwner == DFBR_PIX_OWNER_LIBC) {
    free (Img->Pixels);
  } else {
    stbi_image_free (Img->Pixels);
  }
  Img->Width       = 0;
  Img->Height      = 0;
  Img->Pixels      = DFBR_NULL;
  Img->PixelsOwner = DFBR_PIX_OWNER_STB;
}

/* ------------------------------------------------------------------
 *  DfBrImageDecodeFromUrl
 *
 *  Keeps the single "here's where images get resolved" seam in one
 *  file. Callers don't have to know whether an image was inlined as
 *  a data: URL, fetched over HTTPS, or - later - pulled from an
 *  in-memory blob: store.
 * -----------------------------------------------------------------*/

static b32
StartsWithI (const char *S, const char *Prefix)
{
  usize i;
  if (S == DFBR_NULL || Prefix == DFBR_NULL) return DFBR_FALSE;
  for (i = 0; Prefix[i] != '\0'; i++) {
    char A = S[i];
    char B = Prefix[i];
    if (A >= 'A' && A <= 'Z') A = (char)(A - 'A' + 'a');
    if (B >= 'A' && B <= 'Z') B = (char)(B - 'A' + 'a');
    if (A != B) return DFBR_FALSE;
  }
  return DFBR_TRUE;
}

static b32
IsHttpish (const char *S)
{
  return (StartsWithI (S, "http://") || StartsWithI (S, "https://"))
           ? DFBR_TRUE : DFBR_FALSE;
}

b32
DfBrImageDecodeFromUrl (const char       *Url,
                        const char       *BaseUrl,
                        DfBrImageDecoded *Out)
{
  char             *Abs = DFBR_NULL;
  const char       *Target;
  DfBrHttpResponse  Resp;
  DfBrResult        Rc;
  b32               Ok = DFBR_FALSE;

  if (Url == DFBR_NULL || Out == DFBR_NULL) return DFBR_FALSE;
  memset (Out, 0, sizeof (*Out));

  if (StartsWithI (Url, "data:")) {
    return DfBrImageDecodeDataUrl (Url, Out);
  }

  /* Network fetch: either Url is absolute http(s), or it's relative
   * and we have an http(s) base to resolve against. Anything else
   * (file://, relative with file base, mailto:, ...) is out of scope
   * for this decoder; v1 silently returns false and the caller falls
   * back to the checker placeholder. */
  if (IsHttpish (Url)) {
    Target = Url;
  } else if (BaseUrl != DFBR_NULL && IsHttpish (BaseUrl)) {
    Abs = DfBrUrlResolve (BaseUrl, Url);
    if (Abs == DFBR_NULL || !IsHttpish (Abs)) {
      if (Abs != DFBR_NULL) free (Abs);
      return DFBR_FALSE;
    }
    Target = Abs;
  } else {
    return DFBR_FALSE;
  }

  memset (&Resp, 0, sizeof (Resp));
#ifdef DFBR_IMAGE_TRACE
  fprintf (stdout, "[img-fetch] GET %s\n", Target);
  fflush  (stdout);
#endif

  Rc = DfBrHttpGet (Target, &Resp);
  if (Rc == DFBR_OK && Resp.Body != DFBR_NULL && Resp.BodyLen > 0 &&
      Resp.StatusCode >= 200 && Resp.StatusCode < 300)
  {
    Ok = ImageDecodeWithMime (Resp.Body, Resp.BodyLen, Resp.ContentType, Out);
#ifdef DFBR_IMAGE_TRACE
    fprintf (stdout, "[img-fetch] %s -> %s (%u bytes %s)\n",
             Target,
             Ok ? "decoded" : "decode failed",
             (unsigned)Resp.BodyLen,
             (Resp.ContentType[0] != '\0') ? Resp.ContentType : "no-ct");
#endif
  } else {
#ifdef DFBR_IMAGE_TRACE
    fprintf (stderr, "[img-fetch] %s failed (rc=%d, status=%u, bytes=%zu)\n",
             Target, (int)Rc,
             (unsigned)Resp.StatusCode, Resp.BodyLen);
#endif
  }
#ifdef DFBR_IMAGE_TRACE
  fflush (stdout);
#endif

  DfBrHttpResponseFree (&Resp);
  if (Abs != DFBR_NULL) free (Abs);
  return Ok;
}
