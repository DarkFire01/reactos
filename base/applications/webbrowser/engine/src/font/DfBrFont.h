/** @file
  DfBrFont.h - engine-agnostic font + text measurement API for
               DefyBrowser.

  Design intent
  =============
  One thin interface, one process-wide default engine, many possible
  backends. Layout + paint consume this header; they never know which
  concrete font system is servicing the calls.

    * Windows (today)  - DirectWrite (DfBrFontDWrite).
    * UEFI    (planned) - stb_truetype against the embedded font
                          packed into the firmware image.
    * Linux / SDL (planned) - FreeType or stb_truetype.

  Picking a backend is a link-time decision: whichever implementation
  library is linked into the executable provides
  DfBrFontGetDefaultEngine(). Layout/paint stay identical.

  What this API gives you:
    * Lazy process-wide engine (a singleton in the backend).
    * Face resolution for a prioritized family list + weight + style +
      size, with a sensible fallback when no family matches.
    * Horizontal advance for a UTF-8 text run at that face.
    * Per-face line metrics (ascent / descent / line-gap / x-height /
      cap-height) in CSS pixels, already scaled to the requested size.

  What this API deliberately does NOT do yet:
    * Glyph rasterization. That arrives with the paint pass; the
      backend will grow a DfBrFontFaceRasterizeRun() that produces
      8-bit alpha coverage tiles.
    * Shaping / OpenType features / complex scripts. The v1 measure
      call uses the backend's default shaper (DirectWrite does the
      right thing; stb_truetype will not).
    * Bidi / mirroring. We assume LTR for now.

  All public symbols follow project convention: DfBr prefix, PascalCase
  types/functions, universal numeric types.
**/

#ifndef DFBR_FONT_H_
#define DFBR_FONT_H_

#include "../common/DfBrTypes.h"
#include "../core/DfBrBitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DfBrFontEngine DfBrFontEngine;
typedef struct DfBrFontFace   DfBrFontFace;

typedef enum {
  DFBR_FONT_STYLE_NORMAL,
  DFBR_FONT_STYLE_ITALIC,
  DFBR_FONT_STYLE_OBLIQUE
} DfBrFontStyle;

/** Per-face line metrics, already scaled from the font's design units
    into CSS pixels at the face's size. All values are non-negative.
    Ascent and descent are measured from the baseline (positive). **/
typedef struct {
  f32  AscentPx;
  f32  DescentPx;
  f32  LineGapPx;
  f32  XHeightPx;
  f32  CapHeightPx;
} DfBrFontMetrics;

/** Lazily create (first call) and return the process-wide default
    font engine. Returns NULL if no backend is linked or the backend
    fails to initialise. The engine is never torn down; it lives for
    the duration of the process. **/
DfBrFontEngine *
DfBrFontGetDefaultEngine (void);

/** Resolve a face for the given font request.
    `Families` is an ordered preference list (CSS-like). The first
    installed family wins; if none is installed, a system-appropriate
    fallback is used so that text always has *something* to measure.
    `Weight` is the CSS weight (100..900; 400 is regular, 700 bold).
    `SizePx` is the used font-size in CSS pixels.

    The returned face is owned and cached by the engine. The same
    (families, weight, style, size) request returns the same pointer.
    Returns NULL only if no backend / fallback is available. **/
DfBrFontFace *
DfBrFontEngineResolve (
  DfBrFontEngine    *Engine,
  const char *const *Families,
  usize              FamilyCount,
  i32                Weight,
  DfBrFontStyle      Style,
  f32                SizePx
  );

/** Measure the horizontal advance, in CSS pixels, of the UTF-8 run
    `[Utf8, Utf8+Len)` when laid out on a single unbroken line. **/
f32
DfBrFontFaceMeasureUtf8 (
  DfBrFontFace  *Face,
  const char    *Utf8,
  usize          Len
  );

/** Read back the per-face line metrics. Safe to call from layout. **/
void
DfBrFontFaceGetMetrics (
  const DfBrFontFace   *Face,
  DfBrFontMetrics      *Out
  );

/** Rasterize and blend a UTF-8 run into a BGRA bitmap.

    The pen starts at (PenX, BaselineY) in CSS pixels, where
    BaselineY is the alphabetic baseline - i.e. the text's bottom edge
    sits a little below (at BaselineY + DescentPx) and the top sits at
    (BaselineY - AscentPx). `Color` is blended source-over onto the
    destination using per-pixel alpha coverage from the rasterizer
    (grayscale AA on the DWrite backend).

    This is the glyph-drawing entry point paint uses for text; it's
    deliberately shaped like a simple "putText" call so alternate
    backends (stb_truetype, FreeType) can implement the same contract
    without growing a multi-step GlyphRun API. **/
void
DfBrFontFaceDrawUtf8 (
  DfBrFontFace  *Face,
  const char    *Utf8,
  usize          Len,
  DfBrBitmap    *Target,
  f32            PenX,
  f32            BaselineY,
  DfBrColor      Color
  );

#ifdef __cplusplus
}
#endif

#endif /* DFBR_FONT_H_ */
