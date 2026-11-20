/** @file
  DfBrHtml.h - WHATWG HTML parser + DOM for DefyBrowser.

  Design intent
  =============
  This is the foundation of the layout engine. Everything downstream
  (CSS cascade, layout boxes, JS bindings, paint) will hang off the
  node tree this library produces, so it is designed to grow into a
  full Chrome-replacement DOM without a ground-up rewrite:

    1. **Full node tree, not a string transform.** Elements, text,
       comments, and doctypes are distinct node kinds with parent,
       sibling, and first/last-child links. Attribute lists are
       ordered and preserved verbatim. There is room for mutation
       later without changing the public shape.

    2. **Reserved per-node slots** for subsystems that don't exist
       yet - `StyleData`, `LayoutData`, `ScriptData`. The parser
       never touches them. The eventual CSS cascade, layout tree, and
       DOM-to-JS bindings will each own one.

    3. **Spec-shaped tokenizer.** v1 implements the core subset of
       the WHATWG HTML tokenizer state machine (data, tag open,
       attribute name/value in all three quoting styles, comments,
       doctype, RCDATA for <title>/<textarea>, RAWTEXT for <script>/
       <style>). State names follow the spec so future contributors
       can grep the WHATWG text straight into the source. Adding a
       new state is an additive change to one switch; no cross-file
       surgery.

    4. **Arena allocation.** One pass to parse, one call to free. No
       reference counting, no orphan nodes. Cheap to throw documents
       away, cheap to allocate strings inside nodes.

    5. **Error-tolerant, never fatal.** Malformed input never makes
       DfBrHtmlParse return NULL. We collect diagnostics (line +
       column + message) and keep building whatever sensible tree we
       can - that is how every real browser behaves.

    6. **Engine-agnostic.** No QuickJS, no CSS, no platform types leak
       into this header. The binding layers that come later will own
       the specific types they stuff into the reserved slots.

  Deliberate v1 gaps (tracked in the roadmap):
    - Full 80-state tokenizer (we have the common ~25).
    - HTML5 insertion modes / active formatting elements / Adoption
      Agency. v1 uses a flat open-elements stack.
    - Foreign content (SVG/MathML) nesting rules. v1 treats them as
      ordinary elements.
    - Full named character reference table (2125 entries). v1 ships
      the ~50 most common plus numeric refs.
    - document.write / speculative parsing.
**/

#ifndef DFBR_HTML_H_
#define DFBR_HTML_H_

#include "../common/DfBrTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 *  Tag enum
 *
 *  Flat enumeration of every tag the engine knows by name. Anything
 *  else maps to DFBR_HTML_TAG_UNKNOWN and keeps its original-case
 *  name in DfBrHtmlNode::Name, so custom elements (`<my-widget>`)
 *  round-trip through the tree unharmed.
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_HTML_TAG_UNKNOWN = 0,

  /* document structure */
  DFBR_HTML_TAG_HTML,
  DFBR_HTML_TAG_HEAD,
  DFBR_HTML_TAG_BODY,
  DFBR_HTML_TAG_TITLE,
  DFBR_HTML_TAG_META,
  DFBR_HTML_TAG_LINK,
  DFBR_HTML_TAG_BASE,
  DFBR_HTML_TAG_SCRIPT,
  DFBR_HTML_TAG_STYLE,
  DFBR_HTML_TAG_NOSCRIPT,
  DFBR_HTML_TAG_TEMPLATE,

  /* grouping / block */
  DFBR_HTML_TAG_DIV,
  DFBR_HTML_TAG_P,
  DFBR_HTML_TAG_BR,
  DFBR_HTML_TAG_HR,
  DFBR_HTML_TAG_PRE,
  DFBR_HTML_TAG_BLOCKQUOTE,
  DFBR_HTML_TAG_ADDRESS,

  /* semantic sectioning */
  DFBR_HTML_TAG_HEADER,
  DFBR_HTML_TAG_FOOTER,
  DFBR_HTML_TAG_MAIN,
  DFBR_HTML_TAG_NAV,
  DFBR_HTML_TAG_SECTION,
  DFBR_HTML_TAG_ARTICLE,
  DFBR_HTML_TAG_ASIDE,
  DFBR_HTML_TAG_FIGURE,
  DFBR_HTML_TAG_FIGCAPTION,
  DFBR_HTML_TAG_HGROUP,

  /* headings */
  DFBR_HTML_TAG_H1,
  DFBR_HTML_TAG_H2,
  DFBR_HTML_TAG_H3,
  DFBR_HTML_TAG_H4,
  DFBR_HTML_TAG_H5,
  DFBR_HTML_TAG_H6,

  /* text-level */
  DFBR_HTML_TAG_A,
  DFBR_HTML_TAG_SPAN,
  DFBR_HTML_TAG_B,
  DFBR_HTML_TAG_STRONG,
  DFBR_HTML_TAG_I,
  DFBR_HTML_TAG_EM,
  DFBR_HTML_TAG_U,
  DFBR_HTML_TAG_S,
  DFBR_HTML_TAG_SMALL,
  DFBR_HTML_TAG_MARK,
  DFBR_HTML_TAG_CODE,
  DFBR_HTML_TAG_KBD,
  DFBR_HTML_TAG_SAMP,
  DFBR_HTML_TAG_VAR,
  DFBR_HTML_TAG_Q,
  DFBR_HTML_TAG_CITE,
  DFBR_HTML_TAG_ABBR,
  DFBR_HTML_TAG_DFN,
  DFBR_HTML_TAG_SUB,
  DFBR_HTML_TAG_SUP,
  DFBR_HTML_TAG_TIME,
  DFBR_HTML_TAG_DATA,
  DFBR_HTML_TAG_RUBY,
  DFBR_HTML_TAG_RT,
  DFBR_HTML_TAG_RP,
  DFBR_HTML_TAG_BDI,
  DFBR_HTML_TAG_BDO,
  DFBR_HTML_TAG_WBR,
  DFBR_HTML_TAG_DEL,
  DFBR_HTML_TAG_INS,

  /* lists */
  DFBR_HTML_TAG_UL,
  DFBR_HTML_TAG_OL,
  DFBR_HTML_TAG_LI,
  DFBR_HTML_TAG_DL,
  DFBR_HTML_TAG_DT,
  DFBR_HTML_TAG_DD,
  DFBR_HTML_TAG_MENU,

  /* media / embedded */
  DFBR_HTML_TAG_IMG,
  DFBR_HTML_TAG_PICTURE,
  DFBR_HTML_TAG_SOURCE,
  DFBR_HTML_TAG_VIDEO,
  DFBR_HTML_TAG_AUDIO,
  DFBR_HTML_TAG_TRACK,
  DFBR_HTML_TAG_CANVAS,
  DFBR_HTML_TAG_SVG,
  DFBR_HTML_TAG_MATH,
  DFBR_HTML_TAG_IFRAME,
  DFBR_HTML_TAG_EMBED,
  DFBR_HTML_TAG_OBJECT,
  DFBR_HTML_TAG_PARAM,
  DFBR_HTML_TAG_MAP,
  DFBR_HTML_TAG_AREA,

  /* tables */
  DFBR_HTML_TAG_TABLE,
  DFBR_HTML_TAG_CAPTION,
  DFBR_HTML_TAG_COLGROUP,
  DFBR_HTML_TAG_COL,
  DFBR_HTML_TAG_THEAD,
  DFBR_HTML_TAG_TBODY,
  DFBR_HTML_TAG_TFOOT,
  DFBR_HTML_TAG_TR,
  DFBR_HTML_TAG_TD,
  DFBR_HTML_TAG_TH,

  /* forms */
  DFBR_HTML_TAG_FORM,
  DFBR_HTML_TAG_INPUT,
  DFBR_HTML_TAG_BUTTON,
  DFBR_HTML_TAG_SELECT,
  DFBR_HTML_TAG_OPTION,
  DFBR_HTML_TAG_OPTGROUP,
  DFBR_HTML_TAG_TEXTAREA,
  DFBR_HTML_TAG_LABEL,
  DFBR_HTML_TAG_FIELDSET,
  DFBR_HTML_TAG_LEGEND,
  DFBR_HTML_TAG_OUTPUT,
  DFBR_HTML_TAG_PROGRESS,
  DFBR_HTML_TAG_METER,
  DFBR_HTML_TAG_DATALIST,

  /* interactive */
  DFBR_HTML_TAG_DETAILS,
  DFBR_HTML_TAG_SUMMARY,
  DFBR_HTML_TAG_DIALOG,

  /* Frames (obsolete but the HTML5 tree construction algorithm still
   * recognises them). */
  DFBR_HTML_TAG_FRAMESET,
  DFBR_HTML_TAG_FRAME,

  /* Obsolete / legacy formatting. Tree construction has special rules
   * (auto-close, adoption agency) for most of these, so they need
   * their own enum values even though CSS / layout won't render them
   * specially. */
  DFBR_HTML_TAG_APPLET,
  DFBR_HTML_TAG_BIG,
  DFBR_HTML_TAG_CENTER,
  DFBR_HTML_TAG_DIR,
  DFBR_HTML_TAG_FONT,
  DFBR_HTML_TAG_IMAGE,     /* alias for IMG per spec */
  DFBR_HTML_TAG_KEYGEN,    /* void */
  DFBR_HTML_TAG_LISTING,
  DFBR_HTML_TAG_MARQUEE,
  DFBR_HTML_TAG_NOBR,
  DFBR_HTML_TAG_NOEMBED,   /* rawtext */
  DFBR_HTML_TAG_NOFRAMES,  /* rawtext */
  DFBR_HTML_TAG_PLAINTEXT,
  DFBR_HTML_TAG_RB,
  DFBR_HTML_TAG_RTC,
  DFBR_HTML_TAG_STRIKE,
  DFBR_HTML_TAG_TT,
  DFBR_HTML_TAG_XMP,       /* rawtext */

  DFBR_HTML_TAG_COUNT
} DfBrHtmlTag;

/* ------------------------------------------------------------------
 *  Node kinds
 * -----------------------------------------------------------------*/

typedef enum {
  DFBR_HTML_NODE_DOCUMENT,
  DFBR_HTML_NODE_DOCTYPE,
  DFBR_HTML_NODE_ELEMENT,
  DFBR_HTML_NODE_TEXT,
  DFBR_HTML_NODE_COMMENT
} DfBrHtmlNodeKind;

/* ------------------------------------------------------------------
 *  Attribute + Node + Document
 * -----------------------------------------------------------------*/

typedef struct DfBrHtmlAttr {
  const char           *Name;      /* ASCII-lowercased, NUL-terminated */
  usize                 NameLen;
  const char           *Value;     /* decoded; NUL-terminated; "" if value-less */
  usize                 ValueLen;
  b32                   HasValue;  /* distinguishes `disabled` from `disabled=""` */
  struct DfBrHtmlAttr  *Next;
} DfBrHtmlAttr;

typedef struct DfBrHtmlNode {
  DfBrHtmlNodeKind       Kind;
  DfBrHtmlTag            Tag;          /* valid iff Kind == ELEMENT */
  const char            *Name;         /* original-case name (ELEMENT) or type label */
  usize                  NameLen;
  DfBrHtmlAttr          *FirstAttr;
  const char            *Text;         /* TEXT / COMMENT / DOCTYPE payload */
  usize                  TextLen;

  struct DfBrHtmlNode   *Parent;
  struct DfBrHtmlNode   *FirstChild;
  struct DfBrHtmlNode   *LastChild;
  struct DfBrHtmlNode   *PrevSibling;
  struct DfBrHtmlNode   *NextSibling;

  /* Source location where this node started. 1-based; zero means the
   * parser couldn't attribute it. Useful for diagnostics pointing back
   * into the source. */
  i32                    Line;
  i32                    Column;

  /* Reserved slots for subsystems that will land later. The parser
   * never reads or writes them; they exist so every node has a stable
   * anchor for computed style / layout box / JS wrapper state without
   * a hash map keyed on node pointers. */
  void                  *StyleData;    /* DfBrCssComputedStyle *  (future) */
  void                  *LayoutData;   /* DfBrLayoutBox *         (future) */
  void                  *ScriptData;   /* JS wrapper handle       (future) */
} DfBrHtmlNode;

typedef struct DfBrHtmlDiagEntry {
  i32          Line;       /* 1-based */
  i32          Column;     /* 1-based */
  const char  *Message;    /* arena-owned; valid until doc destroy */
} DfBrHtmlDiagEntry;

typedef struct DfBrHtmlDocument DfBrHtmlDocument;

/* ------------------------------------------------------------------
 *  Parse options
 * -----------------------------------------------------------------*/

typedef struct {
  b32  KeepComments;          /* default: true  */
  b32  KeepDoctype;           /* default: true  */
  b32  EmitSourceLocations;   /* default: true - fill Line/Column in each node */
} DfBrHtmlParseOptions;

/* ------------------------------------------------------------------
 *  Public API
 * -----------------------------------------------------------------*/

/** Parse an HTML document.

    @param[in]  Source     UTF-8 source. Need not be NUL terminated;
                           SourceLen is authoritative.
    @param[in]  SourceLen  Byte count, or 0 to strlen().
    @param[in]  Opts       May be NULL; then defaults are used.

    Never returns NULL unless allocation fails. A structurally empty
    input yields a DOCUMENT node with no children and an empty
    diagnostics list. **/
DfBrHtmlDocument *
DfBrHtmlParse (
  const char                  *Source,
  usize                        SourceLen,
  const DfBrHtmlParseOptions  *Opts
  );

void
DfBrHtmlDocumentDestroy (
  DfBrHtmlDocument  *Doc
  );

/* ------------------------------------------------------------------
 *  Convenience queries
 * -----------------------------------------------------------------*/

const DfBrHtmlNode *DfBrHtmlDocumentRoot (const DfBrHtmlDocument *Doc);
const DfBrHtmlNode *DfBrHtmlDocumentHtml (const DfBrHtmlDocument *Doc);
const DfBrHtmlNode *DfBrHtmlDocumentHead (const DfBrHtmlDocument *Doc);
const DfBrHtmlNode *DfBrHtmlDocumentBody (const DfBrHtmlDocument *Doc);

usize                      DfBrHtmlDocumentDiagCount (const DfBrHtmlDocument *Doc);
const DfBrHtmlDiagEntry   *DfBrHtmlDocumentDiag      (const DfBrHtmlDocument *Doc, usize Index);

const DfBrHtmlAttr *DfBrHtmlNodeGetAttr      (const DfBrHtmlNode *Node, const char *Name);
/** Returns "" (empty NUL-terminated) if attribute exists but has no
    value; returns NULL if attribute is absent. **/
const char         *DfBrHtmlNodeGetAttrValue (const DfBrHtmlNode *Node, const char *Name);

/** Pre-order walk of the subtree rooted at Root. Pass NULL as Cur to
    get Root itself first. Returns NULL after the last descendant. **/
const DfBrHtmlNode *DfBrHtmlNodeNextPreorder (const DfBrHtmlNode *Root, const DfBrHtmlNode *Cur);

const DfBrHtmlNode *DfBrHtmlNodeFindById        (const DfBrHtmlNode *Root, const char *Id);
const DfBrHtmlNode *DfBrHtmlNodeFindFirstByTag  (const DfBrHtmlNode *Root, DfBrHtmlTag Tag);

/* ------------------------------------------------------------------
 *  Mutation API
 *
 *  The parser builds the tree once; these helpers let JS bindings
 *  (and any future DOM tooling) mutate it afterwards. They all take
 *  `DfBrHtmlDocument *` so new strings can land on the doc arena -
 *  nodes and attributes never own their own memory.
 *
 *  A doc-wide "generation" counter bumps on every structural or
 *  attribute change so hosts can cheaply detect that a relayout is
 *  due, without having to diff the tree.
 * -----------------------------------------------------------------*/
 
 /** Create a new detached element node. Free-floating until it gets
     inserted somewhere. Tag is resolved from Name internally. */
 DfBrHtmlNode *
 DfBrHtmlDocumentCreateElement (
   DfBrHtmlDocument  *Doc,
   const char        *Name,
   usize              NameLen    /* 0 = strlen */
   );
 
 /** Create a new detached text node. */
 DfBrHtmlNode *
 DfBrHtmlDocumentCreateTextNode (
   DfBrHtmlDocument  *Doc,
   const char        *Text,
   usize              TextLen    /* 0 = strlen */
   );
 
 /** Append `Child` as the last child of `Parent`. If `Child` was
     already parented elsewhere it is detached first. */
 void
 DfBrHtmlDocumentAppendChild (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Parent,
   DfBrHtmlNode      *Child
   );
 
 /** Insert `NewChild` before `Ref` under `Parent`. `Ref` may be NULL,
     in which case this behaves like AppendChild. */
 void
 DfBrHtmlDocumentInsertBefore (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Parent,
   DfBrHtmlNode      *NewChild,
   DfBrHtmlNode      *Ref
   );
 
 /** Unlink `Child` from its parent. Safe to call on an already-
     detached node. */
 void
 DfBrHtmlDocumentRemoveChild (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Child
   );
 
 /** Set/replace the value of `Name` on `Elem`. If the attribute is
     missing it is appended; otherwise the value is rewritten (both
     source string and length are replaced). */
 void
 DfBrHtmlDocumentSetAttribute (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Elem,
   const char        *Name,
   usize              NameLen,    /* 0 = strlen */
   const char        *Value,
   usize              ValueLen    /* 0 = strlen */
   );
 
 /** Remove attribute `Name` from `Elem`. No-op if absent. */
 void
 DfBrHtmlDocumentRemoveAttribute (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Elem,
   const char        *Name,
   usize              NameLen    /* 0 = strlen */
   );
 
 /** Replace all children of `Elem` with a single text node whose
     content is `Text`. For elements like <script> and <style> this
     is a common scripting pattern and simpler to expose than the
     full Node cluster. */
 void
 DfBrHtmlDocumentSetTextContent (
   DfBrHtmlDocument  *Doc,
   DfBrHtmlNode      *Elem,
   const char        *Text,
   usize              TextLen    /* 0 = strlen */
   );
 
 /** A monotonically-increasing counter that advances on any of the
     mutation calls above. Hosts compare two samples to know whether
     layout needs rerunning. */
 u64
 DfBrHtmlDocumentGeneration (const DfBrHtmlDocument *Doc);
 
 /* ------------------------------------------------------------------
 *  Tag metadata
 * -----------------------------------------------------------------*/

DfBrHtmlTag  DfBrHtmlTagFromName  (const char *Name, usize NameLen);
const char  *DfBrHtmlTagName      (DfBrHtmlTag Tag);
b32          DfBrHtmlTagIsVoid    (DfBrHtmlTag Tag);
b32          DfBrHtmlTagIsRawText (DfBrHtmlTag Tag);
b32          DfBrHtmlTagIsRcData  (DfBrHtmlTag Tag);

/* ------------------------------------------------------------------
 *  Serialization
 * -----------------------------------------------------------------*/

/** Serialize the subtree rooted at Node back to HTML. Returns a
    malloc()'d NUL-terminated UTF-8 buffer; free with
    DfBrHtmlFreeSerialized. OutLen, if non-NULL, receives byte length
    excluding the trailing NUL. Returns NULL on OOM. **/
char *DfBrHtmlSerialize (const DfBrHtmlNode *Node, usize *OutLen);

/** Indented debug dump of a node subtree. Same allocation rules. **/
char *DfBrHtmlDumpTree  (const DfBrHtmlNode *Node, usize *OutLen);

void  DfBrHtmlFreeSerialized (char *S);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_HTML_H_ */
