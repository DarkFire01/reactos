/** @file
  DfBrCssProps.c - property + keyword name <-> enum tables, plus the
                   inheritance / shorthand metadata bit flags that
                   the cascade reads per property.

  Adding a new property is strictly additive: extend the enum in
  DfBrCss.h, add an entry to mPropTable here, and handle it in
  DfBrCssComputed.c where the value actually feeds a computed-style
  slot. The rest of the engine doesn't need to change.
**/

#include "DfBrCssInternal.h"

#include <string.h>

/* ------------------------------------------------------------------
 *  Property metadata table
 * -----------------------------------------------------------------*/

#define INHERIT    1
#define NO_INHERIT 0
#define SHORT      1
#define LONG       0

typedef struct {
  DfBrCssProp  Prop;
  const char  *Name;
  unsigned     Inherits  : 1;
  unsigned     Shorthand : 1;
} PropEntry;

static const PropEntry mPropTable[] = {
  /* display + flow */
  { DFBR_CSS_PROP_DISPLAY,              "display",              NO_INHERIT, LONG },
  { DFBR_CSS_PROP_POSITION,             "position",             NO_INHERIT, LONG },
  { DFBR_CSS_PROP_FLOAT,                "float",                NO_INHERIT, LONG },
  { DFBR_CSS_PROP_CLEAR,                "clear",                NO_INHERIT, LONG },
  { DFBR_CSS_PROP_VISIBILITY,           "visibility",           INHERIT,    LONG },
  { DFBR_CSS_PROP_OVERFLOW,             "overflow",             NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_OVERFLOW_X,           "overflow-x",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_OVERFLOW_Y,           "overflow-y",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BOX_SIZING,           "box-sizing",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_Z_INDEX,              "z-index",              NO_INHERIT, LONG },
  { DFBR_CSS_PROP_OPACITY,              "opacity",              NO_INHERIT, LONG },

  /* box */
  { DFBR_CSS_PROP_WIDTH,                "width",                NO_INHERIT, LONG },
  { DFBR_CSS_PROP_HEIGHT,               "height",               NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MIN_WIDTH,            "min-width",            NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MIN_HEIGHT,           "min-height",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MAX_WIDTH,            "max-width",            NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MAX_HEIGHT,           "max-height",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_TOP,                  "top",                  NO_INHERIT, LONG },
  { DFBR_CSS_PROP_RIGHT,                "right",                NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BOTTOM,               "bottom",               NO_INHERIT, LONG },
  { DFBR_CSS_PROP_LEFT,                 "left",                 NO_INHERIT, LONG },

  { DFBR_CSS_PROP_MARGIN,               "margin",               NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_MARGIN_TOP,           "margin-top",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MARGIN_RIGHT,         "margin-right",         NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MARGIN_BOTTOM,        "margin-bottom",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_MARGIN_LEFT,          "margin-left",          NO_INHERIT, LONG },

  { DFBR_CSS_PROP_PADDING,              "padding",              NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_PADDING_TOP,          "padding-top",          NO_INHERIT, LONG },
  { DFBR_CSS_PROP_PADDING_RIGHT,        "padding-right",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_PADDING_BOTTOM,       "padding-bottom",       NO_INHERIT, LONG },
  { DFBR_CSS_PROP_PADDING_LEFT,         "padding-left",         NO_INHERIT, LONG },

  { DFBR_CSS_PROP_BORDER,               "border",               NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_WIDTH,         "border-width",         NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_STYLE,         "border-style",         NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_COLOR,         "border-color",         NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_TOP,           "border-top",           NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_RIGHT,         "border-right",         NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_BOTTOM,        "border-bottom",        NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_LEFT,          "border-left",          NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BORDER_TOP_WIDTH,     "border-top-width",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_RIGHT_WIDTH,   "border-right-width",   NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_BOTTOM_WIDTH,  "border-bottom-width",  NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_LEFT_WIDTH,    "border-left-width",    NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_TOP_STYLE,     "border-top-style",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_RIGHT_STYLE,   "border-right-style",   NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_BOTTOM_STYLE,  "border-bottom-style",  NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_LEFT_STYLE,    "border-left-style",    NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_TOP_COLOR,     "border-top-color",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_RIGHT_COLOR,   "border-right-color",   NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_BOTTOM_COLOR,  "border-bottom-color",  NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_LEFT_COLOR,    "border-left-color",    NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BORDER_RADIUS,        "border-radius",        NO_INHERIT, SHORT },

  /* background */
  { DFBR_CSS_PROP_BACKGROUND,           "background",           NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_BACKGROUND_COLOR,     "background-color",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_IMAGE,     "background-image",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_REPEAT,    "background-repeat",    NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_POSITION,  "background-position",  NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_SIZE,      "background-size",      NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_ATTACHMENT,"background-attachment",NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_CLIP,      "background-clip",      NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BACKGROUND_ORIGIN,    "background-origin",    NO_INHERIT, LONG },

  /* color + text (most of text is inherited) */
  { DFBR_CSS_PROP_COLOR,                "color",                INHERIT,    LONG },
  { DFBR_CSS_PROP_FONT,                 "font",                 INHERIT,    SHORT },
  { DFBR_CSS_PROP_FONT_FAMILY,          "font-family",          INHERIT,    LONG },
  { DFBR_CSS_PROP_FONT_SIZE,            "font-size",            INHERIT,    LONG },
  { DFBR_CSS_PROP_FONT_WEIGHT,          "font-weight",          INHERIT,    LONG },
  { DFBR_CSS_PROP_FONT_STYLE,           "font-style",           INHERIT,    LONG },
  { DFBR_CSS_PROP_FONT_VARIANT,         "font-variant",         INHERIT,    LONG },
  { DFBR_CSS_PROP_LINE_HEIGHT,          "line-height",          INHERIT,    LONG },
  { DFBR_CSS_PROP_TEXT_ALIGN,           "text-align",           INHERIT,    LONG },
  { DFBR_CSS_PROP_TEXT_DECORATION,      "text-decoration",      NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_TEXT_TRANSFORM,       "text-transform",       INHERIT,    LONG },
  { DFBR_CSS_PROP_TEXT_INDENT,          "text-indent",          INHERIT,    LONG },
  { DFBR_CSS_PROP_LETTER_SPACING,       "letter-spacing",       INHERIT,    LONG },
  { DFBR_CSS_PROP_WORD_SPACING,         "word-spacing",         INHERIT,    LONG },
  { DFBR_CSS_PROP_WHITE_SPACE,          "white-space",          INHERIT,    LONG },
  { DFBR_CSS_PROP_WORD_BREAK,           "word-break",           INHERIT,    LONG },
  { DFBR_CSS_PROP_OVERFLOW_WRAP,        "overflow-wrap",        INHERIT,    LONG },
  { DFBR_CSS_PROP_VERTICAL_ALIGN,       "vertical-align",       NO_INHERIT, LONG },

  /* list */
  { DFBR_CSS_PROP_LIST_STYLE,           "list-style",           INHERIT,    SHORT },
  { DFBR_CSS_PROP_LIST_STYLE_TYPE,      "list-style-type",      INHERIT,    LONG },
  { DFBR_CSS_PROP_LIST_STYLE_POSITION,  "list-style-position",  INHERIT,    LONG },
  { DFBR_CSS_PROP_LIST_STYLE_IMAGE,     "list-style-image",     INHERIT,    LONG },

  /* table */
  { DFBR_CSS_PROP_BORDER_COLLAPSE,      "border-collapse",      INHERIT,    LONG },
  { DFBR_CSS_PROP_BORDER_SPACING,       "border-spacing",       INHERIT,    LONG },
  { DFBR_CSS_PROP_CAPTION_SIDE,         "caption-side",         INHERIT,    LONG },
  { DFBR_CSS_PROP_EMPTY_CELLS,          "empty-cells",          INHERIT,    LONG },
  { DFBR_CSS_PROP_TABLE_LAYOUT,         "table-layout",         NO_INHERIT, LONG },

  /* flex */
  { DFBR_CSS_PROP_FLEX,                 "flex",                 NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_FLEX_DIRECTION,       "flex-direction",       NO_INHERIT, LONG },
  { DFBR_CSS_PROP_FLEX_WRAP,            "flex-wrap",            NO_INHERIT, LONG },
  { DFBR_CSS_PROP_FLEX_FLOW,            "flex-flow",            NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_FLEX_GROW,            "flex-grow",            NO_INHERIT, LONG },
  { DFBR_CSS_PROP_FLEX_SHRINK,          "flex-shrink",          NO_INHERIT, LONG },
  { DFBR_CSS_PROP_FLEX_BASIS,           "flex-basis",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_JUSTIFY_CONTENT,      "justify-content",      NO_INHERIT, LONG },
  { DFBR_CSS_PROP_ALIGN_ITEMS,          "align-items",          NO_INHERIT, LONG },
  { DFBR_CSS_PROP_ALIGN_SELF,           "align-self",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_ALIGN_CONTENT,        "align-content",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_GAP,                  "gap",                  NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_ROW_GAP,              "row-gap",              NO_INHERIT, LONG },
  { DFBR_CSS_PROP_COLUMN_GAP,           "column-gap",           NO_INHERIT, LONG },
  { DFBR_CSS_PROP_ORDER,                "order",                NO_INHERIT, LONG },

  /* misc */
  { DFBR_CSS_PROP_CURSOR,               "cursor",               INHERIT,    LONG },
  { DFBR_CSS_PROP_TRANSFORM,            "transform",            NO_INHERIT, LONG },
  { DFBR_CSS_PROP_TRANSFORM_ORIGIN,     "transform-origin",     NO_INHERIT, LONG },
  { DFBR_CSS_PROP_TRANSITION,           "transition",           NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_ANIMATION,            "animation",            NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_CONTENT,              "content",              NO_INHERIT, LONG },
  { DFBR_CSS_PROP_QUOTES,               "quotes",               INHERIT,    LONG },
  { DFBR_CSS_PROP_OUTLINE,              "outline",              NO_INHERIT, SHORT },
  { DFBR_CSS_PROP_OUTLINE_COLOR,        "outline-color",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_OUTLINE_STYLE,        "outline-style",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_OUTLINE_WIDTH,        "outline-width",        NO_INHERIT, LONG },
  { DFBR_CSS_PROP_BOX_SHADOW,           "box-shadow",           NO_INHERIT, LONG }
};

DfBrCssProp
DfBrCssPropFromName (
  const char  *Name,
  usize        Len
  )
{
  usize  i;
  if ((Name == DFBR_NULL) || (Len == 0)) return DFBR_CSS_PROP_UNKNOWN;
  for (i = 0; i < DFBR_ARRAY_LEN (mPropTable); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mPropTable[i].Name)) {
      return mPropTable[i].Prop;
    }
  }
  return DFBR_CSS_PROP_UNKNOWN;
}

const char *
DfBrCssPropName (
  DfBrCssProp  P
  )
{
  usize  i;
  for (i = 0; i < DFBR_ARRAY_LEN (mPropTable); i++) {
    if (mPropTable[i].Prop == P) return mPropTable[i].Name;
  }
  return "(unknown)";
}

b32
DfBrCssPropInherits (
  DfBrCssProp  P
  )
{
  usize  i;
  for (i = 0; i < DFBR_ARRAY_LEN (mPropTable); i++) {
    if (mPropTable[i].Prop == P) {
      return mPropTable[i].Inherits ? DFBR_TRUE : DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

b32
DfBrCssPropInheritsByDefault (
  DfBrCssProp  P
  )
{
  return DfBrCssPropInherits (P);
}

b32
DfBrCssPropIsShorthand (
  DfBrCssProp  P
  )
{
  usize  i;
  for (i = 0; i < DFBR_ARRAY_LEN (mPropTable); i++) {
    if (mPropTable[i].Prop == P) {
      return mPropTable[i].Shorthand ? DFBR_TRUE : DFBR_FALSE;
    }
  }
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Keyword table
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssKeyword  K;
  const char     *Name;
} KwEntry;

static const KwEntry mKwTable[] = {
  { DFBR_CSS_KW_INHERIT,          "inherit" },
  { DFBR_CSS_KW_INITIAL,          "initial" },
  { DFBR_CSS_KW_UNSET,            "unset" },
  { DFBR_CSS_KW_REVERT,           "revert" },

  { DFBR_CSS_KW_BLOCK,            "block" },
  { DFBR_CSS_KW_INLINE,           "inline" },
  { DFBR_CSS_KW_INLINE_BLOCK,     "inline-block" },
  { DFBR_CSS_KW_INLINE_FLEX,      "inline-flex" },
  { DFBR_CSS_KW_INLINE_TABLE,     "inline-table" },
  { DFBR_CSS_KW_NONE,             "none" },
  { DFBR_CSS_KW_FLEX,             "flex" },
  { DFBR_CSS_KW_GRID,             "grid" },
  { DFBR_CSS_KW_LIST_ITEM,        "list-item" },
  { DFBR_CSS_KW_TABLE,            "table" },
  { DFBR_CSS_KW_TABLE_ROW,        "table-row" },
  { DFBR_CSS_KW_TABLE_CELL,       "table-cell" },
  { DFBR_CSS_KW_TABLE_ROW_GROUP,  "table-row-group" },
  { DFBR_CSS_KW_TABLE_HEADER_GROUP,"table-header-group" },
  { DFBR_CSS_KW_TABLE_FOOTER_GROUP,"table-footer-group" },
  { DFBR_CSS_KW_TABLE_COLUMN,     "table-column" },
  { DFBR_CSS_KW_TABLE_COLUMN_GROUP,"table-column-group" },
  { DFBR_CSS_KW_TABLE_CAPTION,    "table-caption" },
  { DFBR_CSS_KW_RUN_IN,           "run-in" },
  { DFBR_CSS_KW_CONTENTS,         "contents" },

  { DFBR_CSS_KW_STATIC,           "static" },
  { DFBR_CSS_KW_RELATIVE,         "relative" },
  { DFBR_CSS_KW_ABSOLUTE,         "absolute" },
  { DFBR_CSS_KW_FIXED,            "fixed" },
  { DFBR_CSS_KW_STICKY,           "sticky" },

  { DFBR_CSS_KW_LEFT,             "left" },
  { DFBR_CSS_KW_RIGHT,            "right" },
  { DFBR_CSS_KW_BOTH,             "both" },
  { DFBR_CSS_KW_INLINE_START,     "inline-start" },
  { DFBR_CSS_KW_INLINE_END,       "inline-end" },

  { DFBR_CSS_KW_VISIBLE,          "visible" },
  { DFBR_CSS_KW_HIDDEN,           "hidden" },
  { DFBR_CSS_KW_COLLAPSE,         "collapse" },

  { DFBR_CSS_KW_SCROLL,           "scroll" },
  { DFBR_CSS_KW_AUTO,             "auto" },
  { DFBR_CSS_KW_CLIP,             "clip" },

  { DFBR_CSS_KW_CONTENT_BOX,      "content-box" },
  { DFBR_CSS_KW_BORDER_BOX,       "border-box" },
  { DFBR_CSS_KW_PADDING_BOX,      "padding-box" },
  { DFBR_CSS_KW_TEXT,             "text" },
  { DFBR_CSS_KW_TO,               "to" },

  { DFBR_CSS_KW_CENTER,           "center" },
  { DFBR_CSS_KW_JUSTIFY,          "justify" },
  { DFBR_CSS_KW_START,            "start" },
  { DFBR_CSS_KW_END,              "end" },

  { DFBR_CSS_KW_UNDERLINE,        "underline" },
  { DFBR_CSS_KW_OVERLINE,         "overline" },
  { DFBR_CSS_KW_LINE_THROUGH,     "line-through" },
  { DFBR_CSS_KW_BLINK,            "blink" },

  { DFBR_CSS_KW_NORMAL,           "normal" },
  { DFBR_CSS_KW_ITALIC,           "italic" },
  { DFBR_CSS_KW_OBLIQUE,          "oblique" },

  { DFBR_CSS_KW_BOLD,             "bold" },
  { DFBR_CSS_KW_BOLDER,           "bolder" },
  { DFBR_CSS_KW_LIGHTER,          "lighter" },

  { DFBR_CSS_KW_NOWRAP,           "nowrap" },
  { DFBR_CSS_KW_PRE,              "pre" },
  { DFBR_CSS_KW_PRE_WRAP,         "pre-wrap" },
  { DFBR_CSS_KW_PRE_LINE,         "pre-line" },
  { DFBR_CSS_KW_BREAK_SPACES,     "break-spaces" },

  { DFBR_CSS_KW_SOLID,            "solid" },
  { DFBR_CSS_KW_DOTTED,           "dotted" },
  { DFBR_CSS_KW_DASHED,           "dashed" },
  { DFBR_CSS_KW_DOUBLE,           "double" },
  { DFBR_CSS_KW_GROOVE,           "groove" },
  { DFBR_CSS_KW_RIDGE,            "ridge" },
  { DFBR_CSS_KW_INSET,            "inset" },
  { DFBR_CSS_KW_OUTSET,           "outset" },

  { DFBR_CSS_KW_THIN,             "thin" },
  { DFBR_CSS_KW_MEDIUM,           "medium" },
  { DFBR_CSS_KW_THICK,            "thick" },
  { DFBR_CSS_KW_SMALLER,          "smaller" },
  { DFBR_CSS_KW_LARGER,           "larger" },
  { DFBR_CSS_KW_XX_SMALL,         "xx-small" },
  { DFBR_CSS_KW_X_SMALL,          "x-small" },
  { DFBR_CSS_KW_SMALL,            "small" },
  { DFBR_CSS_KW_LARGE,            "large" },
  { DFBR_CSS_KW_X_LARGE,          "x-large" },
  { DFBR_CSS_KW_XX_LARGE,         "xx-large" },

  { DFBR_CSS_KW_ROW,              "row" },
  { DFBR_CSS_KW_ROW_REVERSE,      "row-reverse" },
  { DFBR_CSS_KW_COLUMN,           "column" },
  { DFBR_CSS_KW_COLUMN_REVERSE,   "column-reverse" },
  { DFBR_CSS_KW_WRAP,             "wrap" },
  { DFBR_CSS_KW_WRAP_REVERSE,     "wrap-reverse" },
  { DFBR_CSS_KW_FLEX_START,       "flex-start" },
  { DFBR_CSS_KW_FLEX_END,         "flex-end" },
  { DFBR_CSS_KW_SPACE_BETWEEN,    "space-between" },
  { DFBR_CSS_KW_SPACE_AROUND,     "space-around" },
  { DFBR_CSS_KW_SPACE_EVENLY,     "space-evenly" },
  { DFBR_CSS_KW_STRETCH,          "stretch" },
  { DFBR_CSS_KW_BASELINE,         "baseline" },

  { DFBR_CSS_KW_TRANSPARENT,      "transparent" },
  { DFBR_CSS_KW_CURRENTCOLOR,     "currentcolor" },

  { DFBR_CSS_KW_DISC,             "disc" },
  { DFBR_CSS_KW_CIRCLE,           "circle" },
  { DFBR_CSS_KW_SQUARE,           "square" },
  { DFBR_CSS_KW_DECIMAL,          "decimal" },
  { DFBR_CSS_KW_DECIMAL_LEADING_ZERO, "decimal-leading-zero" },
  { DFBR_CSS_KW_LOWER_ROMAN,      "lower-roman" },
  { DFBR_CSS_KW_UPPER_ROMAN,      "upper-roman" },
  { DFBR_CSS_KW_LOWER_ALPHA,      "lower-alpha" },
  { DFBR_CSS_KW_UPPER_ALPHA,      "upper-alpha" },
  { DFBR_CSS_KW_INSIDE,           "inside" },
  { DFBR_CSS_KW_OUTSIDE,          "outside" },

  { DFBR_CSS_KW_REPEAT,           "repeat" },
  { DFBR_CSS_KW_REPEAT_X,         "repeat-x" },
  { DFBR_CSS_KW_REPEAT_Y,         "repeat-y" },
  { DFBR_CSS_KW_NO_REPEAT,        "no-repeat" },
  { DFBR_CSS_KW_SPACE,            "space" },
  { DFBR_CSS_KW_ROUND,            "round" },
  { DFBR_CSS_KW_COVER,            "cover" },
  { DFBR_CSS_KW_CONTAIN,          "contain" },

  { DFBR_CSS_KW_POINTER,          "pointer" },
  { DFBR_CSS_KW_DEFAULT_KW,       "default" },
  { DFBR_CSS_KW_TEXT_CURSOR,      "text" },

  { DFBR_CSS_KW_UPPERCASE,        "uppercase" },
  { DFBR_CSS_KW_LOWERCASE,        "lowercase" },
  { DFBR_CSS_KW_CAPITALIZE,       "capitalize" },
  { DFBR_CSS_KW_FULL_WIDTH,       "full-width" },

  { DFBR_CSS_KW_TOP,              "top" },
  { DFBR_CSS_KW_MIDDLE,           "middle" },
  { DFBR_CSS_KW_BOTTOM,           "bottom" },
  { DFBR_CSS_KW_SUB,              "sub" },
  { DFBR_CSS_KW_SUPER,            "super" },
  { DFBR_CSS_KW_TEXT_TOP,         "text-top" },
  { DFBR_CSS_KW_TEXT_BOTTOM,      "text-bottom" },

  { DFBR_CSS_KW_SEPARATE,         "separate" }
};

DfBrCssKeyword
DfBrCssKeywordFromName (
  const char  *Name,
  usize        Len
  )
{
  usize  i;
  if ((Name == DFBR_NULL) || (Len == 0)) return DFBR_CSS_KW_UNKNOWN;
  for (i = 0; i < DFBR_ARRAY_LEN (mKwTable); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mKwTable[i].Name)) {
      return mKwTable[i].K;
    }
  }
  return DFBR_CSS_KW_UNKNOWN;
}

const char *
DfBrCssKeywordName (
  DfBrCssKeyword  K
  )
{
  usize  i;
  for (i = 0; i < DFBR_ARRAY_LEN (mKwTable); i++) {
    if (mKwTable[i].K == K) return mKwTable[i].Name;
  }
  return "";
}
