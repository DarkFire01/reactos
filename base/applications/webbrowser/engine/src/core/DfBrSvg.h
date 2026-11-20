/** @file
  DfBrSvg.h - minimal SVG 1.1 rasterizer.

  Ported from DevEnvPkg/Library/SvgLite (UEFI) to platform-neutral C,
  using the universal types in DfBrTypes.h. All UEFI dependencies
  (CHAR16, AllocatePool, EFI_STATUS, ...) have been removed; input is
  plain ASCII-safe UTF-8 since the SVG spec is itself ASCII-delimited.

  Supported:
    - Root:      <svg width height viewBox>
    - Shapes:    <rect> <circle> <ellipse> <line> <polygon> <polyline>
    - Paths:     <path d=...> with M/m L/l H/h V/v Z/z C/c Q/q S/s T/t
                 (arcs A/a are approximated by a straight line)
    - Grouping:  <g> with transform= translate / scale / rotate / matrix /
                 skewX / skewY  (composed as a 2x3 affine stack)
    - Paint:     fill, stroke, stroke-width, opacity, fill-opacity,
                 stroke-opacity, inline style=""
    - Colors:    #rgb, #rrggbb, rgb(), named CSS colors, "none",
                 "transparent", "currentColor" (falls back to black)

  Not supported (silently ignored): <text>, <use>, <defs>, <pattern>,
  gradients, filters, masks, stroke-dasharray / linecap / linejoin,
  fill-rule=evenodd (we always use non-zero winding via even-odd
  crossing count; good enough for typical icons).

  Output is an RGBA bitmap pre-composited over white (the downstream
  blit targets we care about don't honor alpha). Alpha in the result
  is always 0xFF; caller receives an opaque DfBrBitmap.
**/

#ifndef DFBR_SVG_H_
#define DFBR_SVG_H_

#include "../common/DfBrTypes.h"
#include "DfBrBitmap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  DfBrBitmap  Bmp;
} DfBrSvgBitmap;

/** Parse + rasterize an SVG document into a fresh bitmap.

    @param[in]  Source     NUL-terminated (or sized via SourceLen)
                           ASCII/UTF-8 bytes.
    @param[in]  SourceLen  Byte length; 0 means "strlen(Source)".
    @param[in]  TargetW    Desired output width in pixels. 0 = derive
                           from <svg width=...> or viewBox, falling back
                           to 300. Capped at 2048.
    @param[in]  TargetH    Same as TargetW, default 150.
    @param[out] Out        Filled in on success; release with DfBrSvgFree().

    @retval DFBR_OK                Rasterization succeeded.
    @retval DFBR_ERR_INVALID_ARG   Source or Out was NULL.
    @retval DFBR_ERR_NOT_FOUND     No <svg ...> root element was present.
    @retval DFBR_ERR_OUT_OF_MEMORY Allocation failed.
**/
DfBrResult
DfBrSvgRender (
  const char     *Source,
  usize           SourceLen,
  u32             TargetW,
  u32             TargetH,
  DfBrSvgBitmap  *Out
  );

/** Release pixels owned by a DfBrSvgBitmap. Safe with NULL/zeroed. **/
void
DfBrSvgFree (
  DfBrSvgBitmap  *Bmp
  );

#ifdef __cplusplus
}
#endif

#endif /* DFBR_SVG_H_ */
