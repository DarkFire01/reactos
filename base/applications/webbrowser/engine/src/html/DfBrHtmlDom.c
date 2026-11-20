/** @file
  DfBrHtmlDom.c - arena, diagnostics list, DOM construction, and
                  public traversal helpers for DfBrHtml.
**/

#include "DfBrHtmlInternal.h"
#include "DfBrCanvas.h"

#include <stdlib.h>
#include <string.h>

#define DFBR_HTML_ARENA_MIN_CHUNK  (64 * 1024)

/* ------------------------------------------------------------------
 *  Arena
 * -----------------------------------------------------------------*/

static DfBrHtmlArenaChunk *
NewChunk (
  usize  MinPayload
  )
{
  DfBrHtmlArenaChunk  *C;
  usize                Cap;

  Cap = MinPayload;
  if (Cap < DFBR_HTML_ARENA_MIN_CHUNK) {
    Cap = DFBR_HTML_ARENA_MIN_CHUNK;
  }

  C = (DfBrHtmlArenaChunk *)malloc (sizeof (DfBrHtmlArenaChunk) + Cap - 1);
  if (C == DFBR_NULL) {
    return DFBR_NULL;
  }
  C->Next = DFBR_NULL;
  C->Used = 0;
  C->Cap  = Cap;
  return C;
}

void *
DfBrHtmlArenaAlloc (
  DfBrHtmlArena  *A,
  usize           Size
  )
{
  DfBrHtmlArenaChunk  *C;
  usize                Aligned;
  void                *P;

  /* 8-byte align every allocation. Matches worst-case alignment on
   * 64-bit targets (double, pointer, uint64_t). We could query
   * _Alignof(max_align_t) but C99 doesn't expose it portably. */
  Aligned = (Size + 7U) & ~(usize)7U;

  if ((A->Current == DFBR_NULL) ||
      ((A->Current->Used + Aligned) > A->Current->Cap)) {
    C = NewChunk (Aligned);
    if (C == DFBR_NULL) {
      return DFBR_NULL;
    }
    if (A->First == DFBR_NULL) {
      A->First = C;
    } else {
      A->Current->Next = C;
    }
    A->Current = C;
  }

  P = &A->Current->Data[A->Current->Used];
  A->Current->Used += Aligned;
  A->TotalBytes    += Aligned;
  return P;
}

void *
DfBrHtmlArenaZero (
  DfBrHtmlArena  *A,
  usize           Size
  )
{
  void  *P = DfBrHtmlArenaAlloc (A, Size);
  if (P != DFBR_NULL) {
    memset (P, 0, Size);
  }
  return P;
}

char *
DfBrHtmlArenaStrDup (
  DfBrHtmlArena  *A,
  const char     *S,
  usize           Len
  )
{
  char  *Out = (char *)DfBrHtmlArenaAlloc (A, Len + 1);
  if (Out == DFBR_NULL) {
    return DFBR_NULL;
  }
  if (Len > 0) {
    memcpy (Out, S, Len);
  }
  Out[Len] = '\0';
  return Out;
}

char *
DfBrHtmlArenaStrDupLower (
  DfBrHtmlArena  *A,
  const char     *S,
  usize           Len
  )
{
  char   *Out = (char *)DfBrHtmlArenaAlloc (A, Len + 1);
  usize   i;
  char    C;

  if (Out == DFBR_NULL) {
    return DFBR_NULL;
  }
  for (i = 0; i < Len; i++) {
    C = S[i];
    if ((C >= 'A') && (C <= 'Z')) {
      C = (char)(C - 'A' + 'a');
    }
    Out[i] = C;
  }
  Out[Len] = '\0';
  return Out;
}

void
DfBrHtmlArenaDispose (
  DfBrHtmlArena  *A
  )
{
  DfBrHtmlArenaChunk  *C;
  DfBrHtmlArenaChunk  *N;

  for (C = A->First; C != DFBR_NULL; C = N) {
    N = C->Next;
    free (C);
  }
  A->First      = DFBR_NULL;
  A->Current    = DFBR_NULL;
  A->TotalBytes = 0;
}

/* ------------------------------------------------------------------
 *  Diagnostics
 * -----------------------------------------------------------------*/

void
DfBrHtmlDiagEmit (
  DfBrHtmlDiagList  *List,
  DfBrHtmlArena     *A,
  i32                Line,
  i32                Column,
  const char        *Message
  )
{
  usize               NewCap;
  DfBrHtmlDiagEntry  *New;

  if (List->Count == List->Cap) {
    /* Grow-by-doubling. Old storage leaks into the arena until the
     * whole doc is freed - that's fine, diagnostic vectors are tiny
     * and the entire point of the arena is to defer per-allocation
     * bookkeeping. */
    NewCap = (List->Cap == 0) ? 8 : (List->Cap * 2);
    New    = (DfBrHtmlDiagEntry *)DfBrHtmlArenaAlloc (
               A, NewCap * sizeof (DfBrHtmlDiagEntry));
    if (New == DFBR_NULL) {
      return;
    }
    if (List->Entries != DFBR_NULL) {
      memcpy (New, List->Entries,
              List->Count * sizeof (DfBrHtmlDiagEntry));
    }
    List->Entries = New;
    List->Cap     = NewCap;
  }

  List->Entries[List->Count].Line    = Line;
  List->Entries[List->Count].Column  = Column;
  List->Entries[List->Count].Message =
    DfBrHtmlArenaStrDup (A, Message, strlen (Message));
  List->Count++;
}

/* ------------------------------------------------------------------
 *  DOM construction helpers
 * -----------------------------------------------------------------*/

DfBrHtmlNode *
DfBrHtmlNewElement (
  DfBrHtmlDocument  *Doc,
  const char        *Name,
  usize              NameLen,
  DfBrHtmlTag        Tag
  )
{
  DfBrHtmlNode  *N;

  N = (DfBrHtmlNode *)DfBrHtmlArenaZero (&Doc->Arena, sizeof (DfBrHtmlNode));
  if (N == DFBR_NULL) {
    return DFBR_NULL;
  }
  N->Kind    = DFBR_HTML_NODE_ELEMENT;
  N->Tag     = Tag;
  /* Store lowercased - this is what CSS selectors, scripts, and our
   * own tag-matching all expect. If a caller ever needs the original
   * case, the source buffer is still held on the document. */
  N->Name    = DfBrHtmlArenaStrDupLower (&Doc->Arena, Name, NameLen);
  N->NameLen = NameLen;
  return N;
}

DfBrHtmlNode *
DfBrHtmlNewText (
  DfBrHtmlDocument  *Doc,
  const char        *Text,
  usize              TextLen
  )
{
  DfBrHtmlNode  *N;

  N = (DfBrHtmlNode *)DfBrHtmlArenaZero (&Doc->Arena, sizeof (DfBrHtmlNode));
  if (N == DFBR_NULL) {
    return DFBR_NULL;
  }
  N->Kind    = DFBR_HTML_NODE_TEXT;
  N->Text    = DfBrHtmlArenaStrDup (&Doc->Arena, Text, TextLen);
  N->TextLen = TextLen;
  return N;
}

DfBrHtmlNode *
DfBrHtmlNewComment (
  DfBrHtmlDocument  *Doc,
  const char        *Text,
  usize              TextLen
  )
{
  DfBrHtmlNode  *N;

  N = (DfBrHtmlNode *)DfBrHtmlArenaZero (&Doc->Arena, sizeof (DfBrHtmlNode));
  if (N == DFBR_NULL) {
    return DFBR_NULL;
  }
  N->Kind    = DFBR_HTML_NODE_COMMENT;
  N->Text    = DfBrHtmlArenaStrDup (&Doc->Arena, Text, TextLen);
  N->TextLen = TextLen;
  return N;
}

DfBrHtmlNode *
DfBrHtmlNewDoctype (
  DfBrHtmlDocument  *Doc,
  const char        *Text,
  usize              TextLen
  )
{
  DfBrHtmlNode  *N;

  N = (DfBrHtmlNode *)DfBrHtmlArenaZero (&Doc->Arena, sizeof (DfBrHtmlNode));
  if (N == DFBR_NULL) {
    return DFBR_NULL;
  }
  N->Kind    = DFBR_HTML_NODE_DOCTYPE;
  N->Text    = DfBrHtmlArenaStrDup (&Doc->Arena, Text, TextLen);
  N->TextLen = TextLen;
  return N;
}

DfBrHtmlAttr *
DfBrHtmlAppendAttr (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Elem,
  const char        *Name,
  usize              NameLen,
  const char        *Value,
  usize              ValueLen
  )
{
  DfBrHtmlAttr   *A;
  DfBrHtmlAttr  **Slot;

  A = (DfBrHtmlAttr *)DfBrHtmlArenaZero (&Doc->Arena, sizeof (DfBrHtmlAttr));
  if (A == DFBR_NULL) {
    return DFBR_NULL;
  }

  A->Name    = DfBrHtmlArenaStrDupLower (&Doc->Arena, Name, NameLen);
  A->NameLen = NameLen;

  if (Value != DFBR_NULL) {
    A->Value    = DfBrHtmlArenaStrDup (&Doc->Arena, Value, ValueLen);
    A->ValueLen = ValueLen;
    A->HasValue = DFBR_TRUE;
  } else {
    A->Value    = DfBrHtmlArenaStrDup (&Doc->Arena, "", 0);
    A->ValueLen = 0;
    A->HasValue = DFBR_FALSE;
  }

  /* Append at the tail - source order matters for CSS attribute
   * selectors and for scripts doing attribute iteration. Linear walk
   * is fine; attribute counts per element rarely exceed a dozen. */
  for (Slot = &Elem->FirstAttr;
       *Slot != DFBR_NULL;
       Slot  = &(*Slot)->Next) {
    /* no body - just walk to tail */
  }
  *Slot = A;
  return A;
}

void
DfBrHtmlAppendChild (
  DfBrHtmlNode  *Parent,
  DfBrHtmlNode  *Child
  )
{
  Child->Parent      = Parent;
  Child->PrevSibling = Parent->LastChild;
  Child->NextSibling = DFBR_NULL;
  if (Parent->LastChild != DFBR_NULL) {
    Parent->LastChild->NextSibling = Child;
  } else {
    Parent->FirstChild = Child;
  }
  Parent->LastChild = Child;
}

void
DfBrHtmlInsertBefore (
  DfBrHtmlNode  *Parent,
  DfBrHtmlNode  *NewChild,
  DfBrHtmlNode  *Ref
  )
{
  if (Ref == DFBR_NULL) {
    DfBrHtmlAppendChild (Parent, NewChild);
    return;
  }
  NewChild->Parent      = Parent;
  NewChild->PrevSibling = Ref->PrevSibling;
  NewChild->NextSibling = Ref;
  if (Ref->PrevSibling != DFBR_NULL) {
    Ref->PrevSibling->NextSibling = NewChild;
  } else {
    Parent->FirstChild = NewChild;
  }
  Ref->PrevSibling = NewChild;
}

void
DfBrHtmlDetachChild (
  DfBrHtmlNode  *Child
  )
{
  DfBrHtmlNode  *Parent = Child->Parent;
  if (Parent == DFBR_NULL) {
    return;
  }
  if (Child->PrevSibling != DFBR_NULL) {
    Child->PrevSibling->NextSibling = Child->NextSibling;
  } else {
    Parent->FirstChild = Child->NextSibling;
  }
  if (Child->NextSibling != DFBR_NULL) {
    Child->NextSibling->PrevSibling = Child->PrevSibling;
  } else {
    Parent->LastChild = Child->PrevSibling;
  }
  Child->Parent      = DFBR_NULL;
  Child->PrevSibling = DFBR_NULL;
  Child->NextSibling = DFBR_NULL;
}

/* ------------------------------------------------------------------
 *  Public traversal helpers
 * -----------------------------------------------------------------*/

static int
AsciiEqIgnoreCaseZ (
  const char  *A,
  usize        ALen,
  const char  *B   /* NUL-terminated */
  )
{
  usize  i = 0;
  char   x;
  char   y;

  while ((i < ALen) && (B[i] != '\0')) {
    x = A[i];
    y = B[i];
    if ((x >= 'A') && (x <= 'Z')) {
      x = (char)(x - 'A' + 'a');
    }
    if ((y >= 'A') && (y <= 'Z')) {
      y = (char)(y - 'A' + 'a');
    }
    if (x != y) {
      return 0;
    }
    i++;
  }
  return ((i == ALen) && (B[i] == '\0')) ? 1 : 0;
}

const DfBrHtmlNode *
DfBrHtmlDocumentRoot (
  const DfBrHtmlDocument  *Doc
  )
{
  return (Doc != DFBR_NULL) ? Doc->Root : DFBR_NULL;
}

static const DfBrHtmlNode *
FindChildByTag (
  const DfBrHtmlNode  *P,
  DfBrHtmlTag          Tag
  )
{
  const DfBrHtmlNode  *C;

  if (P == DFBR_NULL) {
    return DFBR_NULL;
  }
  for (C = P->FirstChild; C != DFBR_NULL; C = C->NextSibling) {
    if ((C->Kind == DFBR_HTML_NODE_ELEMENT) && (C->Tag == Tag)) {
      return C;
    }
  }
  return DFBR_NULL;
}

const DfBrHtmlNode *
DfBrHtmlDocumentHtml (
  const DfBrHtmlDocument  *Doc
  )
{
  return FindChildByTag (DfBrHtmlDocumentRoot (Doc), DFBR_HTML_TAG_HTML);
}

const DfBrHtmlNode *
DfBrHtmlDocumentHead (
  const DfBrHtmlDocument  *Doc
  )
{
  return FindChildByTag (DfBrHtmlDocumentHtml (Doc), DFBR_HTML_TAG_HEAD);
}

const DfBrHtmlNode *
DfBrHtmlDocumentBody (
  const DfBrHtmlDocument  *Doc
  )
{
  return FindChildByTag (DfBrHtmlDocumentHtml (Doc), DFBR_HTML_TAG_BODY);
}

void
DfBrHtmlDocumentDestroy (
  DfBrHtmlDocument  *Doc
  )
{
  if (Doc == DFBR_NULL) {
    return;
  }
  /* Drop any canvas backings hanging off nodes before the arena goes
   * away - pixels are malloc'd, not arena-owned, so the list walk is
   * the only thing that frees them. DfBrCanvasDestroyAll is a no-op
   * when the list is empty (i.e. the document never touched canvas). */
  DfBrCanvasDestroyAll (Doc);
  DfBrHtmlArenaDispose (&Doc->Arena);
  free (Doc);
}

usize
DfBrHtmlDocumentDiagCount (
  const DfBrHtmlDocument  *Doc
  )
{
  return (Doc != DFBR_NULL) ? Doc->Diags.Count : 0;
}

const DfBrHtmlDiagEntry *
DfBrHtmlDocumentDiag (
  const DfBrHtmlDocument  *Doc,
  usize                    Index
  )
{
  if ((Doc == DFBR_NULL) || (Index >= Doc->Diags.Count)) {
    return DFBR_NULL;
  }
  return &Doc->Diags.Entries[Index];
}

const DfBrHtmlAttr *
DfBrHtmlNodeGetAttr (
  const DfBrHtmlNode  *N,
  const char          *Name
  )
{
  const DfBrHtmlAttr  *A;

  if ((N == DFBR_NULL) || (Name == DFBR_NULL)) {
    return DFBR_NULL;
  }
  for (A = N->FirstAttr; A != DFBR_NULL; A = A->Next) {
    if (AsciiEqIgnoreCaseZ (A->Name, A->NameLen, Name)) {
      return A;
    }
  }
  return DFBR_NULL;
}

const char *
DfBrHtmlNodeGetAttrValue (
  const DfBrHtmlNode  *N,
  const char          *Name
  )
{
  const DfBrHtmlAttr  *A = DfBrHtmlNodeGetAttr (N, Name);
  return (A != DFBR_NULL) ? A->Value : DFBR_NULL;
}

const DfBrHtmlNode *
DfBrHtmlNodeNextPreorder (
  const DfBrHtmlNode  *Root,
  const DfBrHtmlNode  *Cur
  )
{
  const DfBrHtmlNode  *N;

  if (Cur == DFBR_NULL) {
    return Root;
  }
  if (Cur->FirstChild != DFBR_NULL) {
    return Cur->FirstChild;
  }
  N = Cur;
  while ((N != DFBR_NULL) && (N != Root)) {
    if (N->NextSibling != DFBR_NULL) {
      return N->NextSibling;
    }
    N = N->Parent;
  }
  return DFBR_NULL;
}

const DfBrHtmlNode *
DfBrHtmlNodeFindById (
  const DfBrHtmlNode  *Root,
  const char          *Id
  )
{
  const DfBrHtmlNode  *N;
  const char          *V;

  if ((Root == DFBR_NULL) || (Id == DFBR_NULL)) {
    return DFBR_NULL;
  }
  for (N = Root; N != DFBR_NULL; N = DfBrHtmlNodeNextPreorder (Root, N)) {
    if (N->Kind == DFBR_HTML_NODE_ELEMENT) {
      V = DfBrHtmlNodeGetAttrValue (N, "id");
      if ((V != DFBR_NULL) && (strcmp (V, Id) == 0)) {
        return N;
      }
    }
  }
  return DFBR_NULL;
}

const DfBrHtmlNode *
DfBrHtmlNodeFindFirstByTag (
  const DfBrHtmlNode  *Root,
  DfBrHtmlTag          Tag
  )
{
  const DfBrHtmlNode  *N;

  if (Root == DFBR_NULL) {
    return DFBR_NULL;
  }
  for (N = Root; N != DFBR_NULL; N = DfBrHtmlNodeNextPreorder (Root, N)) {
    if ((N->Kind == DFBR_HTML_NODE_ELEMENT) && (N->Tag == Tag)) {
      return N;
    }
  }
  return DFBR_NULL;
}

/* ==================================================================
 *
 *  Public mutation API
 *
 *  Used by the JS bindings (and anything else that wants to change
 *  the tree post-parse). Every mutation bumps Doc->Generation so a
 *  host can decide whether to re-run the cascade + layout + paint
 *  pipeline without diffing the tree.
 *
 * ================================================================*/

static usize
StrLenOrZero (
  const char  *S,
  usize        Len
  )
{
  return (Len != 0) ? Len : ((S != DFBR_NULL) ? strlen (S) : 0);
}

DfBrHtmlNode *
DfBrHtmlDocumentCreateElement (
  DfBrHtmlDocument  *Doc,
  const char        *Name,
  usize              NameLen
  )
{
  DfBrHtmlTag   Tag;
  usize         Len;

  if ((Doc == DFBR_NULL) || (Name == DFBR_NULL)) {
    return DFBR_NULL;
  }
  Len = StrLenOrZero (Name, NameLen);
  Tag = DfBrHtmlTagFromName (Name, Len);
  /* No Generation bump here - the node is free-floating until it is
   * attached. The attach is what matters for the host. */
  return DfBrHtmlNewElement (Doc, Name, Len, Tag);
}

DfBrHtmlNode *
DfBrHtmlDocumentCreateTextNode (
  DfBrHtmlDocument  *Doc,
  const char        *Text,
  usize              TextLen
  )
{
  usize  Len;

  if (Doc == DFBR_NULL) {
    return DFBR_NULL;
  }
  Len = StrLenOrZero (Text, TextLen);
  return DfBrHtmlNewText (Doc, (Text != DFBR_NULL) ? Text : "", Len);
}

void
DfBrHtmlDocumentAppendChild (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Parent,
  DfBrHtmlNode      *Child
  )
{
  if ((Doc == DFBR_NULL) || (Parent == DFBR_NULL) || (Child == DFBR_NULL)) {
    return;
  }
  if (Child->Parent != DFBR_NULL) {
    DfBrHtmlDetachChild (Child);
  }
  DfBrHtmlAppendChild (Parent, Child);
  Doc->Generation++;
}

void
DfBrHtmlDocumentInsertBefore (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Parent,
  DfBrHtmlNode      *NewChild,
  DfBrHtmlNode      *Ref
  )
{
  if ((Doc == DFBR_NULL) || (Parent == DFBR_NULL) || (NewChild == DFBR_NULL)) {
    return;
  }
  if ((Ref != DFBR_NULL) && (Ref->Parent != Parent)) {
    /* WHATWG: raises NotFoundError. We just no-op to keep the JS
     * wrapper layer simple; the caller can detect missing effects
     * via the generation counter. */
    return;
  }
  if (NewChild->Parent != DFBR_NULL) {
    DfBrHtmlDetachChild (NewChild);
  }
  DfBrHtmlInsertBefore (Parent, NewChild, Ref);
  Doc->Generation++;
}

void
DfBrHtmlDocumentRemoveChild (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Child
  )
{
  if ((Doc == DFBR_NULL) || (Child == DFBR_NULL) || (Child->Parent == DFBR_NULL)) {
    return;
  }
  DfBrHtmlDetachChild (Child);
  Doc->Generation++;
}

void
DfBrHtmlDocumentSetAttribute (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Elem,
  const char        *Name,
  usize              NameLen,
  const char        *Value,
  usize              ValueLen
  )
{
  DfBrHtmlAttr  *A;
  usize          NLen;
  usize          VLen;

  if ((Doc == DFBR_NULL) || (Elem == DFBR_NULL) || (Name == DFBR_NULL)) {
    return;
  }
  if (Elem->Kind != DFBR_HTML_NODE_ELEMENT) {
    return;
  }
  NLen = StrLenOrZero (Name, NameLen);
  VLen = StrLenOrZero (Value, ValueLen);
  if (NLen == 0) {
    return;
  }

  /* Walk the attribute list directly - DfBrHtmlNodeGetAttr returns a
   * const pointer and we need to mutate. */
  for (A = Elem->FirstAttr; A != DFBR_NULL; A = A->Next) {
    if (AsciiEqIgnoreCaseZ (A->Name, A->NameLen, Name)) {
      A->Value    = DfBrHtmlArenaStrDup (&Doc->Arena,
                                         (Value != DFBR_NULL) ? Value : "",
                                         VLen);
      A->ValueLen = VLen;
      A->HasValue = DFBR_TRUE;
      Doc->Generation++;
      return;
    }
  }

  DfBrHtmlAppendAttr (Doc, Elem, Name, NLen,
                      (Value != DFBR_NULL) ? Value : "", VLen);
  Doc->Generation++;
}

void
DfBrHtmlDocumentRemoveAttribute (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Elem,
  const char        *Name,
  usize              NameLen
  )
{
  DfBrHtmlAttr  *A;
  DfBrHtmlAttr  *Prev = DFBR_NULL;

  if ((Doc == DFBR_NULL) || (Elem == DFBR_NULL) || (Name == DFBR_NULL)) {
    return;
  }
  if (Elem->Kind != DFBR_HTML_NODE_ELEMENT) {
    return;
  }
  NameLen = StrLenOrZero (Name, NameLen);
  for (A = Elem->FirstAttr; A != DFBR_NULL; A = A->Next) {
    if (AsciiEqIgnoreCaseZ (A->Name, A->NameLen, Name)) {
      if (Prev == DFBR_NULL) {
        Elem->FirstAttr = A->Next;
      } else {
        Prev->Next = A->Next;
      }
      Doc->Generation++;
      return;
    }
    Prev = A;
  }
  (void)NameLen;
}

void
DfBrHtmlDocumentSetTextContent (
  DfBrHtmlDocument  *Doc,
  DfBrHtmlNode      *Elem,
  const char        *Text,
  usize              TextLen
  )
{
  DfBrHtmlNode  *Child;
  DfBrHtmlNode  *Next;
  DfBrHtmlNode  *NewText;
  usize          Len;

  if ((Doc == DFBR_NULL) || (Elem == DFBR_NULL)) {
    return;
  }
  /* Drop every existing child. Arena-owned - we don't free the
   * memory, it just becomes unreachable. */
  Child = Elem->FirstChild;
  while (Child != DFBR_NULL) {
    Next = Child->NextSibling;
    Child->Parent      = DFBR_NULL;
    Child->PrevSibling = DFBR_NULL;
    Child->NextSibling = DFBR_NULL;
    Child = Next;
  }
  Elem->FirstChild = DFBR_NULL;
  Elem->LastChild  = DFBR_NULL;

  Len = StrLenOrZero (Text, TextLen);
  if (Len > 0) {
    NewText = DfBrHtmlNewText (Doc, Text, Len);
    if (NewText != DFBR_NULL) {
      DfBrHtmlAppendChild (Elem, NewText);
    }
  }
  Doc->Generation++;
}

u64
DfBrHtmlDocumentGeneration (
  const DfBrHtmlDocument  *Doc
  )
{
  return (Doc != DFBR_NULL) ? Doc->Generation : 0;
}
