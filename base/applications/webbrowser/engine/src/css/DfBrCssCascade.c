/** @file
  DfBrCssCascade.c - apply CSS stylesheets to an HTML document.

  Pipeline per element (pre-order walk of the DOM):
    1. Allocate a DfBrCssComputedStyle on the style context arena.
    2. Seed inherited values from the parent (root seeds from defaults).
    3. Collect every matching declaration from every sheet into a
       scratch buffer tagged with (bucket, specificity, source order).
    4. If the element has a `style=""` attribute, parse it and collect
       those declarations with the inline bucket.
    5. Sort the collection by cascade ordering.
    6. Apply declarations in ascending sort order so later ones
       overwrite earlier ones in the computed style.
    7. Hang the computed style off DfBrHtmlNode::StyleData.

  Destroying the style context clears StyleData on every node before
  freeing the arena, so the void* slot never dangles.
**/

#include "DfBrCssInternal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 *  Local helpers
 * -----------------------------------------------------------------*/

typedef struct {
  const DfBrCssDeclaration *Decl;
  u32                       Specificity;
  u32                       SourceOrder;
  u8                        Bucket;
    /* 0 = UA normal
     * 1 = Author normal
     * 2 = Inline normal
     * 3 = Author important
     * 4 = Inline important
     * 5 = UA important  */
} Entry;

typedef struct {
  Entry  *Items;
  usize   Count;
  usize   Cap;
} EntryList;

static void
ListPush (EntryList *L, const Entry *E) {
  if (L->Count == L->Cap) {
    usize  NewCap = (L->Cap == 0) ? 16 : L->Cap * 2;
    Entry *NB     = (Entry *)realloc (L->Items, NewCap * sizeof (Entry));
    if (NB == DFBR_NULL) return;
    L->Items = NB;
    L->Cap   = NewCap;
  }
  L->Items[L->Count++] = *E;
}

static int
CmpEntry (const void *A, const void *B) {
  const Entry *Ea = (const Entry *)A;
  const Entry *Eb = (const Entry *)B;
  if (Ea->Bucket      != Eb->Bucket)      return (Ea->Bucket      < Eb->Bucket)      ? -1 : 1;
  if (Ea->Specificity != Eb->Specificity) return (Ea->Specificity < Eb->Specificity) ? -1 : 1;
  if (Ea->SourceOrder != Eb->SourceOrder) return (Ea->SourceOrder < Eb->SourceOrder) ? -1 : 1;
  return 0;
}

static u8
NormalBucket (DfBrCssOrigin O) {
  switch (O) {
    case DFBR_CSS_ORIGIN_UA:     return 0;
    case DFBR_CSS_ORIGIN_USER:   return 1;
    case DFBR_CSS_ORIGIN_AUTHOR: return 1;
    default:                     return 1;
  }
}

static u8
ImportantBucket (DfBrCssOrigin O) {
  switch (O) {
    case DFBR_CSS_ORIGIN_UA:     return 5;
    case DFBR_CSS_ORIGIN_USER:   return 3;
    case DFBR_CSS_ORIGIN_AUTHOR: return 3;
    default:                     return 3;
  }
}

static u32
PackSpecificity (u32 Raw) {
  /* Selector parser packs as a<<20 | b<<10 | c, already good for direct
   * compare. */
  return Raw;
}

/* ------------------------------------------------------------------
 *  Collect matching rules for one element
 * -----------------------------------------------------------------*/

static void
CollectRules (
  EntryList                  *L,
  const DfBrCssRule          *FirstRule,
  DfBrCssOrigin               Origin,
  const DfBrHtmlNode         *Node,
  const DfBrCssMatchContext  *Ctx,
  u32                        *SourceOrderCounter
  )
{
  const DfBrCssRule *R;
  for (R = FirstRule; R != DFBR_NULL; R = R->Next) {
    u32 Spec;

    if (R->Kind == DFBR_CSS_RULE_AT) {
      /* @media: evaluate the prelude against the active viewport and
       * skip the entire inner rule set when it does not match. Unknown
       * at-rules (@supports, @document, @keyframes, @font-face, etc.)
       * are treated as always-matching so we recurse into their body
       * exactly as before. */
      if (R->InnerRules != DFBR_NULL) {
        b32 Include = DFBR_TRUE;
        if (DfBrCssAsciiEqZ (R->AtName, R->AtNameLen, "media")) {
          Include = DfBrCssMediaQueryMatches (R->AtPrelude, Ctx);
        }
        if (Include) {
          CollectRules (L, R->InnerRules, Origin, Node, Ctx, SourceOrderCounter);
        }
      }
      continue;
    }

    if (R->Selectors == DFBR_NULL) continue;
    if (!DfBrCssMatchSelectorList (R->Selectors, Node, Ctx, &Spec)) continue;

    {
      const DfBrCssDeclaration *D;
      for (D = R->FirstDecl; D != DFBR_NULL; D = D->Next) {
        Entry E;
        E.Decl        = D;
        E.Specificity = PackSpecificity (Spec);
        E.SourceOrder = (*SourceOrderCounter)++;
        E.Bucket      = D->Important ? ImportantBucket (Origin) : NormalBucket (Origin);
        ListPush (L, &E);
      }
    }
  }
}

/* ------------------------------------------------------------------
 *  Style context lifecycle
 * -----------------------------------------------------------------*/

static DfBrCssStyleContext *
StyleContextNew (DfBrHtmlDocument *Doc) {
  DfBrCssStyleContext *Sc = (DfBrCssStyleContext *)calloc (1, sizeof (*Sc));
  if (Sc == DFBR_NULL) return DFBR_NULL;
  Sc->Doc                = Doc;
  Sc->RootFontSizePx     = 16.0f;
  Sc->DefaultFontSizePx  = 16.0f;
  return Sc;
}

static void
ClearStyleSlots (const DfBrHtmlNode *Root) {
  const DfBrHtmlNode *N = DFBR_NULL;
  while ((N = DfBrHtmlNodeNextPreorder (Root, N)) != DFBR_NULL) {
    /* Cast away const; StyleData is a mutable slot by design. */
    DfBrHtmlNode *M = (DfBrHtmlNode *)N;
    M->StyleData = DFBR_NULL;
  }
}

void
DfBrCssStyleContextDestroy (DfBrCssStyleContext *Sc) {
  if (Sc == DFBR_NULL) return;
  if (Sc->Doc != DFBR_NULL) {
    const DfBrHtmlNode *Root = DfBrHtmlDocumentRoot (Sc->Doc);
    if (Root != DFBR_NULL) ClearStyleSlots (Root);
  }
  DfBrCssArenaDispose (&Sc->Arena);
  /* Diags array is heap-allocated by DfBrCssDiagEmit. */
  free (Sc->Diags.Entries);
  free (Sc);
}

DfBrHtmlDocument *
DfBrCssStyleContextDocument (DfBrCssStyleContext *Sc) {
  return (Sc != DFBR_NULL) ? Sc->Doc : DFBR_NULL;
}

const DfBrCssComputedStyle *
DfBrCssGetComputedStyle (const DfBrHtmlNode *Node) {
  if (Node == DFBR_NULL) return DFBR_NULL;
  return (const DfBrCssComputedStyle *)Node->StyleData;
}

/* ------------------------------------------------------------------
 *  Inline style
 * -----------------------------------------------------------------*/

const DfBrCssDeclaration *
DfBrCssParseStyleAttr (
  DfBrCssStyleContext  *Sc,
  const char           *Src,
  usize                 SrcLen
  )
{
  DfBrCssTokenizer  T;
  if (Sc == DFBR_NULL || Src == DFBR_NULL || SrcLen == 0) return DFBR_NULL;
  DfBrCssTokInit (&T, Src, SrcLen, &Sc->Arena, &Sc->Diags);
  return DfBrCssParseDeclarationBlock (&T, &Sc->Arena, &Sc->Diags);
}

/* ------------------------------------------------------------------
 *  Per-element cascade
 * -----------------------------------------------------------------*/

static void
CascadeElement (
  DfBrCssStyleContext             *Sc,
  DfBrHtmlNode                    *Elem,
  const DfBrCssComputedStyle      *Parent,
  const DfBrCssStylesheet *const  *Sheets,
  usize                            SheetCount,
  const DfBrCssMatchContext       *Ctx
  )
{
  EntryList L;
  u32       SO = 0;
  usize     i;
  DfBrCssComputedStyle *Cs;
  const DfBrHtmlAttr   *StyleAttr;

  memset (&L, 0, sizeof (L));

  Cs = DfBrCssComputedNew (Sc);
  if (Cs == DFBR_NULL) return;
  if (Parent != DFBR_NULL) DfBrCssComputedInherit (Cs, Parent);

  for (i = 0; i < SheetCount; i++) {
    const DfBrCssStylesheet *Sheet = Sheets[i];
    if (Sheet == DFBR_NULL) continue;
    CollectRules (&L, Sheet->FirstRule, Sheet->Options.Origin,
                  Elem, Ctx, &SO);
  }

  /* Inline style: highest "normal" bucket plus a very high specificity
   * so it beats author rules at the same bucket (spec treats inline
   * style as a special bucket above normal author). */
  StyleAttr = DfBrHtmlNodeGetAttr (Elem, "style");
  if (StyleAttr != DFBR_NULL && StyleAttr->Value != DFBR_NULL && StyleAttr->ValueLen > 0) {
    const DfBrCssDeclaration *D;
    for (D = DfBrCssParseStyleAttr (Sc, StyleAttr->Value, StyleAttr->ValueLen);
         D != DFBR_NULL; D = D->Next) {
      Entry E;
      E.Decl        = D;
      E.Specificity = 0x3fffffffu; /* pinned above any real selector */
      E.SourceOrder = SO++;
      E.Bucket      = D->Important ? 4 /* inline important */ : 2 /* inline normal */;
      ListPush (&L, &E);
    }
  }

  if (L.Count > 0) {
    qsort (L.Items, L.Count, sizeof (Entry), CmpEntry);
    for (i = 0; i < L.Count; i++) {
      DfBrCssApplyDeclaration (Sc, Cs, Parent, L.Items[i].Decl);
    }
  }

  /* Resolve font-size to a concrete px computed value. Per the CSS
   * spec, font-size's computed value is always an absolute length;
   * em/rem/%/keyword sizes must be resolved here so that descendants
   * can use their parent's resolved font-size for their own ems. */
  {
    f32 ParentFs = (Parent != DFBR_NULL) ? Parent->FontSize.Value : Sc->DefaultFontSizePx;
    f32 RootFs   = Sc->RootFontSizePx;
    f32 Resolved = Cs->FontSize.Value;
    if (Cs->FontSize.CalcExpr != DFBR_NULL) {
      /* calc() on font-size: feed the parent's font size as both
       * "current" em basis (itself) and as the percent basis. */
      DfBrCssLengthToPx (&Cs->FontSize, ParentFs, RootFs,
                         Ctx->VpWidthPx, Ctx->VpHeightPx,
                         ParentFs, &Resolved);
    } else {
      switch (Cs->FontSize.Unit) {
        case DFBR_CSS_UNIT_PX:      Resolved = Cs->FontSize.Value;             break;
        case DFBR_CSS_UNIT_EM:      Resolved = Cs->FontSize.Value * ParentFs;  break;
        case DFBR_CSS_UNIT_REM:     Resolved = Cs->FontSize.Value * RootFs;    break;
        case DFBR_CSS_UNIT_PERCENT: Resolved = Cs->FontSize.Value * ParentFs * 0.01f; break;
        case DFBR_CSS_UNIT_PT:      Resolved = Cs->FontSize.Value * (96.0f / 72.0f); break;
        case DFBR_CSS_UNIT_PC:      Resolved = Cs->FontSize.Value * 16.0f;     break;
        case DFBR_CSS_UNIT_IN:      Resolved = Cs->FontSize.Value * 96.0f;     break;
        case DFBR_CSS_UNIT_CM:      Resolved = Cs->FontSize.Value * (96.0f / 2.54f); break;
        case DFBR_CSS_UNIT_MM:      Resolved = Cs->FontSize.Value * (96.0f / 25.4f); break;
        case DFBR_CSS_UNIT_Q:       Resolved = Cs->FontSize.Value * (96.0f / 101.6f); break;
        default: break;
      }
    }
    Cs->FontSize.Unit      = DFBR_CSS_UNIT_PX;
    Cs->FontSize.Auto      = DFBR_FALSE;
    Cs->FontSize.Value     = Resolved;
    Cs->FontSize.CalcExpr  = DFBR_NULL;

    /* The root element (html) defines the rem unit. */
    if (Parent == DFBR_NULL) {
      Sc->RootFontSizePx = Resolved;
    }
  }

  free (L.Items);
  Elem->StyleData = Cs;
}

static void
CascadeRecurse (
  DfBrCssStyleContext             *Sc,
  DfBrHtmlNode                    *Node,
  const DfBrCssComputedStyle      *Parent,
  const DfBrCssStylesheet *const  *Sheets,
  usize                            SheetCount,
  const DfBrCssMatchContext       *Ctx
  )
{
  DfBrHtmlNode *Child;
  if (Node == DFBR_NULL) return;
  if (Node->Kind == DFBR_HTML_NODE_ELEMENT) {
    CascadeElement (Sc, Node, Parent, Sheets, SheetCount, Ctx);
    Parent = (const DfBrCssComputedStyle *)Node->StyleData;
  }
  for (Child = Node->FirstChild; Child != DFBR_NULL; Child = Child->NextSibling) {
    CascadeRecurse (Sc, Child, Parent, Sheets, SheetCount, Ctx);
  }
}

/* ------------------------------------------------------------------
 *  Public entry
 * -----------------------------------------------------------------*/

DfBrCssStyleContext *
DfBrCssApplyToDocument (
  DfBrHtmlDocument               *Doc,
  const DfBrCssStylesheet *const *Sheets,
  usize                           SheetCount
  )
{
  return DfBrCssApplyToDocumentEx (Doc, Sheets, SheetCount, 0.0f, 0.0f);
}

DfBrCssStyleContext *
DfBrCssApplyToDocumentEx (
  DfBrHtmlDocument               *Doc,
  const DfBrCssStylesheet *const *Sheets,
  usize                           SheetCount,
  f32                             ViewportWidthPx,
  f32                             ViewportHeightPx
  )
{
  DfBrCssStyleContext *Sc;
  DfBrCssMatchContext  Ctx;
  const DfBrHtmlNode  *Root;

  if (Doc == DFBR_NULL) return DFBR_NULL;
  Sc = StyleContextNew (Doc);
  if (Sc == DFBR_NULL) return DFBR_NULL;

  Root            = DfBrHtmlDocumentRoot (Doc);
  Ctx.Root        = Root;
  Ctx.VpWidthPx   = ViewportWidthPx;
  Ctx.VpHeightPx  = ViewportHeightPx;

  if (Root != DFBR_NULL) {
    CascadeRecurse (Sc, (DfBrHtmlNode *)Root, DFBR_NULL,
                    Sheets, SheetCount, &Ctx);
  }
  return Sc;
}
