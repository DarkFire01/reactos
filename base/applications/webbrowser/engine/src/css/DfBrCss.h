/** @file
  DfBrCss.h - CSS tokenizer + parser + cascade + computed-style
              resolver for DefyBrowser.

  Design intent
  =============
  DfBrCss is the second pillar of the layout engine, sitting
  immediately above DfBrHtml. When the CSS pipeline runs on a parsed
  HTML document, every element ends up with a fully-resolved
  DfBrCssComputedStyle hanging off DfBrHtmlNode::StyleData. Layout +
  paint will read computed styles directly, never touching the raw
  rule tree.

  The design is deliberately split into small composable modules so
  the library can grow into a real browser's CSS stack without a
  ground-up rewrite:

    1. **Spec-shaped tokenizer.** DfBrCssTokenize.c implements the CSS
       Syntax Module Level 3 tokenizer verbatim - ident, function,
       at-keyword, hash, string, url, number, percentage, dimension,
       delim, CDO/CDC, and all the bracket / punctuation tokens. New
       tokens are one switch case.

    2. **Component values.** Declarations and at-rule preludes are
       stored as linked lists of DfBrCssCv nodes so shorthand
       expansion, calc(), and future value-level features can all
       work against the same structure.

    3. **Selector engine.** DfBrCssSelector.c parses compound +
       complex selectors (type / universal / id / class / attr /
       pseudo-class, with descendant / child / adjacent / sibling
       combinators). Specificity is packed as a,b,c into a u32 at
       parse time.

    4. **Property table.** DfBrCssProps.c is a single static table
       keyed by DfBrCssProp. Each entry carries the canonical name,
       the inherited flag, and (eventually) the specified-value
       parser. Adding a new property is an additive edit.

    5. **Cascade + inheritance.** DfBrCssCascade.c collects every
       matching declaration for every element, sorts by
       (origin, importance, specificity, source order), and applies
       them to a DfBrCssComputedStyle. Inherited properties fall
       back to the parent element's value; non-inherited ones fall
       back to the initial value defined in the property table.

    6. **Baked UA sheet.** DfBrCssUserAgent.c ships a compiled-in
       user-agent stylesheet with the baseline display modes for
       HTML elements. It is parsed once at program start, shared
       across documents.

    7. **Arena allocation + error tolerance**, same as DfBrHtml. A
       CSS parse never returns NULL on malformed input; errors go
       into a diagnostics list and the parser attempts to recover
       at the next semicolon / block boundary.

    8. **Engine-agnostic.** This header pulls in DfBrTypes.h and
       DfBrHtml.h only; no platform, no paint, no layout types
       leak in.

  Deliberate v1 gaps (tracked in the roadmap):
    - Adoption of the whole CSS-Color-4 / CSS-Values-4 pipeline.
      We parse the Level-3 subset browsers encounter in the wild.
    - calc(), var(), and other functional value pipelines (parsed
      as opaque functions, not evaluated).
    - @media feature evaluation (parsed, preserved, not applied).
    - @keyframes animation playback (parsed, preserved).
    - Page box / @page / paged media.
    - Full CSS Selector Level 4 pseudo-class complement (we cover
      the Level-3 set plus :is / :where / :not(simple)).
**/

#ifndef DFBR_CSS_H_
#define DFBR_CSS_H_

#include "../common/DfBrTypes.h"
#include "../html/DfBrHtml.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 *  Origin
 *
 *  The cascade sorts declarations by origin. In a real browser this
 *  is (user-agent, user, author) with specific !important rules;
 *  DfBrCss maps "user" = "author" since we don't expose a user sheet
 *  yet, but the enum shape is compatible so the sort logic does not
 *  change when we add one.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_CSS_ORIGIN_UA      = 0,
  DFBR_CSS_ORIGIN_USER    = 1,
  DFBR_CSS_ORIGIN_AUTHOR  = 2
} DfBrCssOrigin;

/* ------------------------------------------------------------------
 *  Property enum
 *
 *  Every property we recognise at parse time. Unknown properties are
 *  preserved verbatim on the declaration (string name, value token
 *  stream) but do not contribute to the cascade.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_CSS_PROP_UNKNOWN = 0,

  /* Display / visibility / layering */
  DFBR_CSS_PROP_DISPLAY,
  DFBR_CSS_PROP_POSITION,
  DFBR_CSS_PROP_FLOAT,
  DFBR_CSS_PROP_CLEAR,
  DFBR_CSS_PROP_VISIBILITY,
  DFBR_CSS_PROP_OVERFLOW,
  DFBR_CSS_PROP_OVERFLOW_X,
  DFBR_CSS_PROP_OVERFLOW_Y,
  DFBR_CSS_PROP_BOX_SIZING,
  DFBR_CSS_PROP_Z_INDEX,
  DFBR_CSS_PROP_OPACITY,

  /* Box model */
  DFBR_CSS_PROP_WIDTH,
  DFBR_CSS_PROP_HEIGHT,
  DFBR_CSS_PROP_MIN_WIDTH,
  DFBR_CSS_PROP_MIN_HEIGHT,
  DFBR_CSS_PROP_MAX_WIDTH,
  DFBR_CSS_PROP_MAX_HEIGHT,
  DFBR_CSS_PROP_TOP,
  DFBR_CSS_PROP_RIGHT,
  DFBR_CSS_PROP_BOTTOM,
  DFBR_CSS_PROP_LEFT,

  DFBR_CSS_PROP_MARGIN,
  DFBR_CSS_PROP_MARGIN_TOP,
  DFBR_CSS_PROP_MARGIN_RIGHT,
  DFBR_CSS_PROP_MARGIN_BOTTOM,
  DFBR_CSS_PROP_MARGIN_LEFT,

  DFBR_CSS_PROP_PADDING,
  DFBR_CSS_PROP_PADDING_TOP,
  DFBR_CSS_PROP_PADDING_RIGHT,
  DFBR_CSS_PROP_PADDING_BOTTOM,
  DFBR_CSS_PROP_PADDING_LEFT,

  DFBR_CSS_PROP_BORDER,
  DFBR_CSS_PROP_BORDER_WIDTH,
  DFBR_CSS_PROP_BORDER_STYLE,
  DFBR_CSS_PROP_BORDER_COLOR,
  DFBR_CSS_PROP_BORDER_TOP,
  DFBR_CSS_PROP_BORDER_RIGHT,
  DFBR_CSS_PROP_BORDER_BOTTOM,
  DFBR_CSS_PROP_BORDER_LEFT,
  DFBR_CSS_PROP_BORDER_TOP_WIDTH,
  DFBR_CSS_PROP_BORDER_RIGHT_WIDTH,
  DFBR_CSS_PROP_BORDER_BOTTOM_WIDTH,
  DFBR_CSS_PROP_BORDER_LEFT_WIDTH,
  DFBR_CSS_PROP_BORDER_TOP_STYLE,
  DFBR_CSS_PROP_BORDER_RIGHT_STYLE,
  DFBR_CSS_PROP_BORDER_BOTTOM_STYLE,
  DFBR_CSS_PROP_BORDER_LEFT_STYLE,
  DFBR_CSS_PROP_BORDER_TOP_COLOR,
  DFBR_CSS_PROP_BORDER_RIGHT_COLOR,
  DFBR_CSS_PROP_BORDER_BOTTOM_COLOR,
  DFBR_CSS_PROP_BORDER_LEFT_COLOR,
  DFBR_CSS_PROP_BORDER_RADIUS,

  /* Background */
  DFBR_CSS_PROP_BACKGROUND,
  DFBR_CSS_PROP_BACKGROUND_COLOR,
  DFBR_CSS_PROP_BACKGROUND_IMAGE,
  DFBR_CSS_PROP_BACKGROUND_REPEAT,
  DFBR_CSS_PROP_BACKGROUND_POSITION,
  DFBR_CSS_PROP_BACKGROUND_SIZE,
  DFBR_CSS_PROP_BACKGROUND_ATTACHMENT,
  DFBR_CSS_PROP_BACKGROUND_CLIP,
  DFBR_CSS_PROP_BACKGROUND_ORIGIN,

  /* Color + text */
  DFBR_CSS_PROP_COLOR,
  DFBR_CSS_PROP_FONT,
  DFBR_CSS_PROP_FONT_FAMILY,
  DFBR_CSS_PROP_FONT_SIZE,
  DFBR_CSS_PROP_FONT_WEIGHT,
  DFBR_CSS_PROP_FONT_STYLE,
  DFBR_CSS_PROP_FONT_VARIANT,
  DFBR_CSS_PROP_LINE_HEIGHT,
  DFBR_CSS_PROP_TEXT_ALIGN,
  DFBR_CSS_PROP_TEXT_DECORATION,
  DFBR_CSS_PROP_TEXT_TRANSFORM,
  DFBR_CSS_PROP_TEXT_INDENT,
  DFBR_CSS_PROP_LETTER_SPACING,
  DFBR_CSS_PROP_WORD_SPACING,
  DFBR_CSS_PROP_WHITE_SPACE,
  DFBR_CSS_PROP_WORD_BREAK,
  DFBR_CSS_PROP_OVERFLOW_WRAP,
  DFBR_CSS_PROP_VERTICAL_ALIGN,

  /* List */
  DFBR_CSS_PROP_LIST_STYLE,
  DFBR_CSS_PROP_LIST_STYLE_TYPE,
  DFBR_CSS_PROP_LIST_STYLE_POSITION,
  DFBR_CSS_PROP_LIST_STYLE_IMAGE,

  /* Table */
  DFBR_CSS_PROP_BORDER_COLLAPSE,
  DFBR_CSS_PROP_BORDER_SPACING,
  DFBR_CSS_PROP_CAPTION_SIDE,
  DFBR_CSS_PROP_EMPTY_CELLS,
  DFBR_CSS_PROP_TABLE_LAYOUT,

  /* Flex */
  DFBR_CSS_PROP_FLEX,
  DFBR_CSS_PROP_FLEX_DIRECTION,
  DFBR_CSS_PROP_FLEX_WRAP,
  DFBR_CSS_PROP_FLEX_FLOW,
  DFBR_CSS_PROP_FLEX_GROW,
  DFBR_CSS_PROP_FLEX_SHRINK,
  DFBR_CSS_PROP_FLEX_BASIS,
  DFBR_CSS_PROP_JUSTIFY_CONTENT,
  DFBR_CSS_PROP_ALIGN_ITEMS,
  DFBR_CSS_PROP_ALIGN_SELF,
  DFBR_CSS_PROP_ALIGN_CONTENT,
  DFBR_CSS_PROP_GAP,
  DFBR_CSS_PROP_ROW_GAP,
  DFBR_CSS_PROP_COLUMN_GAP,
  DFBR_CSS_PROP_ORDER,

  /* Misc */
  DFBR_CSS_PROP_CURSOR,
  DFBR_CSS_PROP_TRANSFORM,
  DFBR_CSS_PROP_TRANSFORM_ORIGIN,
  DFBR_CSS_PROP_TRANSITION,
  DFBR_CSS_PROP_ANIMATION,
  DFBR_CSS_PROP_CONTENT,
  DFBR_CSS_PROP_QUOTES,
  DFBR_CSS_PROP_OUTLINE,
  DFBR_CSS_PROP_OUTLINE_COLOR,
  DFBR_CSS_PROP_OUTLINE_STYLE,
  DFBR_CSS_PROP_OUTLINE_WIDTH,
  DFBR_CSS_PROP_BOX_SHADOW,

  DFBR_CSS_PROP_COUNT
} DfBrCssProp;

/* ------------------------------------------------------------------
 *  Keyword enum
 *
 *  All keyword values we care about for cascade / matching. The
 *  tokenizer produces idents; the value parser maps them to one of
 *  these, letting the computed-style resolver use integer compares.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_CSS_KW_UNKNOWN = 0,

  /* CSS-wide */
  DFBR_CSS_KW_INHERIT,
  DFBR_CSS_KW_INITIAL,
  DFBR_CSS_KW_UNSET,
  DFBR_CSS_KW_REVERT,

  /* Display */
  DFBR_CSS_KW_BLOCK,
  DFBR_CSS_KW_INLINE,
  DFBR_CSS_KW_INLINE_BLOCK,
  DFBR_CSS_KW_INLINE_FLEX,
  DFBR_CSS_KW_INLINE_TABLE,
  DFBR_CSS_KW_NONE,
  DFBR_CSS_KW_FLEX,
  DFBR_CSS_KW_GRID,
  DFBR_CSS_KW_LIST_ITEM,
  DFBR_CSS_KW_TABLE,
  DFBR_CSS_KW_TABLE_ROW,
  DFBR_CSS_KW_TABLE_CELL,
  DFBR_CSS_KW_TABLE_ROW_GROUP,
  DFBR_CSS_KW_TABLE_HEADER_GROUP,
  DFBR_CSS_KW_TABLE_FOOTER_GROUP,
  DFBR_CSS_KW_TABLE_COLUMN,
  DFBR_CSS_KW_TABLE_COLUMN_GROUP,
  DFBR_CSS_KW_TABLE_CAPTION,
  DFBR_CSS_KW_RUN_IN,
  DFBR_CSS_KW_CONTENTS,

  /* Position */
  DFBR_CSS_KW_STATIC,
  DFBR_CSS_KW_RELATIVE,
  DFBR_CSS_KW_ABSOLUTE,
  DFBR_CSS_KW_FIXED,
  DFBR_CSS_KW_STICKY,

  /* Float / clear */
  DFBR_CSS_KW_LEFT,
  DFBR_CSS_KW_RIGHT,
  DFBR_CSS_KW_BOTH,
  DFBR_CSS_KW_INLINE_START,
  DFBR_CSS_KW_INLINE_END,

  /* Visibility */
  DFBR_CSS_KW_VISIBLE,
  DFBR_CSS_KW_HIDDEN,
  DFBR_CSS_KW_COLLAPSE,

  /* Overflow */
  DFBR_CSS_KW_SCROLL,
  DFBR_CSS_KW_AUTO,
  DFBR_CSS_KW_CLIP,

  /* Box sizing / background-clip / background-origin */
  DFBR_CSS_KW_CONTENT_BOX,
  DFBR_CSS_KW_BORDER_BOX,
  DFBR_CSS_KW_PADDING_BOX,
  DFBR_CSS_KW_TEXT,

  /* Text align */
  DFBR_CSS_KW_CENTER,
  DFBR_CSS_KW_JUSTIFY,
  DFBR_CSS_KW_START,
  DFBR_CSS_KW_END,

  /* Text decoration */
  DFBR_CSS_KW_UNDERLINE,
  DFBR_CSS_KW_OVERLINE,
  DFBR_CSS_KW_LINE_THROUGH,
  DFBR_CSS_KW_BLINK,

  /* Font style */
  DFBR_CSS_KW_NORMAL,
  DFBR_CSS_KW_ITALIC,
  DFBR_CSS_KW_OBLIQUE,

  /* Font weight */
  DFBR_CSS_KW_BOLD,
  DFBR_CSS_KW_BOLDER,
  DFBR_CSS_KW_LIGHTER,

  /* White-space */
  DFBR_CSS_KW_NOWRAP,
  DFBR_CSS_KW_PRE,
  DFBR_CSS_KW_PRE_WRAP,
  DFBR_CSS_KW_PRE_LINE,
  DFBR_CSS_KW_BREAK_SPACES,

  /* Border styles */
  DFBR_CSS_KW_SOLID,
  DFBR_CSS_KW_DOTTED,
  DFBR_CSS_KW_DASHED,
  DFBR_CSS_KW_DOUBLE,
  DFBR_CSS_KW_GROOVE,
  DFBR_CSS_KW_RIDGE,
  DFBR_CSS_KW_INSET,
  DFBR_CSS_KW_OUTSET,
  DFBR_CSS_KW_HIDDEN_BORDER,

  /* Line-height / font-size keywords */
  DFBR_CSS_KW_THIN,
  DFBR_CSS_KW_MEDIUM,
  DFBR_CSS_KW_THICK,
  DFBR_CSS_KW_SMALLER,
  DFBR_CSS_KW_LARGER,
  DFBR_CSS_KW_XX_SMALL,
  DFBR_CSS_KW_X_SMALL,
  DFBR_CSS_KW_SMALL,
  DFBR_CSS_KW_LARGE,
  DFBR_CSS_KW_X_LARGE,
  DFBR_CSS_KW_XX_LARGE,

  /* Flex */
  DFBR_CSS_KW_ROW,
  DFBR_CSS_KW_ROW_REVERSE,
  DFBR_CSS_KW_COLUMN,
  DFBR_CSS_KW_COLUMN_REVERSE,
  DFBR_CSS_KW_WRAP,
  DFBR_CSS_KW_WRAP_REVERSE,
  DFBR_CSS_KW_FLEX_START,
  DFBR_CSS_KW_FLEX_END,
  DFBR_CSS_KW_SPACE_BETWEEN,
  DFBR_CSS_KW_SPACE_AROUND,
  DFBR_CSS_KW_SPACE_EVENLY,
  DFBR_CSS_KW_STRETCH,
  DFBR_CSS_KW_BASELINE,

  /* Colors (common) */
  DFBR_CSS_KW_TRANSPARENT,
  DFBR_CSS_KW_CURRENTCOLOR,

  /* List style */
  DFBR_CSS_KW_DISC,
  DFBR_CSS_KW_CIRCLE,
  DFBR_CSS_KW_SQUARE,
  DFBR_CSS_KW_DECIMAL,
  DFBR_CSS_KW_DECIMAL_LEADING_ZERO,
  DFBR_CSS_KW_LOWER_ROMAN,
  DFBR_CSS_KW_UPPER_ROMAN,
  DFBR_CSS_KW_LOWER_ALPHA,
  DFBR_CSS_KW_UPPER_ALPHA,
  DFBR_CSS_KW_INSIDE,
  DFBR_CSS_KW_OUTSIDE,

  /* Background repeat */
  DFBR_CSS_KW_REPEAT,
  DFBR_CSS_KW_REPEAT_X,
  DFBR_CSS_KW_REPEAT_Y,
  DFBR_CSS_KW_NO_REPEAT,
  DFBR_CSS_KW_SPACE,
  DFBR_CSS_KW_ROUND,
  DFBR_CSS_KW_COVER,
  DFBR_CSS_KW_CONTAIN,

  /* Cursor (subset) */
  DFBR_CSS_KW_POINTER,
  DFBR_CSS_KW_DEFAULT_KW,
  DFBR_CSS_KW_TEXT_CURSOR,

  /* Gradient direction keywords ('to top', 'to bottom left', ...).
   * TOP/LEFT/RIGHT/BOTTOM already exist above; these are the corner
   * composites that can appear after `to`. */
  DFBR_CSS_KW_TO,

  /* Text-transform */
  DFBR_CSS_KW_UPPERCASE,
  DFBR_CSS_KW_LOWERCASE,
  DFBR_CSS_KW_CAPITALIZE,
  DFBR_CSS_KW_FULL_WIDTH,

  /* Vertical align */
  DFBR_CSS_KW_TOP,
  DFBR_CSS_KW_MIDDLE,
  DFBR_CSS_KW_BOTTOM,
  DFBR_CSS_KW_SUB,
  DFBR_CSS_KW_SUPER,
  DFBR_CSS_KW_TEXT_TOP,
  DFBR_CSS_KW_TEXT_BOTTOM,

  /* Border collapse / table */
  DFBR_CSS_KW_COLLAPSE_TBL,
  DFBR_CSS_KW_SEPARATE,

  DFBR_CSS_KW_COUNT
} DfBrCssKeyword;

/* ------------------------------------------------------------------
 *  Units
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_CSS_UNIT_NONE = 0,    /* raw <number> */
  /* Absolute lengths */
  DFBR_CSS_UNIT_PX,
  DFBR_CSS_UNIT_PT,
  DFBR_CSS_UNIT_PC,
  DFBR_CSS_UNIT_CM,
  DFBR_CSS_UNIT_MM,
  DFBR_CSS_UNIT_Q,
  DFBR_CSS_UNIT_IN,
  /* Font-relative */
  DFBR_CSS_UNIT_EM,
  DFBR_CSS_UNIT_REM,
  DFBR_CSS_UNIT_EX,
  DFBR_CSS_UNIT_CH,
  /* Viewport */
  DFBR_CSS_UNIT_VW,
  DFBR_CSS_UNIT_VH,
  DFBR_CSS_UNIT_VMIN,
  DFBR_CSS_UNIT_VMAX,
  /* Angle */
  DFBR_CSS_UNIT_DEG,
  DFBR_CSS_UNIT_RAD,
  DFBR_CSS_UNIT_GRAD,
  DFBR_CSS_UNIT_TURN,
  /* Time */
  DFBR_CSS_UNIT_S,
  DFBR_CSS_UNIT_MS,
  /* Resolution */
  DFBR_CSS_UNIT_DPI,
  DFBR_CSS_UNIT_DPCM,
  DFBR_CSS_UNIT_DPPX,
  /* Percent */
  DFBR_CSS_UNIT_PERCENT,

  DFBR_CSS_UNIT_UNKNOWN   /* preserved but not convertible */
} DfBrCssUnit;

/* ------------------------------------------------------------------
 *  Color
 *
 *  Stored in sRGB with a f32 alpha in [0,1]. Layout + paint consume
 *  this directly; the tokenizer's parsed values (hex / rgb / hsl /
 *  named) all resolve to this struct.
 * -----------------------------------------------------------------*/

typedef struct {
  u8   R;
  u8   G;
  u8   B;
  f32  A;
} DfBrCssColor;

/* ------------------------------------------------------------------
 *  Length
 *
 *  A pair (value, unit). Percents are a distinct unit because they
 *  need a layout-context resolution later. The special value "auto"
 *  is encoded by setting Unit = DFBR_CSS_UNIT_NONE and Value = NaN;
 *  use DfBrCssLengthIsAuto() to test.
 * -----------------------------------------------------------------*/

typedef struct {
  f32          Value;
  DfBrCssUnit  Unit;
  b32          Auto;    /* true => "auto" keyword, Value/Unit unused */

  /* Deferred calc()/min()/max()/clamp() expression. When non-NULL the
   * layout-time resolver (DfBrCssLengthToPx) walks the CV tree against
   * the active layout context instead of using Value/Unit. Owned by
   * the originating stylesheet's arena, so it outlives the
   * DfBrCssComputedStyle. */
  const struct DfBrCssCv *CalcExpr;
} DfBrCssLength;

/* ------------------------------------------------------------------
 *  Image values
 *
 *  CSS <image> covers url(...) and the gradient functions. We currently
 *  model linear-gradient() directly (mirroring the shape Ladybird's
 *  LibWeb keeps in CSS::LinearGradientStyleValue) so paint can sample
 *  it per-pixel without another layer of indirection. Radial /
 *  conic gradients can plug into the same DfBrCssGradientKind tag
 *  without touching consumers that only care about linear.
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssColor  Color;
  b32           ColorIsCurrent;  /* currentColor; resolve at paint time */
  f32           Position;        /* 0.0 .. 1.0; authored or auto-placed */
  b32           PositionAuto;    /* true when the stop had no explicit position */
} DfBrCssColorStop;

typedef enum {
  DFBR_CSS_GRADIENT_LINEAR
  /* Future: RADIAL, CONIC. Each carries its own extra state plus the
   * shared stop array below. */
} DfBrCssGradientKind;

typedef struct {
  DfBrCssGradientKind  Kind;
  /* Angle in radians, measured CSS-style: 0 = to top, positive values
   * rotate clockwise (90deg = to right). `to <side>` / `to <corner>`
   * forms are collapsed into this angle at parse time. */
  f32                  AngleRad;
  b32                  Repeating;   /* repeating-linear-gradient */
  /* Sorted ascending by Position. Heap of PositionAuto stops are
   * pre-resolved during parse using the CSS Images L3 algorithm so
   * the paint path never has to re-distribute. */
  DfBrCssColorStop    *Stops;       /* arena-owned (StyleContext) */
  usize                StopCount;
} DfBrCssGradient;

typedef enum {
  DFBR_CSS_BGCLIP_BORDER_BOX,
  DFBR_CSS_BGCLIP_PADDING_BOX,
  DFBR_CSS_BGCLIP_CONTENT_BOX,
  /* background-clip: text - the defacto modern standard (prefix-less
   * since Chrome 120 / FF 119 / Safari 15). When combined with
   * `color: transparent` this is how every landing page on the
   * modern web paints multi-color wordmarks. */
  DFBR_CSS_BGCLIP_TEXT
} DfBrCssBgClip;

/* ------------------------------------------------------------------
 *  Forward declarations
 * -----------------------------------------------------------------*/

typedef struct DfBrCssStylesheet    DfBrCssStylesheet;
typedef struct DfBrCssRule          DfBrCssRule;
typedef struct DfBrCssSelectorList  DfBrCssSelectorList;
typedef struct DfBrCssCv            DfBrCssCv;
typedef struct DfBrCssDeclaration   DfBrCssDeclaration;
typedef struct DfBrCssComputedStyle DfBrCssComputedStyle;

/* A single `box-shadow` entry in a comma-separated list. `Inset`
 * toggles outer vs. inner shadow; `Color` uses the element's
 * `color` when the declaration omitted a color (`ColorIsCurrent`).
 * The shadow list is owned by the style-context arena and its
 * `Next` chains declarations in author order. */
typedef struct DfBrCssShadow {
  DfBrCssLength          OffsetX;
  DfBrCssLength          OffsetY;
  DfBrCssLength          Blur;
  DfBrCssLength          Spread;
  DfBrCssColor           Color;
  b32                    ColorIsCurrent;
  b32                    Inset;
  struct DfBrCssShadow  *Next;
} DfBrCssShadow;
typedef struct DfBrCssStyleContext  DfBrCssStyleContext;
typedef struct DfBrCssCustomProp    DfBrCssCustomProp;

/* ------------------------------------------------------------------
 *  Diagnostics
 * -----------------------------------------------------------------*/

typedef struct {
  i32          Line;
  i32          Column;
  const char  *Message;    /* arena-owned; lives with the sheet */
} DfBrCssDiagEntry;

/* ------------------------------------------------------------------
 *  Parse options
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssOrigin  Origin;           /* default: AUTHOR */
  b32            KeepUnknownProps; /* preserve unknown decls on rules (default TRUE) */
  b32            EmitSourceLocations; /* default TRUE */
} DfBrCssParseOptions;

/* ------------------------------------------------------------------
 *  Public API - stylesheet parsing
 * -----------------------------------------------------------------*/

/** Parse a CSS stylesheet.
    Never returns NULL unless allocation fails. Malformed input
    surfaces in DfBrCssStylesheetDiag(). **/
DfBrCssStylesheet *
DfBrCssParse (
  const char                 *Source,
  usize                       SourceLen,
  const DfBrCssParseOptions  *Opts
  );

void  DfBrCssStylesheetDestroy (DfBrCssStylesheet *Sheet);

/* Access rules + diagnostics. */
const DfBrCssRule       *DfBrCssStylesheetFirstRule (const DfBrCssStylesheet *S);
DfBrCssOrigin            DfBrCssStylesheetOrigin    (const DfBrCssStylesheet *S);
usize                    DfBrCssStylesheetDiagCount (const DfBrCssStylesheet *S);
const DfBrCssDiagEntry  *DfBrCssStylesheetDiag      (const DfBrCssStylesheet *S, usize Idx);

/* Rule introspection. */
typedef enum {
  DFBR_CSS_RULE_QUALIFIED,   /* selector { decls } */
  DFBR_CSS_RULE_AT           /* @keyword ... ; or @keyword ... { block } */
} DfBrCssRuleKind;

DfBrCssRuleKind             DfBrCssRuleType       (const DfBrCssRule *R);
const DfBrCssSelectorList  *DfBrCssRuleSelectors  (const DfBrCssRule *R);
const DfBrCssDeclaration   *DfBrCssRuleFirstDecl  (const DfBrCssRule *R);
const char                 *DfBrCssRuleAtName     (const DfBrCssRule *R, usize *OutLen);
const DfBrCssCv            *DfBrCssRuleAtPrelude  (const DfBrCssRule *R);
const DfBrCssRule          *DfBrCssRuleInnerRules (const DfBrCssRule *R);
const DfBrCssRule          *DfBrCssRuleNext       (const DfBrCssRule *R);

/** If `R` is an `@import` rule, returns the referenced URL as a
    heap-owned NUL-terminated C string (caller must free()). Accepts
    both `@import "foo.css"` and `@import url(foo.css)` forms.
    Returns NULL when `R` is anything other than `@import` or the
    prelude is missing/malformed. **/
char                       *DfBrCssRuleImportUrl  (const DfBrCssRule *R);

/* Declaration introspection. */
DfBrCssProp                DfBrCssDeclProp      (const DfBrCssDeclaration *D);
const char                *DfBrCssDeclName      (const DfBrCssDeclaration *D, usize *OutLen);
const DfBrCssCv           *DfBrCssDeclValue     (const DfBrCssDeclaration *D);
b32                        DfBrCssDeclImportant (const DfBrCssDeclaration *D);
const DfBrCssDeclaration  *DfBrCssDeclNext      (const DfBrCssDeclaration *D);

/* ------------------------------------------------------------------
 *  Property metadata
 * -----------------------------------------------------------------*/

DfBrCssProp  DfBrCssPropFromName (const char *Name, usize Len);
const char  *DfBrCssPropName     (DfBrCssProp P);
b32          DfBrCssPropInherits (DfBrCssProp P);
b32          DfBrCssPropIsShorthand (DfBrCssProp P);

/* ------------------------------------------------------------------
 *  Keyword metadata
 * -----------------------------------------------------------------*/

DfBrCssKeyword  DfBrCssKeywordFromName (const char *Name, usize Len);
const char     *DfBrCssKeywordName     (DfBrCssKeyword K);

/* ------------------------------------------------------------------
 *  Selector queries
 *
 *  Thin, public wrappers around DfBrCssSelector + DfBrCssMatch so
 *  bindings (JS, eventually an accessibility tree walker) don't have
 *  to include internal CSS headers. Each query parses the selector
 *  string into a temporary arena, walks the DOM in pre-order, and
 *  returns matched nodes. The arena is freed when the result is
 *  destroyed, so node pointers stored in the result remain valid for
 *  the life of the DOM, not the query.
 * -----------------------------------------------------------------*/

struct DfBrHtmlNode;

typedef struct DfBrCssQueryResult DfBrCssQueryResult;

/** Run a selector list against the subtree rooted at `Root`. Returns
    an opaque result; destroy it with DfBrCssQueryResultDestroy.
    Always non-NULL; invalid selectors yield an empty result. `SelLen`
    may be 0 to mean `strlen(Selector)`. **/
DfBrCssQueryResult *DfBrCssQuerySelectorAll (
  const struct DfBrHtmlNode  *Root,
  const char                 *Selector,
  usize                       SelLen
  );

usize                       DfBrCssQueryResultCount (const DfBrCssQueryResult *R);
const struct DfBrHtmlNode  *DfBrCssQueryResultAt    (const DfBrCssQueryResult *R, usize Idx);
void                        DfBrCssQueryResultDestroy (DfBrCssQueryResult *R);

/** Convenience: first matching node or NULL. **/
const struct DfBrHtmlNode *DfBrCssQuerySelector (
  const struct DfBrHtmlNode  *Root,
  const char                 *Selector,
  usize                       SelLen
  );

/* ------------------------------------------------------------------
 *  Named colors (CSS3: ~147 entries)
 * -----------------------------------------------------------------*/

/** Look up a named color. Returns DFBR_FALSE if Name is not a
    recognised CSS named color. Name matching is ASCII
    case-insensitive; length-prefixed so callers don't need NUL
    termination. **/
b32  DfBrCssColorFromName (const char *Name, usize Len, DfBrCssColor *Out);

/* Parse a color from a component-value list (hex / rgb / rgba / hsl /
 * hsla / named). Returns DFBR_FALSE on parse failure. Consumes only
 * as much of the CV list as needed; caller can continue walking. */
b32  DfBrCssColorFromCv (const DfBrCssCv *Cv, DfBrCssColor *Out);

/* ------------------------------------------------------------------
 *  Computed style (per-element resolved declarations)
 *
 *  Everything layout + paint needs is here. Lengths that require a
 *  layout context (percent, em, rem, vw, ...) stay as DfBrCssLength
 *  until the layout engine resolves them against a containing box.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_CSS_DISPLAY_NONE,
  DFBR_CSS_DISPLAY_BLOCK,
  DFBR_CSS_DISPLAY_INLINE,
  DFBR_CSS_DISPLAY_INLINE_BLOCK,
  DFBR_CSS_DISPLAY_LIST_ITEM,
  DFBR_CSS_DISPLAY_FLEX,
  DFBR_CSS_DISPLAY_INLINE_FLEX,
  DFBR_CSS_DISPLAY_GRID,
  DFBR_CSS_DISPLAY_TABLE,
  DFBR_CSS_DISPLAY_INLINE_TABLE,
  DFBR_CSS_DISPLAY_TABLE_ROW,
  DFBR_CSS_DISPLAY_TABLE_CELL,
  DFBR_CSS_DISPLAY_TABLE_ROW_GROUP,
  DFBR_CSS_DISPLAY_TABLE_HEADER_GROUP,
  DFBR_CSS_DISPLAY_TABLE_FOOTER_GROUP,
  DFBR_CSS_DISPLAY_TABLE_COLUMN,
  DFBR_CSS_DISPLAY_TABLE_COLUMN_GROUP,
  DFBR_CSS_DISPLAY_TABLE_CAPTION,
  DFBR_CSS_DISPLAY_RUN_IN,
  DFBR_CSS_DISPLAY_CONTENTS
} DfBrCssDisplay;

typedef enum {
  DFBR_CSS_POSITION_STATIC,
  DFBR_CSS_POSITION_RELATIVE,
  DFBR_CSS_POSITION_ABSOLUTE,
  DFBR_CSS_POSITION_FIXED,
  DFBR_CSS_POSITION_STICKY
} DfBrCssPosition;

typedef enum {
  DFBR_CSS_FLOAT_NONE,
  DFBR_CSS_FLOAT_LEFT,
  DFBR_CSS_FLOAT_RIGHT
} DfBrCssFloat;

typedef enum {
  DFBR_CSS_CLEAR_NONE,
  DFBR_CSS_CLEAR_LEFT,
  DFBR_CSS_CLEAR_RIGHT,
  DFBR_CSS_CLEAR_BOTH
} DfBrCssClear;

typedef enum {
  DFBR_CSS_VISIBLE,
  DFBR_CSS_HIDDEN,
  DFBR_CSS_COLLAPSE
} DfBrCssVisibility;

typedef enum {
  DFBR_CSS_OVERFLOW_VISIBLE,
  DFBR_CSS_OVERFLOW_HIDDEN,
  DFBR_CSS_OVERFLOW_SCROLL,
  DFBR_CSS_OVERFLOW_AUTO,
  DFBR_CSS_OVERFLOW_CLIP
} DfBrCssOverflowKind;

typedef enum {
  DFBR_CSS_BOX_CONTENT,
  DFBR_CSS_BOX_BORDER,
  DFBR_CSS_BOX_PADDING
} DfBrCssBoxSizing;

typedef enum {
  DFBR_CSS_WS_NORMAL,
  DFBR_CSS_WS_NOWRAP,
  DFBR_CSS_WS_PRE,
  DFBR_CSS_WS_PRE_WRAP,
  DFBR_CSS_WS_PRE_LINE,
  DFBR_CSS_WS_BREAK_SPACES
} DfBrCssWhiteSpace;

typedef enum {
  DFBR_CSS_TALIGN_START,
  DFBR_CSS_TALIGN_END,
  DFBR_CSS_TALIGN_LEFT,
  DFBR_CSS_TALIGN_RIGHT,
  DFBR_CSS_TALIGN_CENTER,
  DFBR_CSS_TALIGN_JUSTIFY
} DfBrCssTextAlign;

typedef enum {
  DFBR_CSS_TTRANSFORM_NONE,
  DFBR_CSS_TTRANSFORM_UPPERCASE,
  DFBR_CSS_TTRANSFORM_LOWERCASE,
  DFBR_CSS_TTRANSFORM_CAPITALIZE
} DfBrCssTextTransform;

typedef enum {
  DFBR_CSS_FSTYLE_NORMAL,
  DFBR_CSS_FSTYLE_ITALIC,
  DFBR_CSS_FSTYLE_OBLIQUE
} DfBrCssFontStyle;

typedef enum {
  DFBR_CSS_BORDER_NONE,
  DFBR_CSS_BORDER_HIDDEN,
  DFBR_CSS_BORDER_DOTTED,
  DFBR_CSS_BORDER_DASHED,
  DFBR_CSS_BORDER_SOLID,
  DFBR_CSS_BORDER_DOUBLE,
  DFBR_CSS_BORDER_GROOVE,
  DFBR_CSS_BORDER_RIDGE,
  DFBR_CSS_BORDER_INSET,
  DFBR_CSS_BORDER_OUTSET
} DfBrCssBorderStyleK;

typedef struct {
  DfBrCssLength        Width;
  DfBrCssBorderStyleK  Style;
  DfBrCssColor         Color;
  b32                  ColorIsCurrent;  /* currentColor */
} DfBrCssBorder;

typedef enum {
  DFBR_CSS_VALIGN_BASELINE,
  DFBR_CSS_VALIGN_SUB,
  DFBR_CSS_VALIGN_SUPER,
  DFBR_CSS_VALIGN_TOP,
  DFBR_CSS_VALIGN_MIDDLE,
  DFBR_CSS_VALIGN_BOTTOM,
  DFBR_CSS_VALIGN_TEXT_TOP,
  DFBR_CSS_VALIGN_TEXT_BOTTOM,
  DFBR_CSS_VALIGN_LENGTH        /* use VerticalAlignLength */
} DfBrCssVerticalAlign;

typedef enum {
  DFBR_CSS_LIST_NONE,
  DFBR_CSS_LIST_DISC,
  DFBR_CSS_LIST_CIRCLE,
  DFBR_CSS_LIST_SQUARE,
  DFBR_CSS_LIST_DECIMAL,
  DFBR_CSS_LIST_DECIMAL_LEADING_ZERO,
  DFBR_CSS_LIST_LOWER_ROMAN,
  DFBR_CSS_LIST_UPPER_ROMAN,
  DFBR_CSS_LIST_LOWER_ALPHA,
  DFBR_CSS_LIST_UPPER_ALPHA
} DfBrCssListStyleTypeK;

typedef enum {
  DFBR_CSS_LINEHEIGHT_NORMAL,
  DFBR_CSS_LINEHEIGHT_NUMBER,   /* unitless multiplier */
  DFBR_CSS_LINEHEIGHT_LENGTH    /* absolute or percent length */
} DfBrCssLineHeightKind;

typedef struct {
  DfBrCssLineHeightKind Kind;
  f32                   Number;   /* used when Kind == NUMBER */
  DfBrCssLength         Length;   /* used when Kind == LENGTH */
} DfBrCssLineHeight;

typedef struct {
  b32 Underline;
  b32 Overline;
  b32 LineThrough;
  b32 Blink;
} DfBrCssTextDecorationLines;

typedef enum {
  DFBR_CSS_FLEX_ROW,
  DFBR_CSS_FLEX_ROW_REVERSE,
  DFBR_CSS_FLEX_COLUMN,
  DFBR_CSS_FLEX_COLUMN_REVERSE
} DfBrCssFlexDirection;

typedef enum {
  DFBR_CSS_FLEX_NOWRAP,
  DFBR_CSS_FLEX_WRAP,
  DFBR_CSS_FLEX_WRAP_REVERSE
} DfBrCssFlexWrap;

typedef enum {
  DFBR_CSS_ALIGN_START,
  DFBR_CSS_ALIGN_END,
  DFBR_CSS_ALIGN_FLEX_START,
  DFBR_CSS_ALIGN_FLEX_END,
  DFBR_CSS_ALIGN_CENTER,
  DFBR_CSS_ALIGN_BASELINE,
  DFBR_CSS_ALIGN_STRETCH,
  DFBR_CSS_ALIGN_SPACE_BETWEEN,
  DFBR_CSS_ALIGN_SPACE_AROUND,
  DFBR_CSS_ALIGN_SPACE_EVENLY,
  DFBR_CSS_ALIGN_NORMAL,
  DFBR_CSS_ALIGN_AUTO
} DfBrCssAlign;

struct DfBrCssComputedStyle {
  /* --- display + flow ------------------------------------------- */
  DfBrCssDisplay       Display;
  DfBrCssPosition      Position;
  DfBrCssFloat         Float;
  DfBrCssClear         Clear;
  DfBrCssVisibility    Visibility;
  DfBrCssOverflowKind  OverflowX;
  DfBrCssOverflowKind  OverflowY;
  DfBrCssBoxSizing     BoxSizing;
  b32                  ZIndexAuto;
  i32                  ZIndex;
  f32                  Opacity;

  /* --- box ------------------------------------------------------- */
  DfBrCssLength        Width;
  DfBrCssLength        Height;
  DfBrCssLength        MinWidth;
  DfBrCssLength        MinHeight;
  DfBrCssLength        MaxWidth;
  DfBrCssLength        MaxHeight;
  DfBrCssLength        Top;
  DfBrCssLength        Right;
  DfBrCssLength        Bottom;
  DfBrCssLength        Left;
  DfBrCssLength        MarginTop;
  DfBrCssLength        MarginRight;
  DfBrCssLength        MarginBottom;
  DfBrCssLength        MarginLeft;
  DfBrCssLength        PaddingTop;
  DfBrCssLength        PaddingRight;
  DfBrCssLength        PaddingBottom;
  DfBrCssLength        PaddingLeft;
  DfBrCssBorder        BorderTop;
  DfBrCssBorder        BorderRight;
  DfBrCssBorder        BorderBottom;
  DfBrCssBorder        BorderLeft;
  DfBrCssLength        BorderTopLeftRadius;
  DfBrCssLength        BorderTopRightRadius;
  DfBrCssLength        BorderBottomRightRadius;
  DfBrCssLength        BorderBottomLeftRadius;

  /* --- background ----------------------------------------------- */
  DfBrCssColor         BackgroundColor;
  b32                  BackgroundColorIsCurrent;
  const char          *BackgroundImageUrl;       /* arena-owned; NULL = none */
  usize                BackgroundImageUrlLen;
  /* Parsed linear-gradient(...). Owned by the DfBrCssStyleContext's
   * arena; lifetime matches the computed style. NULL means "no
   * gradient" (either the author declared a solid/url only, or the
   * gradient failed to parse and we fell back). */
  const DfBrCssGradient *BackgroundGradient;
  DfBrCssBgClip        BackgroundClip;

  /* CSS UI §7.1 `box-shadow` - a comma-separated list of shadow
   * layers. First layer paints on top (authored first). Arena-owned;
   * NULL means "no shadow". */
  const DfBrCssShadow *BoxShadow;

  /* --- color + text --------------------------------------------- */
  DfBrCssColor         Color;
  DfBrCssLength        FontSize;
  i32                  FontWeight;               /* 100..900 */
  DfBrCssFontStyle     FontStyle;
  /* Font family list. Strings are arena-owned, lowercased for compare
   * but preserved in original case for debug dump. Order = author's. */
  const char         **FontFamilies;             /* NULL-terminated array of C strings */
  usize                FontFamilyCount;
  DfBrCssLineHeight    LineHeight;
  DfBrCssTextAlign     TextAlign;
  DfBrCssTextDecorationLines TextDecoration;
  DfBrCssColor         TextDecorationColor;
  DfBrCssTextTransform TextTransform;
  DfBrCssLength        TextIndent;
  DfBrCssLength        LetterSpacing;
  DfBrCssLength        WordSpacing;
  DfBrCssWhiteSpace    WhiteSpace;
  DfBrCssVerticalAlign VerticalAlign;
  DfBrCssLength        VerticalAlignLength;

  /* --- list ----------------------------------------------------- */
  DfBrCssListStyleTypeK ListStyleType;

  /* --- flex ----------------------------------------------------- */
  DfBrCssFlexDirection FlexDirection;
  DfBrCssFlexWrap      FlexWrap;
  f32                  FlexGrow;
  f32                  FlexShrink;
  DfBrCssLength        FlexBasis;
  DfBrCssAlign         JustifyContent;
  DfBrCssAlign         AlignItems;
  DfBrCssAlign         AlignSelf;
  DfBrCssAlign         AlignContent;
  DfBrCssLength        RowGap;
  DfBrCssLength        ColumnGap;
  i32                  Order;

  /* Raw "specified" opacity / other interpolated values stay out of
   * v1 - layout doesn't use them yet. */

  /* CSS custom properties (`--foo: value;`) declared on this element,
   * chained to the parent's list. Most recently-declared (highest
   * cascade order) are at the head. Value CV lists are arena-owned
   * by the style context.
   *
   * Inherited by design: children link their new entries to the parent
   * chain, so `var(--foo)` resolution walks up until it finds a match.
   * Declared in DfBrCssInternal.h; kept opaque here so layout / paint
   * modules don't need to include CSS-internal headers. */
  DfBrCssCustomProp   *CustomProps;
};

/** Accessor: read the computed style of an element or return NULL if
    DfBrCssApplyToDocument has not populated it. **/
const DfBrCssComputedStyle *
DfBrCssGetComputedStyle (const DfBrHtmlNode *Node);

/* ------------------------------------------------------------------
 *  Cascade / style context
 *
 *  A DfBrCssStyleContext owns the arena that holds every computed
 *  style pointed to by DfBrHtmlNode::StyleData. Destroying the
 *  context walks the document once to NULL out StyleData before
 *  freeing the arena, so the pointers never dangle.
 *
 *  Typical use:
 *
 *      DfBrHtmlDocument   *Doc   = DfBrHtmlParse(html, htmlLen, NULL);
 *      DfBrCssStylesheet  *A     = DfBrCssParse(cssA, lenA, NULL);
 *      DfBrCssStylesheet  *UA    = DfBrCssUserAgentStylesheet();
 *      const DfBrCssStylesheet *Sheets[] = { UA, A };
 *      DfBrCssStyleContext *Sc = DfBrCssApplyToDocument(Doc, Sheets, 2);
 *      ...
 *      DfBrCssStyleContextDestroy(Sc);
 *      DfBrHtmlDocumentDestroy(Doc);
 *      DfBrCssStylesheetDestroy(A);
 * -----------------------------------------------------------------*/

DfBrCssStyleContext *
DfBrCssApplyToDocument (
  DfBrHtmlDocument              *Doc,
  const DfBrCssStylesheet *const *Sheets,
  usize                           SheetCount
  );

/** Same as DfBrCssApplyToDocument, but also takes the viewport size
    (CSS pixels) used to evaluate @media (min-width: ...) and friends
    during the cascade. Pass 0 to either dimension for "unknown" - the
    cascade will then accept every viewport-based query (matching the
    behaviour of the plain DfBrCssApplyToDocument entry point). **/
DfBrCssStyleContext *
DfBrCssApplyToDocumentEx (
  DfBrHtmlDocument              *Doc,
  const DfBrCssStylesheet *const *Sheets,
  usize                           SheetCount,
  f32                             ViewportWidthPx,
  f32                             ViewportHeightPx
  );

/** Destroy a style context. Walks the document to clear StyleData
    pointers from every DfBrHtmlNode before freeing the arena. Safe
    to call with NULL. **/
void  DfBrCssStyleContextDestroy (DfBrCssStyleContext *Sc);

/** Access the document the context was built for (useful for
    downstream layout consumers that receive just the context). **/
DfBrHtmlDocument *DfBrCssStyleContextDocument (DfBrCssStyleContext *Sc);

/* ------------------------------------------------------------------
 *  Shared user-agent stylesheet
 *
 *  Single process-wide instance, parsed lazily on first call. Safe
 *  to use in DfBrCssApplyToDocument() alongside author sheets. Do
 *  not call DfBrCssStylesheetDestroy() on it.
 * -----------------------------------------------------------------*/

const DfBrCssStylesheet *DfBrCssUserAgentStylesheet (void);

/* Manual teardown of the singleton. Only useful for leak-checkers
 * at program exit; the process can happily leak it otherwise. */
void  DfBrCssUserAgentShutdown (void);

/* ------------------------------------------------------------------
 *  Media queries on <link>/<style> "media" attributes
 *
 *  Evaluates the content of an HTML `media` attribute (e.g.
 *  `media="print"`, `media="screen and (max-width: 600px)"`,
 *  `media="(min-width: 768px), (min-height: 500px)"`) against the
 *  current rendering environment. Returns DFBR_TRUE if the attached
 *  stylesheet must be considered part of the cascade, DFBR_FALSE if
 *  it must be skipped.
 *
 *  An empty / NULL / whitespace-only media string matches (the HTML
 *  default when the attribute is omitted is `media="all"`). Any parse
 *  failure or unknown feature is treated as matching as well, to
 *  preserve the spec-mandated forward compatibility behaviour.
 *
 *  Pass 0 to VpWidthPx/VpHeightPx when the viewport size is unknown;
 *  viewport-based features (min-width/max-width/...) then match
 *  unconditionally, which keeps historical behaviour.
 * -----------------------------------------------------------------*/

b32 DfBrCssMediaAttrMatches (
  const char *MediaAttr,
  usize       MediaAttrLen,
  f32         VpWidthPx,
  f32         VpHeightPx
  );

/* ------------------------------------------------------------------
 *  Inline styles
 *
 *  Extracts and applies declarations from a `style="..."` attribute.
 *  Has the highest specificity bucket in the cascade (per the spec);
 *  DfBrCssApplyToDocument() calls this for you when it encounters
 *  a `style` attribute on any element.
 * -----------------------------------------------------------------*/

/** Parse a style attribute's content into a Declaration list. The
    list is arena-allocated on `ArenaDoc`, a hidden helper used by
    the cascade; most callers should not use this directly. **/
const DfBrCssDeclaration *
DfBrCssParseStyleAttr (
  DfBrCssStyleContext  *Sc,
  const char           *Src,
  usize                 SrcLen
  );

/* ------------------------------------------------------------------
 *  Debug dumping
 * -----------------------------------------------------------------*/

char *DfBrCssStylesheetDump (const DfBrCssStylesheet *S, usize *OutLen);
char *DfBrCssComputedStyleDump (const DfBrCssComputedStyle *Cs, usize *OutLen);
/** Walk the DOM and emit per-element "path -> { key: value; ... }"
    lines. Handy for verifying the cascade end to end. **/
char *DfBrCssStyleContextDump (DfBrCssStyleContext *Sc, usize *OutLen);
void  DfBrCssFreeDump          (char *S);

/* ------------------------------------------------------------------
 *  Length helpers
 * -----------------------------------------------------------------*/

b32  DfBrCssLengthIsAuto (const DfBrCssLength *L);
b32  DfBrCssLengthIsZero (const DfBrCssLength *L);

/** Best-effort conversion to CSS pixels given a font-size context.
    Percent / em / rem / viewport units that have no available
    context here default to 0 and Out is set to that. Returns
    DFBR_TRUE iff the conversion was fully resolved. **/
b32  DfBrCssLengthToPx (const DfBrCssLength *L,
                        f32 FontSizePx, f32 RootFontSizePx,
                        f32 ViewportW,  f32 ViewportH,
                        f32 ParentPx,   f32 *Out);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_CSS_H_ */
