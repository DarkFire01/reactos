/** @file
  DfBrHtmlTags.c - element name <-> DfBrHtmlTag mapping plus the
  per-tag flags the tokenizer cares about (void / rawtext / rcdata).

  Implementation is a flat array with a linear probe. At ~100 tags and
  strict ASCII-lowercase comparison this is well below the noise floor
  of a parse; a perfect-hash upgrade is a drop-in if we ever care.
**/

#include "DfBrHtmlInternal.h"
#include <string.h>

typedef struct {
  DfBrHtmlTag  Tag;
  const char  *Name;
  u8           Len;
  u8           IsVoid    : 1;
  u8           IsRawText : 1;
  u8           IsRcData  : 1;
} TagEntry;

/* Flag groups derive from WHATWG HTML:
 *   void elements        - self-closing by definition, never have children.
 *   raw-text elements    - content parsed as a single text token, no
 *                          entity decoding, no nested tags until the
 *                          matching close tag. E.g. <script>, <style>.
 *   escapable raw text   - like raw text but entities *are* decoded.
 *                          E.g. <title>, <textarea>. We call these
 *                          rcdata to match spec vocabulary. */

#define E(Tag_, Name_)            { DFBR_HTML_TAG_##Tag_, Name_, (u8)(sizeof (Name_) - 1), 0, 0, 0 }
#define VOID_E(Tag_, Name_)       { DFBR_HTML_TAG_##Tag_, Name_, (u8)(sizeof (Name_) - 1), 1, 0, 0 }
#define RAW_E(Tag_, Name_)        { DFBR_HTML_TAG_##Tag_, Name_, (u8)(sizeof (Name_) - 1), 0, 1, 0 }
#define RCDATA_E(Tag_, Name_)     { DFBR_HTML_TAG_##Tag_, Name_, (u8)(sizeof (Name_) - 1), 0, 0, 1 }

static const TagEntry mTable[] = {
  /* document */
  E(HTML,       "html"),
  E(HEAD,       "head"),
  E(BODY,       "body"),
  RCDATA_E(TITLE, "title"),
  VOID_E(META,  "meta"),
  VOID_E(LINK,  "link"),
  VOID_E(BASE,  "base"),
  RAW_E(SCRIPT, "script"),
  RAW_E(STYLE,  "style"),
  E(NOSCRIPT,   "noscript"),
  E(TEMPLATE,   "template"),

  /* grouping */
  E(DIV,        "div"),
  E(P,          "p"),
  VOID_E(BR,    "br"),
  VOID_E(HR,    "hr"),
  E(PRE,        "pre"),
  E(BLOCKQUOTE, "blockquote"),
  E(ADDRESS,    "address"),

  /* semantic */
  E(HEADER,     "header"),
  E(FOOTER,     "footer"),
  E(MAIN,       "main"),
  E(NAV,        "nav"),
  E(SECTION,    "section"),
  E(ARTICLE,    "article"),
  E(ASIDE,      "aside"),
  E(FIGURE,     "figure"),
  E(FIGCAPTION, "figcaption"),
  E(HGROUP,     "hgroup"),

  /* headings */
  E(H1, "h1"), E(H2, "h2"), E(H3, "h3"),
  E(H4, "h4"), E(H5, "h5"), E(H6, "h6"),

  /* text-level */
  E(A,      "a"),
  E(SPAN,   "span"),
  E(B,      "b"),
  E(STRONG, "strong"),
  E(I,      "i"),
  E(EM,     "em"),
  E(U,      "u"),
  E(S,      "s"),
  E(SMALL,  "small"),
  E(MARK,   "mark"),
  E(CODE,   "code"),
  E(KBD,    "kbd"),
  E(SAMP,   "samp"),
  E(VAR,    "var"),
  E(Q,      "q"),
  E(CITE,   "cite"),
  E(ABBR,   "abbr"),
  E(DFN,    "dfn"),
  E(SUB,    "sub"),
  E(SUP,    "sup"),
  E(TIME,   "time"),
  E(DATA,   "data"),
  E(RUBY,   "ruby"),
  E(RT,     "rt"),
  E(RP,     "rp"),
  E(BDI,    "bdi"),
  E(BDO,    "bdo"),
  VOID_E(WBR, "wbr"),
  E(DEL,    "del"),
  E(INS,    "ins"),

  /* lists */
  E(UL, "ul"), E(OL, "ol"), E(LI, "li"),
  E(DL, "dl"), E(DT, "dt"), E(DD, "dd"),
  E(MENU, "menu"),

  /* media / embedded */
  VOID_E(IMG,   "img"),
  E(PICTURE,    "picture"),
  VOID_E(SOURCE,"source"),
  E(VIDEO,      "video"),
  E(AUDIO,      "audio"),
  VOID_E(TRACK, "track"),
  E(CANVAS,     "canvas"),
  E(SVG,        "svg"),   /* foreign content - v1 keeps it flat */
  E(MATH,       "math"),  /* foreign content - v1 keeps it flat */
  E(IFRAME,     "iframe"),
  VOID_E(EMBED, "embed"),
  E(OBJECT,     "object"),
  VOID_E(PARAM, "param"),
  E(MAP,        "map"),
  VOID_E(AREA,  "area"),

  /* tables */
  E(TABLE,    "table"),
  E(CAPTION,  "caption"),
  E(COLGROUP, "colgroup"),
  VOID_E(COL, "col"),
  E(THEAD,    "thead"),
  E(TBODY,    "tbody"),
  E(TFOOT,    "tfoot"),
  E(TR,       "tr"),
  E(TD,       "td"),
  E(TH,       "th"),

  /* forms */
  E(FORM,       "form"),
  VOID_E(INPUT, "input"),
  E(BUTTON,     "button"),
  E(SELECT,     "select"),
  E(OPTION,     "option"),
  E(OPTGROUP,   "optgroup"),
  RCDATA_E(TEXTAREA, "textarea"),
  E(LABEL,      "label"),
  E(FIELDSET,   "fieldset"),
  E(LEGEND,     "legend"),
  E(OUTPUT,     "output"),
  E(PROGRESS,   "progress"),
  E(METER,      "meter"),
  E(DATALIST,   "datalist"),

  /* interactive */
  E(DETAILS, "details"),
  E(SUMMARY, "summary"),
  E(DIALOG,  "dialog"),

  /* frames (obsolete) */
  E(FRAMESET,  "frameset"),
  E(FRAME,     "frame"),

  /* legacy / obscure */
  E(APPLET,    "applet"),
  E(BIG,       "big"),
  E(CENTER,    "center"),
  E(DIR,       "dir"),
  E(FONT,      "font"),
  E(IMAGE,     "image"),     /* parser rewrites to IMG on the fly */
  VOID_E(KEYGEN, "keygen"),
  E(LISTING,   "listing"),
  E(MARQUEE,   "marquee"),
  E(NOBR,      "nobr"),
  RAW_E(NOEMBED,  "noembed"),
  RAW_E(NOFRAMES, "noframes"),
  E(PLAINTEXT, "plaintext"),
  E(RB,        "rb"),
  E(RTC,       "rtc"),
  E(STRIKE,    "strike"),
  E(TT,        "tt"),
  RAW_E(XMP,   "xmp"),
};

#undef E
#undef VOID_E
#undef RAW_E
#undef RCDATA_E

static int
AsciiLowerEq (
  const char  *A,
  usize        ALen,
  const char  *B,
  u8           BLen
  )
{
  usize  i;
  char   C;

  if (ALen != (usize)BLen) {
    return 0;
  }
  for (i = 0; i < ALen; i++) {
    C = A[i];
    if ((C >= 'A') && (C <= 'Z')) {
      C = (char)(C - 'A' + 'a');
    }
    if (C != B[i]) {
      return 0;
    }
  }
  return 1;
}

static const TagEntry *
FindEntryByTag (
  DfBrHtmlTag  Tag
  )
{
  usize  i;
  for (i = 0; i < DFBR_ARRAY_LEN (mTable); i++) {
    if (mTable[i].Tag == Tag) {
      return &mTable[i];
    }
  }
  return DFBR_NULL;
}

DfBrHtmlTag
DfBrHtmlTagFromName (
  const char  *Name,
  usize        NameLen
  )
{
  usize  i;

  if ((Name == DFBR_NULL) || (NameLen == 0)) {
    return DFBR_HTML_TAG_UNKNOWN;
  }
  for (i = 0; i < DFBR_ARRAY_LEN (mTable); i++) {
    if (AsciiLowerEq (Name, NameLen, mTable[i].Name, mTable[i].Len)) {
      return mTable[i].Tag;
    }
  }
  return DFBR_HTML_TAG_UNKNOWN;
}

const char *
DfBrHtmlTagName (
  DfBrHtmlTag  Tag
  )
{
  const TagEntry  *Ent = FindEntryByTag (Tag);
  return (Ent != DFBR_NULL) ? Ent->Name : "";
}

b32
DfBrHtmlTagIsVoid (
  DfBrHtmlTag  Tag
  )
{
  const TagEntry  *Ent = FindEntryByTag (Tag);
  return ((Ent != DFBR_NULL) && Ent->IsVoid) ? DFBR_TRUE : DFBR_FALSE;
}

b32
DfBrHtmlTagIsRawText (
  DfBrHtmlTag  Tag
  )
{
  const TagEntry  *Ent = FindEntryByTag (Tag);
  return ((Ent != DFBR_NULL) && Ent->IsRawText) ? DFBR_TRUE : DFBR_FALSE;
}

b32
DfBrHtmlTagIsRcData (
  DfBrHtmlTag  Tag
  )
{
  const TagEntry  *Ent = FindEntryByTag (Tag);
  return ((Ent != DFBR_NULL) && Ent->IsRcData) ? DFBR_TRUE : DFBR_FALSE;
}
