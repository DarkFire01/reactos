/** @file
  DfBrCssInternal.h - shared internals across DfBrCss TUs.
  Not part of the public API.
**/

#ifndef DFBR_CSS_INTERNAL_H_
#define DFBR_CSS_INTERNAL_H_

#include "DfBrCss.h"

#include <stddef.h>

/* ------------------------------------------------------------------
 *  Arena
 *
 *  Mirrors DfBrHtml's chunked bump allocator. Each CSS subsystem
 *  that needs a lifetime (stylesheet, style context, scratch) owns
 *  one. Freeing the arena walks the chunk list once.
 * -----------------------------------------------------------------*/

typedef struct DfBrCssArenaChunk {
  struct DfBrCssArenaChunk  *Next;
  usize                      Used;
  usize                      Cap;
  u8                         Data[1];   /* flexible tail */
} DfBrCssArenaChunk;

typedef struct {
  DfBrCssArenaChunk  *First;
  DfBrCssArenaChunk  *Current;
  usize               TotalBytes;
} DfBrCssArena;

void   *DfBrCssArenaAlloc       (DfBrCssArena *A, usize Size);
void   *DfBrCssArenaZero        (DfBrCssArena *A, usize Size);
char   *DfBrCssArenaStrDup      (DfBrCssArena *A, const char *S, usize Len);
char   *DfBrCssArenaStrDupLower (DfBrCssArena *A, const char *S, usize Len);
void    DfBrCssArenaDispose     (DfBrCssArena *A);

/* ------------------------------------------------------------------
 *  Diagnostics list
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssDiagEntry  *Entries;
  usize              Count;
  usize              Cap;
} DfBrCssDiagList;

void DfBrCssDiagEmit (
  DfBrCssDiagList  *List,
  DfBrCssArena     *A,
  i32               Line,
  i32               Column,
  const char       *Message
  );

/* ------------------------------------------------------------------
 *  Tokens (CSS Syntax Module Level 3)
 *
 *  Names follow the spec so a reader can grep the spec straight
 *  into the tokenizer.
 * -----------------------------------------------------------------*/

typedef enum {
  DfBrCssTokEof = 0,
  DfBrCssTokWhitespace,
  DfBrCssTokIdent,
  DfBrCssTokFunction,        /* ident followed by `(` */
  DfBrCssTokAtKeyword,       /* `@ident` */
  DfBrCssTokHash,            /* `#name` (IdLike or Unrestricted) */
  DfBrCssTokString,
  DfBrCssTokBadString,
  DfBrCssTokUrl,
  DfBrCssTokBadUrl,
  DfBrCssTokDelim,           /* single codepoint that did not start anything else */
  DfBrCssTokNumber,
  DfBrCssTokPercentage,
  DfBrCssTokDimension,
  DfBrCssTokCdo,             /* <!-- */
  DfBrCssTokCdc,             /* --> */
  DfBrCssTokColon,
  DfBrCssTokSemicolon,
  DfBrCssTokComma,
  DfBrCssTokLBracket,
  DfBrCssTokRBracket,
  DfBrCssTokLParen,
  DfBrCssTokRParen,
  DfBrCssTokLBrace,
  DfBrCssTokRBrace,
  DfBrCssTokInclude,         /* ~= */
  DfBrCssTokDashMatch,       /* |= */
  DfBrCssTokPrefixMatch,     /* ^= */
  DfBrCssTokSuffixMatch,     /* $= */
  DfBrCssTokSubstringMatch,  /* *= */
  DfBrCssTokColumn           /* || */
} DfBrCssTokKind;

typedef enum {
  DfBrCssHashUnrestricted,
  DfBrCssHashIdLike
} DfBrCssHashFlag;

typedef struct {
  DfBrCssTokKind   Kind;
  /* Source span */
  const char      *Start;
  usize            Len;
  i32              Line;
  i32              Column;

  /* Ident / Function / AtKeyword / Hash / String / Url / Delim value.
   * Kept arena-owned, already case-preserved but strip-escaped and
   * stripped of quotes (for strings) and surrounding markers. */
  const char      *Str;
  usize            StrLen;

  /* Number / Percentage / Dimension payload. */
  f64              Number;
  b32              IsInteger;
  b32              HasSign;

  /* Dimension unit ident (arena-owned, lowercased). */
  const char      *Unit;
  usize            UnitLen;
  DfBrCssUnit      UnitId;

  /* Hash type. */
  DfBrCssHashFlag  HashFlag;
} DfBrCssToken;

/* ------------------------------------------------------------------
 *  Tokenizer state
 * -----------------------------------------------------------------*/

typedef struct DfBrCssTokenizer {
  const char    *Src;
  usize          Len;
  usize          Pos;
  i32            Line;
  i32            Column;
  DfBrCssArena  *Arena;
  DfBrCssDiagList *Diags;
} DfBrCssTokenizer;

void  DfBrCssTokInit (
  DfBrCssTokenizer  *T,
  const char        *Src,
  usize              Len,
  DfBrCssArena      *Arena,
  DfBrCssDiagList   *Diags
  );

void  DfBrCssTokNext (DfBrCssTokenizer *T, DfBrCssToken *Out);

/* Unit name -> enum. Returns DFBR_CSS_UNIT_UNKNOWN if unrecognised. */
DfBrCssUnit DfBrCssUnitFromName (const char *Name, usize Len);
const char *DfBrCssUnitName     (DfBrCssUnit U);

/* ------------------------------------------------------------------
 *  Component values (the AST-ish payload of declarations + at-rule
 *  preludes)
 *
 *  A contiguous run of non-whitespace component values represents
 *  one "value expression". Functions own their argument list via
 *  `Children`; the list terminates at the matching `)`. Blocks
 *  (used only at top level by at-rules) own their content via
 *  `Children` terminating at `}` / `]`.
 * -----------------------------------------------------------------*/

typedef enum {
  DfBrCssCvKindIdent,
  DfBrCssCvKindFunction,     /* name(...) -> Children is arg list */
  DfBrCssCvKindAtKeyword,
  DfBrCssCvKindHash,         /* #foo / #fff */
  DfBrCssCvKindString,
  DfBrCssCvKindUrl,
  DfBrCssCvKindNumber,
  DfBrCssCvKindPercentage,
  DfBrCssCvKindDimension,
  DfBrCssCvKindDelim,        /* a single codepoint delim */
  DfBrCssCvKindColon,
  DfBrCssCvKindComma,
  DfBrCssCvKindWhitespace,   /* elided by default; used in a few places */
  DfBrCssCvKindBlock,        /* [ ... ] or { ... } (non-top-level) */
  DfBrCssCvKindInclude,      /* ~= */
  DfBrCssCvKindDashMatch,    /* |= */
  DfBrCssCvKindPrefixMatch,  /* ^= */
  DfBrCssCvKindSuffixMatch,  /* $= */
  DfBrCssCvKindSubstringMatch, /* *= */
  DfBrCssCvKindColumn        /* || */
} DfBrCssCvKind;

struct DfBrCssCv {
  DfBrCssCvKind     Kind;

  /* Most kinds carry a string payload (ident name, function name,
   * hash name, raw string body, url body, delim char, unit, etc.). */
  const char       *Str;
  usize             StrLen;

  /* Numeric payload for Number/Percentage/Dimension. */
  f64               Number;
  b32               IsInteger;
  b32               HasSign;

  /* Dimension unit (already-lowercased ident). */
  const char       *Unit;
  usize             UnitLen;
  DfBrCssUnit       UnitId;

  /* Block / function body. */
  struct DfBrCssCv *Children;

  /* For Block: which bracket pair (0 = `[`, 1 = `{`). */
  i32               BlockKind;

  /* Source location of the opening token. */
  i32               Line;
  i32               Column;

  struct DfBrCssCv *Next;
};

/* CSS custom property ("--foo") binding hanging off the computed
 * style. `Name` includes the leading `--`, lowercased. `Value` is the
 * parsed, var()-unsubstituted CV list exactly as authored; resolution
 * happens at use site (DfBrCssResolveVars). */
struct DfBrCssCustomProp {
  const char               *Name;     /* arena-owned; "--..." */
  usize                     NameLen;
  DfBrCssCv                *Value;    /* arena-owned CV list (may be NULL) */
  struct DfBrCssCustomProp *Next;
};

/* Resolve `var(--name [, fallback])` calls inside a CV list against an
 * element's (inherited) custom-property chain. Returns a NEW CV list
 * allocated from `A`; children lists are cloned too. When a var() can't
 * be resolved and has no fallback, this returns NULL to signal "declaration
 * becomes invalid at computed-value time" (per CSS Variables spec). */
DfBrCssCv *DfBrCssResolveVars (
  DfBrCssArena               *A,
  const DfBrCssComputedStyle *Cs,
  const DfBrCssCv            *Src
  );

/* Build a CV list out of a token stream, stopping at a top-level
 * terminator (semicolon, `}`, or EOF). Returns the first CV, sets
 * *OutStopToken to the kind of token that caused the stop. */
DfBrCssCv *DfBrCssParseCvList (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags,
  DfBrCssTokKind    *OutStopToken
  );

/* Trim leading/trailing whitespace CVs. */
DfBrCssCv *DfBrCssCvTrim (DfBrCssCv *Head);

/* ------------------------------------------------------------------
 *  Declarations
 * -----------------------------------------------------------------*/

struct DfBrCssDeclaration {
  DfBrCssProp               Prop;
  const char               *Name;       /* lowercased */
  usize                     NameLen;
  DfBrCssCv                *Value;
  b32                       Important;
  i32                       Line;
  i32                       Column;
  struct DfBrCssDeclaration *Next;
};

/* ------------------------------------------------------------------
 *  Selectors
 * -----------------------------------------------------------------*/

typedef enum {
  DfBrCssSelType,         /* ns|tag or tag - we ignore namespaces in v1 */
  DfBrCssSelUniversal,    /* * */
  DfBrCssSelId,           /* #foo */
  DfBrCssSelClass,        /* .foo */
  DfBrCssSelAttr,         /* [attr], [attr=val], etc. */
  DfBrCssSelPseudoClass,
  DfBrCssSelPseudoElement
} DfBrCssSimpleSelKind;

typedef enum {
  DfBrCssAttrExists,
  DfBrCssAttrEq,
  DfBrCssAttrIncludes,     /* ~= */
  DfBrCssAttrDashMatch,    /* |= */
  DfBrCssAttrPrefix,       /* ^= */
  DfBrCssAttrSuffix,       /* $= */
  DfBrCssAttrSubstring     /* *= */
} DfBrCssAttrOp;

typedef enum {
  DfBrCssPcUnknown = 0,
  DfBrCssPcHover,
  DfBrCssPcFocus,
  DfBrCssPcFocusWithin,
  DfBrCssPcFocusVisible,
  DfBrCssPcActive,
  DfBrCssPcFirstChild,
  DfBrCssPcLastChild,
  DfBrCssPcOnlyChild,
  DfBrCssPcFirstOfType,
  DfBrCssPcLastOfType,
  DfBrCssPcOnlyOfType,
  DfBrCssPcNthChild,
  DfBrCssPcNthLastChild,
  DfBrCssPcNthOfType,
  DfBrCssPcNthLastOfType,
  DfBrCssPcNot,
  DfBrCssPcIs,
  DfBrCssPcWhere,
  DfBrCssPcHas,
  DfBrCssPcRoot,
  DfBrCssPcEmpty,
  DfBrCssPcLink,
  DfBrCssPcVisited,
  DfBrCssPcAnyLink,
  DfBrCssPcLocalLink,
  DfBrCssPcDisabled,
  DfBrCssPcEnabled,
  DfBrCssPcChecked,
  DfBrCssPcRequired,
  DfBrCssPcOptional,
  DfBrCssPcReadOnly,
  DfBrCssPcReadWrite,
  DfBrCssPcPlaceholderShown,
  DfBrCssPcLang,
  DfBrCssPcDir,
  DfBrCssPcTarget,
  DfBrCssPcScope
} DfBrCssPseudoClass;

typedef enum {
  DfBrCssPeUnknown = 0,
  DfBrCssPeBefore,
  DfBrCssPeAfter,
  DfBrCssPeFirstLine,
  DfBrCssPeFirstLetter,
  DfBrCssPeSelection,
  DfBrCssPeMarker,
  DfBrCssPePlaceholder,
  DfBrCssPeBackdrop
} DfBrCssPseudoElement;

typedef struct DfBrCssCompound DfBrCssCompound;

typedef struct DfBrCssSimpleSel {
  DfBrCssSimpleSelKind        Kind;

  /* Type / id / class / attr name / pseudo name identifier. */
  const char                 *Name;
  usize                       NameLen;

  /* For Attr: value + op + case flag. */
  const char                 *AttrValue;
  usize                       AttrValueLen;
  DfBrCssAttrOp               AttrOp;
  b32                         AttrIgnoreCase;

  /* Pseudo-class + pseudo-element ids. */
  DfBrCssPseudoClass          Pc;
  DfBrCssPseudoElement        Pe;

  /* :nth-child(An+B) coefficients. */
  i32                         NthA;
  i32                         NthB;
  b32                         NthValid;

  /* :not(...) / :is(...) / :where(...) / :has(...) argument as a
   * nested selector list. v1 only matches simple arguments (no
   * combinators inside :not). */
  struct DfBrCssComplex      *InnerList;

  /* :lang(code) argument. */
  const char                 *LangCode;
  usize                       LangCodeLen;

  struct DfBrCssSimpleSel    *Next;   /* within compound (no combinator) */
} DfBrCssSimpleSel;

typedef enum {
  DfBrCssCombDescendant,   /* (space) */
  DfBrCssCombChild,        /* > */
  DfBrCssCombAdjacent,     /* + */
  DfBrCssCombSibling       /* ~ */
} DfBrCssCombinator;

struct DfBrCssCompound {
  DfBrCssSimpleSel        *Simples;
  DfBrCssCombinator        ComboToNext;
  struct DfBrCssCompound  *Next;
};

typedef struct DfBrCssComplex {
  DfBrCssCompound         *First;
  DfBrCssCompound         *Last;
  u32                      Specificity;  /* packed a<<20 | b<<10 | c */
  struct DfBrCssComplex   *Next;         /* next complex in selector list */
} DfBrCssComplex;

struct DfBrCssSelectorList {
  DfBrCssComplex *First;
};

/* ------------------------------------------------------------------
 *  Rules
 * -----------------------------------------------------------------*/

struct DfBrCssRule {
  DfBrCssRuleKind         Kind;

  /* Qualified rules */
  DfBrCssSelectorList    *Selectors;
  DfBrCssDeclaration     *FirstDecl;

  /* At-rules */
  const char             *AtName;
  usize                   AtNameLen;
  DfBrCssCv              *AtPrelude;
  struct DfBrCssRule     *InnerRules;

  i32                     Line;
  i32                     Column;
  struct DfBrCssRule     *Next;
};

/* ------------------------------------------------------------------
 *  Stylesheet (opaque outside this header)
 * -----------------------------------------------------------------*/

struct DfBrCssStylesheet {
  DfBrCssArena            Arena;
  DfBrCssRule            *FirstRule;
  DfBrCssDiagList         Diags;
  char                   *Source;      /* arena copy */
  usize                   SourceLen;
  DfBrCssParseOptions     Options;
};

/* ------------------------------------------------------------------
 *  Parser entry (DfBrCssParse.c)
 * -----------------------------------------------------------------*/

void DfBrCssParseStylesheet (DfBrCssStylesheet *Sheet);

/* Parse a declaration block content (body of `{ ... }`, plus style=""
 * attributes). Stops at EOF or `}`. Produces a linked list of decls. */
DfBrCssDeclaration *DfBrCssParseDeclarationBlock (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  );

/* Parse a selector list (comma-separated). Consumes until `{` / EOF. */
DfBrCssSelectorList *DfBrCssParseSelectorList (
  DfBrCssTokenizer  *T,
  DfBrCssArena      *A,
  DfBrCssDiagList   *Diags
  );

/* ------------------------------------------------------------------
 *  Matching (DfBrCssMatch.c)
 * -----------------------------------------------------------------*/

typedef struct {
  const DfBrHtmlNode *Root;        /* Document root for :root checks */
  /* Viewport size in CSS pixels, used when evaluating @media
   * (min-width/max-width/min-height/max-height/...) at cascade time.
   * 0 means "unknown" - treat every viewport-based query as matching
   * (preserves behaviour prior to @media evaluation). */
  f32                 VpWidthPx;
  f32                 VpHeightPx;
} DfBrCssMatchContext;

/* Evaluate an @media prelude against a match context. The prelude is
 * a list of composed values as produced by the CSS parser. Unknown /
 * unparseable queries are considered matching so we never drop rules
 * we failed to understand. */
b32 DfBrCssMediaQueryMatches (
  const struct DfBrCssCv    *Prelude,
  const DfBrCssMatchContext *Ctx
  );

b32 DfBrCssMatchSelectorList (
  const DfBrCssSelectorList  *SelList,
  const DfBrHtmlNode         *Elem,
  const DfBrCssMatchContext  *Ctx,
  u32                        *OutSpecificity   /* may be NULL */
  );

b32 DfBrCssMatchComplex (
  const DfBrCssComplex       *Complex,
  const DfBrHtmlNode         *Elem,
  const DfBrCssMatchContext  *Ctx
  );

/* ------------------------------------------------------------------
 *  Style context (owned arena + per-node computed styles)
 * -----------------------------------------------------------------*/

struct DfBrCssStyleContext {
  DfBrCssArena           Arena;
  DfBrHtmlDocument      *Doc;
  DfBrCssDiagList        Diags;
  f32                    RootFontSizePx;     /* for rem resolution; default 16 */
  f32                    DefaultFontSizePx;  /* initial / inherit-root font-size */
};

/* Allocate a zero-init DfBrCssComputedStyle on the style context arena,
 * pre-seeded with initial values (display = inline, color = black, ...).
 * The cascade mutates it in place. */
DfBrCssComputedStyle *DfBrCssComputedNew (DfBrCssStyleContext *Sc);

/* Copy `Parent` -> `Child` for inherited properties only. Used by the
 * cascade to implement inheritance before applying per-element decls. */
void DfBrCssComputedInherit (DfBrCssComputedStyle *Child,
                             const DfBrCssComputedStyle *Parent);

/* Apply one decl to a computed style. Handles shorthand expansion,
 * unit parsing, and keyword resolution. */
void DfBrCssApplyDeclaration (DfBrCssStyleContext       *Sc,
                              DfBrCssComputedStyle      *Cs,
                              const DfBrCssComputedStyle *Parent,
                              const DfBrCssDeclaration   *Decl);

/* ------------------------------------------------------------------
 *  CV helpers (DfBrCssValue.c)
 * -----------------------------------------------------------------*/

/** True if `Cv` is a <length> (dimension with a length unit or
    a bare number 0). Resolves to (value, unit) in Out. **/
b32 DfBrCssCvAsLength (const DfBrCssCv *Cv, DfBrCssLength *Out);

/** True if `Cv` is the keyword `auto`. **/
b32 DfBrCssCvIsAuto (const DfBrCssCv *Cv);

/** True if the leading run of Cv parses as a <color>. On success
    writes into Out and writes the CV AFTER the color (or NULL) to
    *OutTail (may be NULL if caller does not care). **/
b32 DfBrCssCvAsColor (const DfBrCssCv *Cv, DfBrCssColor *Out, const DfBrCssCv **OutTail);

/* ------------------------------------------------------------------
 *  Property table helpers
 * -----------------------------------------------------------------*/

/* Called by the cascade: returns DFBR_TRUE if this property, given
 * its computed-style slot, should be inherited from the parent when
 * the author sets `inherit`/nothing on this element. */
b32 DfBrCssPropInheritsByDefault (DfBrCssProp P);

/* ------------------------------------------------------------------
 *  Serialization helpers
 * -----------------------------------------------------------------*/

void DfBrCssDumpCv           (const DfBrCssCv *Cv, char **Buf, usize *Cap, usize *Len);
void DfBrCssDumpSelectorList (const DfBrCssSelectorList *SL, char **Buf, usize *Cap, usize *Len);

/* ------------------------------------------------------------------
 *  Misc helpers
 * -----------------------------------------------------------------*/

/* ASCII-case-insensitive compare for N bytes vs a NUL-terminated lit. */
b32  DfBrCssAsciiEqZ (const char *A, usize ALen, const char *LitZ);

#endif /* DFBR_CSS_INTERNAL_H_ */
