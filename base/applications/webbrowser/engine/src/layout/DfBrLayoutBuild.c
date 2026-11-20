/** @file
  DfBrLayoutBuild.c - walk the styled DOM and produce a DfBrLayoutBox
                      tree.

  The DOM + computed styles are the input; the output is a fresh tree
  whose structure mirrors CSS formatting contexts rather than the
  HTML tree itself:

    * Elements with `display: none` and their subtree are dropped.
    * Elements with `display: block`, `list-item`, `flex`, `grid`,
      or any `table-*` become BLOCK boxes. (v1 folds flex/grid/table
      onto block until their formatting contexts land.)
    * Elements with `display: inline` / `inline-block` become INLINE
      boxes. `inline-block` is a box that participates in the parent
      inline formatting context but establishes a block formatting
      context internally - v1 treats it as INLINE for flow.
    * Text nodes become TEXT boxes (a leaf inline).
    * Replaced elements (<img>) become REPLACED inline boxes.
    * If a block container would end up with a mix of block and
      inline direct children, runs of inline siblings are wrapped in
      an anonymous BLOCK box. This is the "anonymous block" rule
      from the CSS box model.
    * Comments and doctypes are skipped.
**/

#include "DfBrLayoutInternal.h"
#include "../html/DfBrCanvas.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Box creation + tree manipulation
 * -----------------------------------------------------------------*/

DfBrLayoutBox *
DfBrLayoutBoxNew (
  DfBrLayoutEngine            *E,
  DfBrLayoutBoxKind            Kind,
  const DfBrHtmlNode          *Dom,
  const DfBrCssComputedStyle  *Style
  )
{
  DfBrLayoutBox *B = (DfBrLayoutBox *)DfBrLayoutArenaAlloc (&E->Arena, sizeof (*B));
  if (B == DFBR_NULL) return DFBR_NULL;
  B->Kind    = Kind;
  B->DomNode = Dom;
  B->Style   = Style;
  B->FontSizePx = (Style != DFBR_NULL) ? Style->FontSize.Value : 16.0f;
  return B;
}

void
DfBrLayoutBoxAppend (DfBrLayoutBox *Parent, DfBrLayoutBox *Child) {
  if (Parent == DFBR_NULL || Child == DFBR_NULL) return;
  Child->Parent = Parent;
  Child->PrevSibling = Parent->LastChild;
  Child->NextSibling = DFBR_NULL;
  if (Parent->LastChild != DFBR_NULL) Parent->LastChild->NextSibling = Child;
  else                                 Parent->FirstChild              = Child;
  Parent->LastChild = Child;
}

/* ------------------------------------------------------------------
 *  Display -> box-kind mapping
 * -----------------------------------------------------------------*/

static b32
IsBlockDisplay (DfBrCssDisplay D) {
  switch (D) {
    case DFBR_CSS_DISPLAY_BLOCK:
    case DFBR_CSS_DISPLAY_LIST_ITEM:
    case DFBR_CSS_DISPLAY_FLEX:
    case DFBR_CSS_DISPLAY_GRID:
    case DFBR_CSS_DISPLAY_TABLE:
    case DFBR_CSS_DISPLAY_TABLE_ROW:
    case DFBR_CSS_DISPLAY_TABLE_CELL:
    case DFBR_CSS_DISPLAY_TABLE_ROW_GROUP:
    case DFBR_CSS_DISPLAY_TABLE_HEADER_GROUP:
    case DFBR_CSS_DISPLAY_TABLE_FOOTER_GROUP:
    case DFBR_CSS_DISPLAY_TABLE_COLUMN:
    case DFBR_CSS_DISPLAY_TABLE_COLUMN_GROUP:
    case DFBR_CSS_DISPLAY_TABLE_CAPTION:
      return DFBR_TRUE;
    default:
      return DFBR_FALSE;
  }
}

static b32
IsReplacedTag (DfBrHtmlTag T) {
  switch (T) {
    case DFBR_HTML_TAG_IMG:
    case DFBR_HTML_TAG_IFRAME:
    case DFBR_HTML_TAG_VIDEO:
    case DFBR_HTML_TAG_AUDIO:
    case DFBR_HTML_TAG_CANVAS:
    case DFBR_HTML_TAG_EMBED:
    case DFBR_HTML_TAG_OBJECT:
    /* Inline SVG is a replaced element as far as CSS layout is
     * concerned; its subtree is painted by DfBrSvg, not by this
     * engine, and its children don't participate in the surrounding
     * box tree. */
    case DFBR_HTML_TAG_SVG:
    /* Form controls: treated as opaque replaced boxes with intrinsic
     * default sizes (see intrinsic sizing in DfBrLayoutInline.c).
     * Authors can still override via CSS width/height. */
    case DFBR_HTML_TAG_INPUT:
    case DFBR_HTML_TAG_TEXTAREA:
    case DFBR_HTML_TAG_SELECT:
      return DFBR_TRUE;
    default:
      return DFBR_FALSE;
  }
}

/* ------------------------------------------------------------------
 *  Whitespace collapsing for text nodes
 *
 *  v1 implements the default "white-space: normal" rule:
 *    - runs of ASCII whitespace collapse to a single U+0020
 *    - leading/trailing whitespace is kept as a single space if
 *      non-empty (so inline siblings separated by newlines keep
 *      word boundaries); the inline layout pass will drop it when
 *      it falls at a line boundary.
 *  "white-space: pre" (and friends) is not yet implemented - text
 *  inside <pre> will look a bit wrong until DfBrLayoutInline.c
 *  gains a pre-wrap path.
 * -----------------------------------------------------------------*/

static char *
CollapseWhitespace (DfBrLayoutArena *A,
                    const char *Src, usize SrcLen,
                    usize *OutLen)
{
  char  *Dst;
  usize  I;
  usize  J      = 0;
  b32    PrevWs = DFBR_TRUE;   /* start "in whitespace" to trim leading */

  if (SrcLen == 0) { *OutLen = 0; return DfBrLayoutArenaStrdupN (A, "", 0); }
  Dst = (char *)DfBrLayoutArenaAlloc (A, SrcLen + 1);
  if (Dst == DFBR_NULL) { *OutLen = 0; return DFBR_NULL; }

  for (I = 0; I < SrcLen; I++) {
    unsigned char C = (unsigned char)Src[I];
    b32 IsWs = (C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f');
    if (IsWs) {
      if (!PrevWs) { Dst[J++] = ' '; PrevWs = DFBR_TRUE; }
    } else {
      Dst[J++] = (char)C;
      PrevWs = DFBR_FALSE;
    }
  }
  /* Drop a single trailing space that came from a whitespace tail. */
  if (J > 0 && Dst[J - 1] == ' ' && PrevWs) {
    /* Keep it: a space between adjacent inline text/element runs is
     * semantically meaningful. Inline layout trims when it lands at
     * a line boundary. */
  }
  Dst[J] = '\0';
  *OutLen = J;
  return Dst;
}

/* ------------------------------------------------------------------
 *  Recursive tree builder
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrLayoutBox *Anon;      /* current anonymous block we're appending into, or NULL */
} BuildState;

static void
FlushAnon (BuildState *S) { S->Anon = DFBR_NULL; }

/* When appending to a block container, CSS requires that inline
 * children be wrapped in an anonymous block if the container has
 * any block children. We implement the simpler invariant: if a
 * block container will end up with any block child, wrap every run
 * of inline children in an anon block. We detect "will end up with
 * any block child" by a quick pre-scan. */
static b32
ContainerWillHaveBlocks (const DfBrHtmlNode *Dom) {
  const DfBrHtmlNode *C;
  if (Dom == DFBR_NULL) return DFBR_FALSE;
  for (C = Dom->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    const DfBrCssComputedStyle *Cs;
    if (C->Kind != DFBR_HTML_NODE_ELEMENT) continue;
    Cs = DfBrCssGetComputedStyle (C);
    if (Cs == DFBR_NULL) continue;
    if (Cs->Display == DFBR_CSS_DISPLAY_NONE) continue;
    if (IsBlockDisplay (Cs->Display)) return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* Append `Child` under `Parent`, inserting an anonymous block wrapper
 * when mixing inline children into a block parent. */
static void
AppendIntoParent (DfBrLayoutEngine *E,
                  DfBrLayoutBox *Parent,
                  DfBrLayoutBox *Child,
                  BuildState *S,
                  b32 ParentMixesBlocks)
{
  if (ParentMixesBlocks && DfBrLayoutIsInlineLike (Child)) {
    if (S->Anon == DFBR_NULL) {
      S->Anon = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_ANON_BLOCK,
                                  DFBR_NULL, Parent->Style);
      DfBrLayoutBoxAppend (Parent, S->Anon);
    }
    DfBrLayoutBoxAppend (S->Anon, Child);
    return;
  }
  if (DfBrLayoutIsBlockLike (Child)) FlushAnon (S);
  DfBrLayoutBoxAppend (Parent, Child);
}

DfBrLayoutBox *
DfBrLayoutBuildSubtree (
  DfBrLayoutEngine    *E,
  const DfBrHtmlNode  *DomNode
  )
{
  const DfBrCssComputedStyle *Style;
  DfBrLayoutBox              *Self;
  const DfBrHtmlNode         *C;
  BuildState                  State;
  b32                         Mix;

  if (DomNode == DFBR_NULL) return DFBR_NULL;

  /* --- TEXT nodes ------------------------------------------------- */
  if (DomNode->Kind == DFBR_HTML_NODE_TEXT) {
    usize Len;
    char *S;
    const DfBrCssComputedStyle *Parent =
      (DomNode->Parent != DFBR_NULL) ? DfBrCssGetComputedStyle (DomNode->Parent) : DFBR_NULL;
    if (DomNode->Text == DFBR_NULL || DomNode->TextLen == 0) return DFBR_NULL;
    S = CollapseWhitespace (&E->Arena, DomNode->Text, DomNode->TextLen, &Len);
    if (S == DFBR_NULL || Len == 0) return DFBR_NULL;
    /* A pure whitespace-only run collapses to a single space; keep
     * it so inline layout can produce a word gap. */
    {
      DfBrLayoutBox *T = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_TEXT,
                                           DomNode, Parent);
      if (T == DFBR_NULL) return DFBR_NULL;
      T->TextData = S;
      T->TextLen  = Len;
      return T;
    }
  }

  /* --- Skip doctype / comment / document (document handled
   *     separately; we descend from html). --------------------------- */
  if (DomNode->Kind != DFBR_HTML_NODE_ELEMENT) return DFBR_NULL;

  /* Never render these: they're either metadata (head / meta / link /
   * title / base) or content that feeds other pipelines (script / style
   * / template). Dropping them here is simpler than teaching every
   * formatting-context rule about them. Style text still reaches the
   * cascade via CollectInlineStylesheets, and script text still reaches
   * the JS runtime via RunInlineScripts, both BEFORE layout runs, so
   * skipping the visual representation is safe.
   *
   * <noscript> is intentionally NOT in this list: HTML spec says
   * noscript content is parsed as raw text in script-enabled UAs, but
   * our parser leaves the children in the DOM. Rendering them gives
   * users a visible fallback on pages that still rely on server-side
   * noscript HTML (Google's search page without full JS, reactos.org's
   * mobile prompt, etc). The CSS cascade separately drops any <style>
   * inside <noscript> so `noscript { display:none }` style hacks don't
   * blank out sibling content. */
  switch (DomNode->Tag) {
    case DFBR_HTML_TAG_SCRIPT:
    case DFBR_HTML_TAG_STYLE:
    case DFBR_HTML_TAG_TEMPLATE:
    case DFBR_HTML_TAG_HEAD:
    case DFBR_HTML_TAG_META:
    case DFBR_HTML_TAG_LINK:
    case DFBR_HTML_TAG_TITLE:
    case DFBR_HTML_TAG_BASE:
      return DFBR_NULL;
    default: break;
  }

  Style = DfBrCssGetComputedStyle (DomNode);
  if (Style == DFBR_NULL) return DFBR_NULL;
  if (Style->Display == DFBR_CSS_DISPLAY_NONE) return DFBR_NULL;

  /* Replaced inline elements. */
  if (IsReplacedTag (DomNode->Tag)) {
    Self = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_REPLACED, DomNode, Style);
    /* <img> decode hook. DfBrImageDecodeFromUrl dispatches on scheme:
     * data: URLs decode inline; http(s) URLs (absolute, or relative
     * against E->BaseUrl) go through DfBrNet; everything else -
     * file://, blob:, relative-against-file-base - falls back to the
     * checker placeholder path. */
    if (Self != DFBR_NULL && DomNode->Tag == DFBR_HTML_TAG_IMG) {
      const char *Src = DfBrHtmlNodeGetAttrValue (DomNode, "src");
      if (Src != DFBR_NULL && Src[0] != '\0') {
        struct DfBrLayoutImageNode *N;
        N = (struct DfBrLayoutImageNode *)calloc (1, sizeof (*N));
        if (N != DFBR_NULL) {
          if (DfBrImageDecodeFromUrl (Src, E->BaseUrl, &N->Img)) {
            N->Next     = E->Images;
            E->Images   = N;
            Self->ImagePixels = N->Img.Pixels;
            Self->ImageW      = N->Img.Width;
            Self->ImageH      = N->Img.Height;
          } else {
            free (N);
          }
        }
      }
    }
    /* <canvas>: wire a pixel backing so the paint pass can blit its
     * contents as if it were a decoded image. JS drawing commands
     * mutate the same buffer in place, so a later re-paint picks up
     * whatever the script drew. Size comes from width/height attrs,
     * falling back to the HTML-spec 300x150 defaults. Cast away the
     * Dom const because DfBrCanvasEnsure writes into ScriptData, a
     * slot reserved for subsystem state by the node's own contract
     * (see DfBrHtml.h). */
    if (Self != DFBR_NULL && DomNode->Tag == DFBR_HTML_TAG_CANVAS) {
      u32 Cw = 0, Ch = 0;
      const char *Wa = DfBrHtmlNodeGetAttrValue (DomNode, "width");
      const char *Ha = DfBrHtmlNodeGetAttrValue (DomNode, "height");
      if (Wa != DFBR_NULL) { long V = strtol (Wa, DFBR_NULL, 10); if (V > 0) Cw = (u32)V; }
      if (Ha != DFBR_NULL) { long V = strtol (Ha, DFBR_NULL, 10); if (V > 0) Ch = (u32)V; }
      DfBrCanvasBacking *B = DfBrCanvasEnsure ((DfBrHtmlNode *)DomNode,
                                               E->Doc, Cw, Ch);
      if (B != DFBR_NULL) {
        Self->ImagePixels = B->Pixels;
        Self->ImageW      = B->Width;
        Self->ImageH      = B->Height;
      }
    }
    return Self;
  }

  /* Flex / grid blockify their items: any non-replaced child of a
   * `display: flex | grid` container becomes a block-level box
   * regardless of its own `display` value (CSS flexbox §4 "4. Flex
   * items"). This lets LayoutFlex pack them horizontally and is a
   * no-op for non-flex parents. */
  {
    b32 Blockify = DFBR_FALSE;
    if (DomNode->Parent != DFBR_NULL) {
      const DfBrCssComputedStyle *Ps =
        DfBrCssGetComputedStyle (DomNode->Parent);
      if (Ps != DFBR_NULL &&
          (Ps->Display == DFBR_CSS_DISPLAY_FLEX ||
           Ps->Display == DFBR_CSS_DISPLAY_GRID))
      {
        Blockify = DFBR_TRUE;
      }
      /* Inline-flex should also blockify children per spec, but our
       * flex engine does not yet wrap anonymous text runs into flex
       * items, so blanket-blockifying produces a zero-sized
       * "lowcoregames" text when it sits directly inside an
       * inline-flex `.brand` anchor. Until DfBrLayoutFlex grows an
       * anonymous-flex-item wrapper for raw text, we fall back to
       * the inline path, and the `inline-sized` decorator below in
       * DfBrLayoutInline.c handles the common decorative empty
       * child (icon / dot / pill) shape. */
    }

    /* HTML5 / CSS Display L3: if an inline element has a block-level
     * in-flow descendant, the parent inline formatting context must
     * be broken around it. Real engines either split the inline or
     * (as a pragmatic shortcut - what Ladybird and Blink's layout
     * tree do in similar cases) blockify the inline. Without this,
     * patterns like <a><div>...</div><img></a> (very common on
     * google.com's account-nav anchor) end up routing block kids
     * through an inline parent, which corrupts the inner coordinate
     * space - the child ends up at viewport (0, 0). Blockifying the
     * ancestor keeps things correct enough to render. */
    if (!IsBlockDisplay (Style->Display) && !Blockify &&
        ContainerWillHaveBlocks (DomNode))
    {
      Blockify = DFBR_TRUE;
    }

    if (IsBlockDisplay (Style->Display) || Blockify) {
      Self = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_BLOCK, DomNode, Style);
    } else {
      Self = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_INLINE, DomNode, Style);
    }
  }
  if (Self == DFBR_NULL) return DFBR_NULL;

  /* Recurse over children. */
  State.Anon = DFBR_NULL;
  Mix        = DfBrLayoutIsBlockLike (Self) ? ContainerWillHaveBlocks (DomNode) : DFBR_FALSE;

  for (C = DomNode->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    DfBrLayoutBox *Kid = DfBrLayoutBuildSubtree (E, C);
    if (Kid == DFBR_NULL) continue;
    AppendIntoParent (E, Self, Kid, &State, Mix);
  }

  return Self;
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

DfBrLayoutEngine *
DfBrLayoutBuild (
  DfBrHtmlDocument     *Doc,
  DfBrCssStyleContext  *Sc
  )
{
  return DfBrLayoutBuildWithBase (Doc, Sc, DFBR_NULL);
}

DfBrLayoutEngine *
DfBrLayoutBuildWithBase (
  DfBrHtmlDocument     *Doc,
  DfBrCssStyleContext  *Sc,
  const char           *BaseUrl
  )
{
  DfBrLayoutEngine   *E;
  const DfBrHtmlNode *Root;
  DfBrLayoutBox      *HtmlBox;

  if (Doc == DFBR_NULL || Sc == DFBR_NULL) return DFBR_NULL;

  E = (DfBrLayoutEngine *)calloc (1, sizeof (*E));
  if (E == DFBR_NULL) return DFBR_NULL;
  E->Doc = Doc;
  E->Sc  = Sc;

  if (BaseUrl != DFBR_NULL) {
    usize Len = strlen (BaseUrl);
    E->BaseUrl = (char *)malloc (Len + 1);
    if (E->BaseUrl != DFBR_NULL) {
      memcpy (E->BaseUrl, BaseUrl, Len);
      E->BaseUrl[Len] = '\0';
    }
  }

  /* Synthetic root = initial containing block. Style stays NULL; we
   * do not inherit from it. */
  E->Root = DfBrLayoutBoxNew (E, DFBR_LAYOUT_BOX_BLOCK, DFBR_NULL, DFBR_NULL);
  if (E->Root == DFBR_NULL) { free (E); return DFBR_NULL; }
  E->Root->FontSizePx = 16.0f;

  Root = DfBrHtmlDocumentRoot (Doc);
  if (Root != DFBR_NULL) {
    /* DfBrHtmlDocumentRoot() returns the synthetic #document node;
     * descend through its children. */
    const DfBrHtmlNode *C;
    for (C = Root->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
      HtmlBox = DfBrLayoutBuildSubtree (E, C);
      if (HtmlBox != DFBR_NULL) DfBrLayoutBoxAppend (E->Root, HtmlBox);
    }
  }

  return E;
}

void
DfBrLayoutEngineDestroy (DfBrLayoutEngine *E) {
  struct DfBrLayoutImageNode *In, *Next;
  if (E == DFBR_NULL) return;
  for (In = E->Images; In != DFBR_NULL; In = Next) {
    Next = In->Next;
    DfBrImageFree (&In->Img);
    free (In);
  }
  if (E->BaseUrl != DFBR_NULL) free (E->BaseUrl);
  DfBrLayoutArenaDispose (&E->Arena);
  free (E);
}

const DfBrLayoutBox *
DfBrLayoutEngineRoot (const DfBrLayoutEngine *E) {
  return (E != DFBR_NULL) ? E->Root : DFBR_NULL;
}

const DfBrLayoutBox *
DfBrLayoutBoxNextPreorder (const DfBrLayoutBox *Root,
                           const DfBrLayoutBox *Cur)
{
  if (Root == DFBR_NULL) return DFBR_NULL;
  if (Cur == DFBR_NULL) return Root;
  if (Cur->FirstChild != DFBR_NULL) return Cur->FirstChild;
  for (;;) {
    if (Cur == Root) return DFBR_NULL;
    if (Cur->NextSibling != DFBR_NULL) return Cur->NextSibling;
    Cur = Cur->Parent;
    if (Cur == DFBR_NULL) return DFBR_NULL;
  }
}

/* ------------------------------------------------------------------
 *  Hit testing
 *
 *  Paint draws children last-to-first-by-document-order but layer
 *  compositing gets this "right" by revisiting later siblings over
 *  earlier ones. For hit testing we need the reverse: the point
 *  belongs to the last-drawn (topmost) descendant that covers it.
 *  The recursion below walks children last-to-first so the deepest
 *  late-in-DOM hit wins, which matches what users expect when two
 *  overlapping elements both would accept the click.
 *
 *  Border-rect, not content-rect, is used so padding / border
 *  zones still generate hits (real browsers do the same).
 *
 *  Anonymous boxes are returned verbatim; callers that care about
 *  DOM identity walk Parent themselves.
 * -----------------------------------------------------------------*/

static b32
PointInBorderBox (const DfBrLayoutBox *Box, f32 X, f32 Y)
{
  f32 L, T, R, B;
  if (Box == DFBR_NULL) return DFBR_FALSE;
  L = Box->ContentRect.X - Box->Padding.Left   - Box->Border.Left;
  T = Box->ContentRect.Y - Box->Padding.Top    - Box->Border.Top;
  R = Box->ContentRect.X + Box->ContentRect.W + Box->Padding.Right  + Box->Border.Right;
  B = Box->ContentRect.Y + Box->ContentRect.H + Box->Padding.Bottom + Box->Border.Bottom;
  return (X >= L && X < R && Y >= T && Y < B) ? DFBR_TRUE : DFBR_FALSE;
}

static const DfBrLayoutBox *
HitTestRecursive (const DfBrLayoutBox *Box, f32 X, f32 Y)
{
  const DfBrLayoutBox *C;
  const DfBrLayoutBox *Hit;

  if (Box == DFBR_NULL) return DFBR_NULL;
  /* Walk children last-first so topmost (latest drawn) siblings win. */
  for (C = Box->LastChild; C != DFBR_NULL; C = C->PrevSibling) {
    Hit = HitTestRecursive (C, X, Y);
    if (Hit != DFBR_NULL) return Hit;
  }
  if (PointInBorderBox (Box, X, Y)) return Box;
  return DFBR_NULL;
}

const DfBrLayoutBox *
DfBrLayoutHitTest (
  const DfBrLayoutEngine *Engine,
  f32                     X,
  f32                     Y
  )
{
  const DfBrLayoutBox *Root;
  if (Engine == DFBR_NULL) return DFBR_NULL;
  Root = DfBrLayoutEngineRoot (Engine);
  return HitTestRecursive (Root, X, Y);
}

const DfBrLayoutBox *
DfBrLayoutBoxForDomNode (
  const DfBrLayoutEngine *Engine,
  const DfBrHtmlNode     *Node
  )
{
  const DfBrLayoutBox *Root;
  const DfBrLayoutBox *Cur;

  if (Engine == DFBR_NULL || Node == DFBR_NULL) return DFBR_NULL;
  Root = DfBrLayoutEngineRoot (Engine);
  for (Cur = DFBR_NULL;;) {
    Cur = DfBrLayoutBoxNextPreorder (Root, Cur);
    if (Cur == DFBR_NULL) break;
    if (Cur->DomNode == Node) return Cur;
  }
  return DFBR_NULL;
}

/* ------------------------------------------------------------------
 *  Intrinsic sizing for REPLACED boxes
 * -----------------------------------------------------------------*/

/* Parse a bare length attribute like "272", "272px", or "100%". We
 * only honor integer/decimal px values here; percent and everything
 * else collapse to 0 so the fallback path in the caller kicks in. */
static f32
ParseAttrLengthPx (const char *S)
{
  f32  Acc   = 0.0f;
  f32  Frac  = 0.0f;
  f32  Scale = 0.1f;
  b32  Neg   = DFBR_FALSE;
  b32  AnyDigit = DFBR_FALSE;
  b32  InFrac   = DFBR_FALSE;
  if (S == DFBR_NULL) return 0.0f;
  while (*S == ' ' || *S == '\t') S++;
  if (*S == '-') { Neg = DFBR_TRUE; S++; }
  else if (*S == '+') S++;
  while (*S != '\0') {
    char C = *S++;
    if (C >= '0' && C <= '9') {
      AnyDigit = DFBR_TRUE;
      if (!InFrac) {
        Acc = Acc * 10.0f + (f32)(C - '0');
      } else {
        Frac += (f32)(C - '0') * Scale;
        Scale *= 0.1f;
      }
      continue;
    }
    if (C == '.' && !InFrac) { InFrac = DFBR_TRUE; continue; }
    /* End of numeric portion - accept "272px", "272.5", "272 " etc.
     * Percent signals relative sizing; caller handles fallback. */
    break;
  }
  if (!AnyDigit) return 0.0f;
  Acc += Frac;
  if (Neg) Acc = -Acc;
  return Acc;
}

/* Parse "minX minY width height" from an SVG viewBox. Returns the
 * width and height via out params; returns DFBR_TRUE on success. */
static b32
ParseViewBoxWH (const char *S, f32 *OutW, f32 *OutH)
{
  f32 Values[4] = { 0, 0, 0, 0 };
  int Idx       = 0;
  if (S == DFBR_NULL) return DFBR_FALSE;
  while (*S != '\0' && Idx < 4) {
    while (*S == ' ' || *S == '\t' || *S == ',' ||
           *S == '\n' || *S == '\r')
    {
      S++;
    }
    if (*S == '\0') break;
    Values[Idx++] = ParseAttrLengthPx (S);
    /* Skip over the token just read. */
    while (*S != '\0' && *S != ' ' && *S != '\t' && *S != ',' &&
           *S != '\n' && *S != '\r')
    {
      S++;
    }
  }
  if (Idx < 4) return DFBR_FALSE;
  if (OutW != DFBR_NULL) *OutW = Values[2];
  if (OutH != DFBR_NULL) *OutH = Values[3];
  return DFBR_TRUE;
}

/* Default intrinsic size per-type for form controls. Mirrors what
 * Chrome/Firefox have hard-coded in their user-agent stylesheets. */
static void
DefaultInputSize (const DfBrHtmlNode *Dom, f32 *W, f32 *H)
{
  const char *Type = DfBrHtmlNodeGetAttrValue (Dom, "type");
  f32 Dw = 160.0f;
  f32 Dh = 22.0f;
  if (Type != DFBR_NULL) {
    /* Case-insensitive shallow compare is fine here. */
    if (strcmp (Type, "checkbox") == 0 || strcmp (Type, "radio") == 0) {
      Dw = 13.0f; Dh = 13.0f;
    } else if (strcmp (Type, "submit") == 0 ||
               strcmp (Type, "reset")  == 0 ||
               strcmp (Type, "button") == 0)
    {
      Dw = 72.0f; Dh = 22.0f;
    } else if (strcmp (Type, "color") == 0) {
      Dw = 44.0f; Dh = 22.0f;
    } else if (strcmp (Type, "image") == 0) {
      Dw = 0.0f;  Dh = 0.0f;
    } else if (strcmp (Type, "hidden") == 0) {
      Dw = 0.0f;  Dh = 0.0f;
    } else if (strcmp (Type, "range") == 0) {
      Dw = 129.0f; Dh = 22.0f;
    }
  }
  *W = Dw; *H = Dh;
}

void
DfBrLayoutIntrinsicReplacedSize (
  const DfBrLayoutBox *Box,
  f32                 *OutW,
  f32                 *OutH
  )
{
  const DfBrHtmlNode *Dom;
  f32                 W = 0.0f;
  f32                 H = 0.0f;
  const char         *A;

  if (OutW != DFBR_NULL) *OutW = 0.0f;
  if (OutH != DFBR_NULL) *OutH = 0.0f;
  if (Box == DFBR_NULL || Box->DomNode == DFBR_NULL) return;
  Dom = Box->DomNode;

  switch (Dom->Tag) {
    case DFBR_HTML_TAG_SVG: {
      /* SVG integration: width/height come from the width/height
       * attributes *or* CSS; viewBox only defines the user-coordinate
       * system, it is not a display size. If neither side is given
       * we fall back to the 300 x 150 replaced default. When exactly
       * one side is given we preserve the viewBox aspect ratio. */
      f32 Vw = 0.0f, Vh = 0.0f;
      b32 HaveVb = DFBR_FALSE;

      A = DfBrHtmlNodeGetAttrValue (Dom, "width");
      if (A != DFBR_NULL) W = ParseAttrLengthPx (A);
      A = DfBrHtmlNodeGetAttrValue (Dom, "height");
      if (A != DFBR_NULL) H = ParseAttrLengthPx (A);

      A = DfBrHtmlNodeGetAttrValue (Dom, "viewBox");
      if (A == DFBR_NULL) A = DfBrHtmlNodeGetAttrValue (Dom, "viewbox");
      if (A != DFBR_NULL) HaveVb = ParseViewBoxWH (A, &Vw, &Vh);

              if (W > 0.0f && H <= 0.0f) {
                H = (HaveVb && Vw > 0.0f) ? W * (Vh / Vw) : 150.0f;
              } else if (H > 0.0f && W <= 0.0f) {
                W = (HaveVb && Vh > 0.0f) ? H * (Vw / Vh) : 300.0f;
              } else if (W <= 0.0f && H <= 0.0f) {
                /* CSS 2.1 says the default replaced size is 300x150,
                 * but for pages that style SVGs via class-based CSS
                 * we don't yet cascade (icons typically), that
                 * default is way too big. Fall back to the viewBox
                 * dimensions when available - scaled to cap the
                 * longest side at ~24px so icon-style SVGs (common
                 * viewBox values like "0 0 24 24" or "0 -960 960
                 * 960") render at sensible visual sizes in the
                 * absence of explicit sizing. */
                if (HaveVb && Vw > 0.0f && Vh > 0.0f) {
                  f32 IconCap = 24.0f;
                  f32 Larger  = (Vw > Vh) ? Vw : Vh;
                  f32 Scale   = (Larger > IconCap) ? (IconCap / Larger) : 1.0f;
                  W = Vw * Scale;
                  H = Vh * Scale;
                } else {
                  W = 300.0f;
                  H = 150.0f;
                }
              }
      break;
    }

    case DFBR_HTML_TAG_IMG: {
      f32 Iw = (Box->ImagePixels != DFBR_NULL) ? (f32)Box->ImageW : 0.0f;
      f32 Ih = (Box->ImagePixels != DFBR_NULL) ? (f32)Box->ImageH : 0.0f;
      A = DfBrHtmlNodeGetAttrValue (Dom, "width");
      if (A != DFBR_NULL) W = ParseAttrLengthPx (A);
      A = DfBrHtmlNodeGetAttrValue (Dom, "height");
      if (A != DFBR_NULL) H = ParseAttrLengthPx (A);
      /* Fall back to the decoded bitmap's intrinsic size when only
       * one axis was authored, preserving aspect ratio (CSS
       * "intrinsic aspect ratio" per CSS Images §5.1). */
      if (Iw > 0.0f && Ih > 0.0f) {
        if (W <= 0.0f && H <= 0.0f) { W = Iw; H = Ih; }
        else if (W <= 0.0f)         { W = Iw * (H / Ih); }
        else if (H <= 0.0f)         { H = Ih * (W / Iw); }
      }
      break;
    }

    case DFBR_HTML_TAG_INPUT: {
      DefaultInputSize (Dom, &W, &H);
      A = DfBrHtmlNodeGetAttrValue (Dom, "width");
      if (A != DFBR_NULL) { f32 V = ParseAttrLengthPx (A); if (V > 0.0f) W = V; }
      A = DfBrHtmlNodeGetAttrValue (Dom, "height");
      if (A != DFBR_NULL) { f32 V = ParseAttrLengthPx (A); if (V > 0.0f) H = V; }
      break;
    }

    case DFBR_HTML_TAG_TEXTAREA: {
      /* cols/rows aren't parsed yet; use a "looks like" default. */
      W = 160.0f; H = 60.0f;
      break;
    }

    case DFBR_HTML_TAG_SELECT: {
      W = 120.0f; H = 22.0f;
      break;
    }

    case DFBR_HTML_TAG_IFRAME:
    case DFBR_HTML_TAG_CANVAS: {
      A = DfBrHtmlNodeGetAttrValue (Dom, "width");
      if (A != DFBR_NULL) W = ParseAttrLengthPx (A);
      A = DfBrHtmlNodeGetAttrValue (Dom, "height");
      if (A != DFBR_NULL) H = ParseAttrLengthPx (A);
      if (W <= 0.0f) W = 300.0f;
      if (H <= 0.0f) H = 150.0f;
      break;
    }

    default:
      break;
  }

  if (OutW != DFBR_NULL) *OutW = W;
  if (OutH != DFBR_NULL) *OutH = H;
}
