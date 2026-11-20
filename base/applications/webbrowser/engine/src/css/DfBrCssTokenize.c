/** @file
  DfBrCssTokenize.c - CSS Syntax Module Level 3 tokenizer.

  This is a straight transcription of https://www.w3.org/TR/css-syntax-3/
  Section 4. Every major production below has a comment pointing to
  the spec section name so a reader can grep the spec into this file.

  Input is UTF-8 bytes. The 3-byte BOM is skipped up front. Null
  codepoints are replaced with U+FFFD at read time per the preprocess
  step. Line / column counters are 1-based and tracked byte-by-byte,
  treating \r, \n, and \r\n all as a single line break.
**/

#include "DfBrCssInternal.h"

#include <string.h>

/* UTF-8 replacement character emitted in place of NULs. */
static const char kReplacementUtf8[3] = { (char)0xEF, (char)0xBF, (char)0xBD };

/* ------------------------------------------------------------------
 *  Low-level byte I/O
 * -----------------------------------------------------------------*/

static int PeekByte (const DfBrCssTokenizer *T, usize Ahead) {
  if ((T->Pos + Ahead) >= T->Len) return -1;
  return (unsigned char)T->Src[T->Pos + Ahead];
}

static int NextByte (DfBrCssTokenizer *T) {
  int c;
  if (T->Pos >= T->Len) return -1;
  c = (unsigned char)T->Src[T->Pos++];

  /* Track source locations. Treat \r\n as one logical line break; we
   * see \r and bump Line/Column, then on the following \n we skip the
   * column bump. */
  if (c == '\n') {
    T->Line++;
    T->Column = 1;
  } else if (c == '\r') {
    T->Line++;
    T->Column = 1;
    if (PeekByte (T, 0) == '\n') {
      /* Swallow the LF so the caller sees a single newline. */
      T->Pos++;
    }
    c = '\n';
  } else {
    T->Column++;
  }
  return c;
}

static void UngetByte (DfBrCssTokenizer *T) {
  /* Cheap single-byte ungets only; we never cross a \r\n pair so
   * callers just need to back one column. Callers that peek first
   * via PeekByte and don't commit don't even need this. */
  if (T->Pos == 0) return;
  T->Pos--;
  if (T->Column > 1) T->Column--;
}

/* ------------------------------------------------------------------
 *  Tiny dynamic byte buffer (arena-backed, exponential growth)
 *
 *  Used to accumulate ident / string / url bodies after escapes have
 *  been processed. At finish time we move/copy into the arena and
 *  return a NUL-terminated slice.
 * -----------------------------------------------------------------*/

typedef struct {
  DfBrCssArena *Arena;
  char         *Data;
  usize         Len;
  usize         Cap;
} Buf;

static void BufInit (Buf *B, DfBrCssArena *A) {
  B->Arena = A;
  B->Data  = DFBR_NULL;
  B->Len   = 0;
  B->Cap   = 0;
}

static void BufEnsure (Buf *B, usize AddBytes) {
  usize  NewCap;
  char  *New;

  if ((B->Len + AddBytes + 1) <= B->Cap) return;

  NewCap = (B->Cap == 0) ? 32 : B->Cap;
  while ((B->Len + AddBytes + 1) > NewCap) { NewCap *= 2; }

  New = (char *)DfBrCssArenaAlloc (B->Arena, NewCap);
  if (New == DFBR_NULL) return;
  if (B->Len > 0) memcpy (New, B->Data, B->Len);
  B->Data = New;
  B->Cap  = NewCap;
}

static void BufAppendByte (Buf *B, int C) {
  BufEnsure (B, 1);
  if (B->Data == DFBR_NULL) return;
  B->Data[B->Len++] = (char)C;
}

static void BufAppendBytes (Buf *B, const char *S, usize N) {
  BufEnsure (B, N);
  if (B->Data == DFBR_NULL) return;
  if (N > 0) memcpy (B->Data + B->Len, S, N);
  B->Len += N;
}

/* Append a Unicode codepoint as UTF-8. */
static void BufAppendCodepoint (Buf *B, u32 Cp) {
  if (Cp < 0x80) {
    BufAppendByte (B, (int)Cp);
  } else if (Cp < 0x800) {
    BufAppendByte (B, 0xC0 | ((int)(Cp >> 6)   & 0x1F));
    BufAppendByte (B, 0x80 | ((int) Cp         & 0x3F));
  } else if (Cp < 0x10000) {
    BufAppendByte (B, 0xE0 | ((int)(Cp >> 12)  & 0x0F));
    BufAppendByte (B, 0x80 | ((int)(Cp >>  6)  & 0x3F));
    BufAppendByte (B, 0x80 | ((int) Cp         & 0x3F));
  } else if (Cp < 0x110000) {
    BufAppendByte (B, 0xF0 | ((int)(Cp >> 18)  & 0x07));
    BufAppendByte (B, 0x80 | ((int)(Cp >> 12)  & 0x3F));
    BufAppendByte (B, 0x80 | ((int)(Cp >>  6)  & 0x3F));
    BufAppendByte (B, 0x80 | ((int) Cp         & 0x3F));
  } else {
    BufAppendBytes (B, kReplacementUtf8, sizeof (kReplacementUtf8));
  }
}

/* Finish the buffer: terminate + hand the pointer out. */
static void BufFinish (Buf *B, const char **OutStr, usize *OutLen) {
  if (B->Data == DFBR_NULL) {
    BufEnsure (B, 0);
    if (B->Data == DFBR_NULL) { *OutStr = DFBR_NULL; *OutLen = 0; return; }
  }
  B->Data[B->Len] = '\0';
  *OutStr = B->Data;
  *OutLen = B->Len;
}

/* ------------------------------------------------------------------
 *  Character predicates (spec definitions)
 * -----------------------------------------------------------------*/

static b32 IsWhitespace (int C) {
  return (C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f')
         ? DFBR_TRUE : DFBR_FALSE;
}

static b32 IsDigit (int C) {
  return (C >= '0' && C <= '9') ? DFBR_TRUE : DFBR_FALSE;
}

static b32 IsHexDigit (int C) {
  return ( (C >= '0' && C <= '9')
        || (C >= 'a' && C <= 'f')
        || (C >= 'A' && C <= 'F')) ? DFBR_TRUE : DFBR_FALSE;
}

static b32 IsLetter (int C) {
  return ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'))
         ? DFBR_TRUE : DFBR_FALSE;
}

/* "name start code point": letter, _, U+0080+ (all non-ASCII). */
static b32 IsNameStart (int C) {
  if (C < 0) return DFBR_FALSE;
  if (IsLetter (C)) return DFBR_TRUE;
  if (C == '_') return DFBR_TRUE;
  if ((unsigned)C >= 0x80) return DFBR_TRUE;
  return DFBR_FALSE;
}

/* "name code point": NameStart | digit | '-'. */
static b32 IsName (int C) {
  if (IsNameStart (C)) return DFBR_TRUE;
  if (IsDigit (C))     return DFBR_TRUE;
  if (C == '-')        return DFBR_TRUE;
  return DFBR_FALSE;
}

static b32 IsNonPrintable (int C) {
  if (C < 0) return DFBR_FALSE;
  if ((C >= 0x00 && C <= 0x08) || C == 0x0B
      || (C >= 0x0E && C <= 0x1F) || C == 0x7F) {
    return DFBR_TRUE;
  }
  return DFBR_FALSE;
}

/* "valid escape": `\` not followed by newline or EOF. */
static b32 TwoCpsAreValidEscape (int C1, int C2) {
  if (C1 != '\\') return DFBR_FALSE;
  if (C2 == '\n' || C2 < 0) return DFBR_FALSE;
  return DFBR_TRUE;
}

/* "starts an ident": per spec, at positions (0,1,2):
 *   - U+002D '-' and (ident-start OR '-') OR valid-escape-starting-at-1
 *   - ident-start at 0
 *   - U+005C '\' and valid-escape starting there
 */
static b32 ThreeCpsStartIdent (int C0, int C1, int C2) {
  if (C0 == '-') {
    if (IsNameStart (C1) || C1 == '-') return DFBR_TRUE;
    if (C1 == '\\' && C2 >= 0 && C2 != '\n') return DFBR_TRUE;
    return DFBR_FALSE;
  }
  if (IsNameStart (C0)) return DFBR_TRUE;
  if (C0 == '\\') return (C1 >= 0 && C1 != '\n') ? DFBR_TRUE : DFBR_FALSE;
  return DFBR_FALSE;
}

/* "starts a number" at (C0, C1, C2). */
static b32 ThreeCpsStartNumber (int C0, int C1, int C2) {
  if (C0 == '+' || C0 == '-') {
    if (IsDigit (C1)) return DFBR_TRUE;
    if (C1 == '.' && IsDigit (C2)) return DFBR_TRUE;
    return DFBR_FALSE;
  }
  if (C0 == '.' && IsDigit (C1)) return DFBR_TRUE;
  if (IsDigit (C0)) return DFBR_TRUE;
  return DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Escape consumption
 *
 *  Assumes the leading `\` has been consumed. Returns the decoded
 *  codepoint (or U+FFFD on error). If the escape was a hex sequence,
 *  consumes up to 6 hex digits plus a single trailing whitespace.
 * -----------------------------------------------------------------*/

static u32 ConsumeEscapeCp (DfBrCssTokenizer *T) {
  int   C = NextByte (T);
  u32   Cp;
  int   Hex;
  int   Digits;
  int   Next;

  if (C < 0) {
    /* parse error; return replacement char */
    return 0xFFFD;
  }
  if (IsHexDigit (C)) {
    Cp = 0;
    Hex = C;
    Digits = 0;
    for (;;) {
      if      (Hex >= '0' && Hex <= '9') Cp = (Cp << 4) + (u32)(Hex - '0');
      else if (Hex >= 'a' && Hex <= 'f') Cp = (Cp << 4) + (u32)(Hex - 'a' + 10);
      else                               Cp = (Cp << 4) + (u32)(Hex - 'A' + 10);
      Digits++;
      if (Digits >= 6) break;
      Next = PeekByte (T, 0);
      if (!IsHexDigit (Next)) break;
      Hex = NextByte (T);
    }
    /* Optional single whitespace after escape. */
    Next = PeekByte (T, 0);
    if (IsWhitespace (Next)) {
      (void)NextByte (T);
    }
    if (Cp == 0 || Cp > 0x10FFFF ||
        (Cp >= 0xD800 && Cp <= 0xDFFF)) {
      return 0xFFFD;
    }
    return Cp;
  }

  /* Non-hex: return the codepoint literally. Technically UTF-8
   * continuation bytes would be mis-decoded here; for now we treat
   * each byte as its own codepoint (fine for ASCII-only escapes). */
  return (u32)C;
}

/* ------------------------------------------------------------------
 *  Ident / name consumption
 * -----------------------------------------------------------------*/

static void ConsumeName (DfBrCssTokenizer *T, Buf *B) {
  int  C1;
  int  C2;

  for (;;) {
    C1 = PeekByte (T, 0);
    if (IsName (C1)) {
      (void)NextByte (T);
      BufAppendByte (B, C1);
      continue;
    }
    C2 = PeekByte (T, 1);
    if (C1 == '\\' && TwoCpsAreValidEscape (C1, C2)) {
      (void)NextByte (T);     /* consume the backslash */
      BufAppendCodepoint (B, ConsumeEscapeCp (T));
      continue;
    }
    break;
  }
}

/* ------------------------------------------------------------------
 *  Numeric token consumption
 * -----------------------------------------------------------------*/

static f64 ParseF64 (const char *S, usize N) {
  /* Manual scanner to avoid pulling locale into the tokenizer. We
   * already know the input matches CSS <number> shape:
   *   [+-]? (digits (\.digits?)? | \.digits) ([eE][+-]?digits)? */
  usize  i = 0;
  f64    Sign = 1.0;
  f64    Int  = 0.0;
  f64    Frac = 0.0;
  f64    FracDiv = 1.0;
  int    ExpSign = 1;
  int    Exp = 0;
  f64    Mult = 1.0;
  int    k;

  if (i < N && (S[i] == '+' || S[i] == '-')) {
    if (S[i] == '-') Sign = -1.0;
    i++;
  }
  while (i < N && S[i] >= '0' && S[i] <= '9') {
    Int = Int * 10.0 + (f64)(S[i] - '0');
    i++;
  }
  if (i < N && S[i] == '.') {
    i++;
    while (i < N && S[i] >= '0' && S[i] <= '9') {
      FracDiv *= 10.0;
      Frac = Frac + (f64)(S[i] - '0') / FracDiv;
      i++;
    }
  }
  if (i < N && (S[i] == 'e' || S[i] == 'E')) {
    i++;
    if (i < N && (S[i] == '+' || S[i] == '-')) {
      if (S[i] == '-') ExpSign = -1;
      i++;
    }
    while (i < N && S[i] >= '0' && S[i] <= '9') {
      Exp = Exp * 10 + (S[i] - '0');
      i++;
    }
    if (ExpSign < 0) {
      for (k = 0; k < Exp; k++) Mult /= 10.0;
    } else {
      for (k = 0; k < Exp; k++) Mult *= 10.0;
    }
  }
  return Sign * (Int + Frac) * Mult;
}

static void ConsumeNumber (
  DfBrCssTokenizer *T,
  DfBrCssToken     *Out
  )
{
  const char *Start = &T->Src[T->Pos];
  usize       StartPos = T->Pos;
  int         C;
  b32         IsInt = DFBR_TRUE;
  b32         HasSign = DFBR_FALSE;

  C = PeekByte (T, 0);
  if (C == '+' || C == '-') {
    HasSign = DFBR_TRUE;
    (void)NextByte (T);
  }
  while (IsDigit (PeekByte (T, 0))) {
    (void)NextByte (T);
  }
  if (PeekByte (T, 0) == '.' && IsDigit (PeekByte (T, 1))) {
    IsInt = DFBR_FALSE;
    (void)NextByte (T);   /* '.' */
    while (IsDigit (PeekByte (T, 0))) (void)NextByte (T);
  }
  /* Scientific exponent. */
  C = PeekByte (T, 0);
  if (C == 'e' || C == 'E') {
    int D1 = PeekByte (T, 1);
    int D2 = PeekByte (T, 2);
    if (IsDigit (D1)
        || ((D1 == '+' || D1 == '-') && IsDigit (D2))) {
      IsInt = DFBR_FALSE;
      (void)NextByte (T);                    /* e / E */
      if (PeekByte (T, 0) == '+' || PeekByte (T, 0) == '-') {
        (void)NextByte (T);
      }
      while (IsDigit (PeekByte (T, 0))) (void)NextByte (T);
    }
  }

  Out->Kind      = DfBrCssTokNumber;
  Out->Start     = Start;
  Out->Len       = T->Pos - StartPos;
  Out->Number    = ParseF64 (Start, Out->Len);
  Out->IsInteger = IsInt;
  Out->HasSign   = HasSign;
}

/* After a number has been consumed, check for an ident or '%' to
 * upgrade it to dimension / percentage. */
static void ConsumeNumericTail (
  DfBrCssTokenizer *T,
  DfBrCssToken     *Out
  )
{
  int  C0 = PeekByte (T, 0);
  int  C1 = PeekByte (T, 1);
  int  C2 = PeekByte (T, 2);
  Buf  B;

  if (ThreeCpsStartIdent (C0, C1, C2)) {
    BufInit (&B, T->Arena);
    ConsumeName (T, &B);
    BufFinish (&B, &Out->Unit, &Out->UnitLen);
    Out->Kind   = DfBrCssTokDimension;
    Out->UnitId = DfBrCssUnitFromName (Out->Unit, Out->UnitLen);
    return;
  }
  if (C0 == '%') {
    (void)NextByte (T);
    Out->Kind = DfBrCssTokPercentage;
    return;
  }
}

/* ------------------------------------------------------------------
 *  Strings
 * -----------------------------------------------------------------*/

static void ConsumeString (
  DfBrCssTokenizer *T,
  int               Quote,
  DfBrCssToken     *Out
  )
{
  Buf  B;
  int  C;
  int  C2;

  BufInit (&B, T->Arena);
  for (;;) {
    C = NextByte (T);
    if (C < 0 || C == Quote) break;
    if (C == '\n') {
      /* Unescaped newline = bad-string; rewind so the \n starts a
       * fresh whitespace token next call. */
      UngetByte (T);
      Out->Kind = DfBrCssTokBadString;
      DfBrCssDiagEmit (T->Diags, T->Arena, Out->Line, Out->Column,
                       "unterminated-string");
      BufFinish (&B, &Out->Str, &Out->StrLen);
      return;
    }
    if (C == '\\') {
      C2 = PeekByte (T, 0);
      if (C2 < 0) {
        /* EOF after backslash: ignore the backslash. */
        continue;
      }
      if (C2 == '\n') {
        /* Escaped newline: eat it and continue. */
        (void)NextByte (T);
        continue;
      }
      BufAppendCodepoint (&B, ConsumeEscapeCp (T));
      continue;
    }
    BufAppendByte (&B, C);
  }
  Out->Kind = DfBrCssTokString;
  BufFinish (&B, &Out->Str, &Out->StrLen);
}

/* ------------------------------------------------------------------
 *  url( ... ) - unquoted form
 * -----------------------------------------------------------------*/

static void ConsumeUnquotedUrl (
  DfBrCssTokenizer *T,
  DfBrCssToken     *Out
  )
{
  Buf  B;
  int  C;
  int  C2;

  BufInit (&B, T->Arena);

  /* Skip leading whitespace. */
  while (IsWhitespace (PeekByte (T, 0))) (void)NextByte (T);

  for (;;) {
    C = NextByte (T);
    if (C < 0) {
      Out->Kind = DfBrCssTokUrl;
      BufFinish (&B, &Out->Str, &Out->StrLen);
      DfBrCssDiagEmit (T->Diags, T->Arena, Out->Line, Out->Column,
                       "unterminated-url");
      return;
    }
    if (C == ')') {
      Out->Kind = DfBrCssTokUrl;
      BufFinish (&B, &Out->Str, &Out->StrLen);
      return;
    }
    if (IsWhitespace (C)) {
      while (IsWhitespace (PeekByte (T, 0))) (void)NextByte (T);
      C = NextByte (T);
      if (C < 0 || C == ')') {
        Out->Kind = DfBrCssTokUrl;
        BufFinish (&B, &Out->Str, &Out->StrLen);
        return;
      }
      /* bad url: consume remainder through closing paren */
      goto BadUrl;
    }
    if (C == '"' || C == '\'' || C == '(' || IsNonPrintable (C)) {
      goto BadUrl;
    }
    if (C == '\\') {
      C2 = PeekByte (T, 0);
      if (TwoCpsAreValidEscape ('\\', C2)) {
        BufAppendCodepoint (&B, ConsumeEscapeCp (T));
        continue;
      }
      goto BadUrl;
    }
    BufAppendByte (&B, C);
  }

BadUrl:
  /* Walk to a `)` or EOF, no payload. */
  for (;;) {
    C = NextByte (T);
    if (C < 0 || C == ')') break;
    if (C == '\\') {
      int N2 = PeekByte (T, 0);
      if (TwoCpsAreValidEscape ('\\', N2)) (void)ConsumeEscapeCp (T);
    }
  }
  Out->Kind = DfBrCssTokBadUrl;
  BufFinish (&B, &Out->Str, &Out->StrLen);
  DfBrCssDiagEmit (T->Diags, T->Arena, Out->Line, Out->Column, "bad-url");
}

/* ------------------------------------------------------------------
 *  Ident-like dispatch
 *
 *  Once we know a name consumption just happened, decide whether we
 *  have:
 *    - url( ... )  -> Url / BadUrl token
 *    - Ident(      -> Function token
 *    - Ident       -> plain ident
 * -----------------------------------------------------------------*/

static void ConsumeIdentLike (
  DfBrCssTokenizer *T,
  DfBrCssToken     *Out
  )
{
  Buf  B;
  int  Peek;

  BufInit (&B, T->Arena);
  ConsumeName (T, &B);
  BufFinish (&B, &Out->Str, &Out->StrLen);

  /* url(  - case-insensitive, and only if not followed by a string
   * (which is a <function-token> wrapping a <string-token>). */
  if (Out->StrLen == 3 &&
      (Out->Str[0] == 'u' || Out->Str[0] == 'U') &&
      (Out->Str[1] == 'r' || Out->Str[1] == 'R') &&
      (Out->Str[2] == 'l' || Out->Str[2] == 'L') &&
      PeekByte (T, 0) == '(') {
    (void)NextByte (T);                /* consume '(' */
    /* Skip whitespace; if first non-WS is a quote, treat as function. */
    {
      usize Save = T->Pos;
      i32   SavL = T->Line;
      i32   SavC = T->Column;
      while (IsWhitespace (PeekByte (T, 0))) (void)NextByte (T);
      Peek = PeekByte (T, 0);
      if (Peek == '"' || Peek == '\'') {
        /* Treat as function-token url( that will wrap the string. */
        T->Pos = Save;
        T->Line = SavL;
        T->Column = SavC;
        Out->Kind = DfBrCssTokFunction;
        return;
      }
    }
    ConsumeUnquotedUrl (T, Out);
    return;
  }
  if (PeekByte (T, 0) == '(') {
    (void)NextByte (T);
    Out->Kind = DfBrCssTokFunction;
    return;
  }
  Out->Kind = DfBrCssTokIdent;
}

/* ------------------------------------------------------------------
 *  Comment skip
 *
 *  Returns DFBR_TRUE if a CSS comment was consumed. Does not
 *  emit a token. Unterminated comments trigger a diagnostic.
 * -----------------------------------------------------------------*/

static b32 MaybeSkipComment (DfBrCssTokenizer *T) {
  i32  StartLine;
  i32  StartCol;
  int  c;

  if (PeekByte (T, 0) != '/' || PeekByte (T, 1) != '*') return DFBR_FALSE;

  StartLine = T->Line;
  StartCol  = T->Column;
  (void)NextByte (T);      /* '/' */
  (void)NextByte (T);      /* '*' */

  for (;;) {
    c = NextByte (T);
    if (c < 0) {
      DfBrCssDiagEmit (T->Diags, T->Arena, StartLine, StartCol,
                       "unterminated-comment");
      return DFBR_TRUE;
    }
    if (c == '*' && PeekByte (T, 0) == '/') {
      (void)NextByte (T);
      return DFBR_TRUE;
    }
  }
}

/* ------------------------------------------------------------------
 *  Whitespace run
 * -----------------------------------------------------------------*/

static void ConsumeWhitespace (DfBrCssTokenizer *T, DfBrCssToken *Out) {
  Out->Kind = DfBrCssTokWhitespace;
  while (IsWhitespace (PeekByte (T, 0))) (void)NextByte (T);
}

/* ------------------------------------------------------------------
 *  Public: init + next-token
 * -----------------------------------------------------------------*/

void
DfBrCssTokInit (
  DfBrCssTokenizer  *T,
  const char        *Src,
  usize              Len,
  DfBrCssArena      *Arena,
  DfBrCssDiagList   *Diags
  )
{
  T->Src    = Src;
  T->Len    = Len;
  T->Pos    = 0;
  T->Line   = 1;
  T->Column = 1;
  T->Arena  = Arena;
  T->Diags  = Diags;

  /* Skip BOM. */
  if (Len >= 3 &&
      (unsigned char)Src[0] == 0xEF &&
      (unsigned char)Src[1] == 0xBB &&
      (unsigned char)Src[2] == 0xBF) {
    T->Pos = 3;
  }
}

void
DfBrCssTokNext (
  DfBrCssTokenizer  *T,
  DfBrCssToken      *Out
  )
{
  int  C;
  int  C1;
  int  C2;
  i32  StartLine;
  i32  StartCol;
  usize StartPos;

  memset (Out, 0, sizeof (*Out));

  /* Strip comments transparently. */
  while (MaybeSkipComment (T)) { /* loop */ }

  StartPos  = T->Pos;
  StartLine = T->Line;
  StartCol  = T->Column;

  Out->Start  = &T->Src[StartPos];
  Out->Line   = StartLine;
  Out->Column = StartCol;

  C = PeekByte (T, 0);
  if (C < 0) {
    Out->Kind = DfBrCssTokEof;
    return;
  }

  if (IsWhitespace (C)) {
    ConsumeWhitespace (T, Out);
    Out->Len = T->Pos - StartPos;
    return;
  }

  switch (C) {
    case '"':
    case '\'':
      (void)NextByte (T);
      ConsumeString (T, C, Out);
      Out->Len = T->Pos - StartPos;
      return;

    case '#': {
      (void)NextByte (T);
      C1 = PeekByte (T, 0);
      C2 = PeekByte (T, 1);
      if (IsName (C1) ||
          (C1 == '\\' && C2 >= 0 && C2 != '\n')) {
        Buf B;
        int C0 = C1;
        int CA = C2;
        int CB = PeekByte (T, 2);
        BufInit (&B, T->Arena);
        ConsumeName (T, &B);
        BufFinish (&B, &Out->Str, &Out->StrLen);
        Out->Kind     = DfBrCssTokHash;
        Out->HashFlag = ThreeCpsStartIdent (C0, CA, CB)
                        ? DfBrCssHashIdLike : DfBrCssHashUnrestricted;
        Out->Len = T->Pos - StartPos;
        return;
      }
      /* Lone '#' -> delim */
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "#", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;
    }

    case '(':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokLParen;
      Out->Len = T->Pos - StartPos;
      return;
    case ')':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokRParen;
      Out->Len = T->Pos - StartPos;
      return;
    case '[':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokLBracket;
      Out->Len = T->Pos - StartPos;
      return;
    case ']':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokRBracket;
      Out->Len = T->Pos - StartPos;
      return;
    case '{':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokLBrace;
      Out->Len = T->Pos - StartPos;
      return;
    case '}':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokRBrace;
      Out->Len = T->Pos - StartPos;
      return;
    case ',':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokComma;
      Out->Len = T->Pos - StartPos;
      return;
    case ':':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokColon;
      Out->Len = T->Pos - StartPos;
      return;
    case ';':
      (void)NextByte (T);
      Out->Kind = DfBrCssTokSemicolon;
      Out->Len = T->Pos - StartPos;
      return;

    case '+':
    case '.':
    case '-': {
      C1 = PeekByte (T, 1);
      C2 = PeekByte (T, 2);
      if (ThreeCpsStartNumber (C, C1, C2)) {
        ConsumeNumber (T, Out);
        ConsumeNumericTail (T, Out);
        Out->Len = T->Pos - StartPos;
        return;
      }
      if (C == '-') {
        if (C1 == '-' && C2 == '>') {
          (void)NextByte (T); (void)NextByte (T); (void)NextByte (T);
          Out->Kind = DfBrCssTokCdc;
          Out->Len = T->Pos - StartPos;
          return;
        }
        if (ThreeCpsStartIdent (C, C1, C2)) {
          ConsumeIdentLike (T, Out);
          Out->Len = T->Pos - StartPos;
          return;
        }
      }
      /* Plain delim. */
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, &T->Src[StartPos], 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;
    }

    case '<':
      if (PeekByte (T, 1) == '!' && PeekByte (T, 2) == '-' && PeekByte (T, 3) == '-') {
        (void)NextByte (T); (void)NextByte (T); (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokCdo;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "<", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    case '@': {
      C1 = PeekByte (T, 1);
      C2 = PeekByte (T, 2);
      int C3 = PeekByte (T, 3);
      (void)NextByte (T);
      if (ThreeCpsStartIdent (C1, C2, C3)) {
        Buf B;
        BufInit (&B, T->Arena);
        ConsumeName (T, &B);
        BufFinish (&B, &Out->Str, &Out->StrLen);
        Out->Kind = DfBrCssTokAtKeyword;
        Out->Len = T->Pos - StartPos;
        return;
      }
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "@", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;
    }

    case '\\': {
      C1 = PeekByte (T, 1);
      if (TwoCpsAreValidEscape ('\\', C1)) {
        ConsumeIdentLike (T, Out);
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "\\", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      DfBrCssDiagEmit (T->Diags, T->Arena, StartLine, StartCol,
                       "bad-escape");
      return;
    }

    case '~':
      if (PeekByte (T, 1) == '=') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokInclude;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "~", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    case '|':
      if (PeekByte (T, 1) == '=') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokDashMatch;
        Out->Len = T->Pos - StartPos;
        return;
      }
      if (PeekByte (T, 1) == '|') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokColumn;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "|", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    case '^':
      if (PeekByte (T, 1) == '=') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokPrefixMatch;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "^", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    case '$':
      if (PeekByte (T, 1) == '=') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokSuffixMatch;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "$", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    case '*':
      if (PeekByte (T, 1) == '=') {
        (void)NextByte (T); (void)NextByte (T);
        Out->Kind = DfBrCssTokSubstringMatch;
        Out->Len = T->Pos - StartPos;
        return;
      }
      (void)NextByte (T);
      Out->Kind   = DfBrCssTokDelim;
      Out->Str    = DfBrCssArenaStrDup (T->Arena, "*", 1);
      Out->StrLen = 1;
      Out->Len    = T->Pos - StartPos;
      return;

    default: break;
  }

  /* Digit / letter / other */
  if (IsDigit (C)) {
    ConsumeNumber (T, Out);
    ConsumeNumericTail (T, Out);
    Out->Len = T->Pos - StartPos;
    return;
  }
  if (IsNameStart (C)) {
    ConsumeIdentLike (T, Out);
    Out->Len = T->Pos - StartPos;
    return;
  }

  /* A raw NUL: replace with U+FFFD ident body. Emit as delim though,
   * matching the spec's preprocess-as-replace-but-then-delim behavior. */
  if (C == 0) {
    (void)NextByte (T);
    Out->Kind   = DfBrCssTokDelim;
    Out->Str    = DfBrCssArenaStrDup (T->Arena, kReplacementUtf8, 3);
    Out->StrLen = 3;
    Out->Len    = T->Pos - StartPos;
    return;
  }

  /* Anything else: single-codepoint delim. For multi-byte UTF-8, we
   * group the continuation bytes so the string slice covers the full
   * codepoint. */
  {
    int  n = 1;
    int  b0 = C & 0xFF;
    if      ((b0 & 0xE0) == 0xC0) n = 2;
    else if ((b0 & 0xF0) == 0xE0) n = 3;
    else if ((b0 & 0xF8) == 0xF0) n = 4;
    {
      int k;
      for (k = 0; k < n; k++) {
        if (PeekByte (T, 0) < 0) break;
        (void)NextByte (T);
      }
    }
    Out->Kind   = DfBrCssTokDelim;
    Out->Str    = DfBrCssArenaStrDup (T->Arena, &T->Src[StartPos], T->Pos - StartPos);
    Out->StrLen = T->Pos - StartPos;
    Out->Len    = T->Pos - StartPos;
    return;
  }

}

/* ------------------------------------------------------------------
 *  Unit name tables
 * -----------------------------------------------------------------*/

typedef struct { const char *Name; DfBrCssUnit Unit; } UnitEntry;

static const UnitEntry mUnits[] = {
  { "px",   DFBR_CSS_UNIT_PX },
  { "pt",   DFBR_CSS_UNIT_PT },
  { "pc",   DFBR_CSS_UNIT_PC },
  { "cm",   DFBR_CSS_UNIT_CM },
  { "mm",   DFBR_CSS_UNIT_MM },
  { "q",    DFBR_CSS_UNIT_Q },
  { "in",   DFBR_CSS_UNIT_IN },
  { "em",   DFBR_CSS_UNIT_EM },
  { "rem",  DFBR_CSS_UNIT_REM },
  { "ex",   DFBR_CSS_UNIT_EX },
  { "ch",   DFBR_CSS_UNIT_CH },
  { "vw",   DFBR_CSS_UNIT_VW },
  { "vh",   DFBR_CSS_UNIT_VH },
  { "vmin", DFBR_CSS_UNIT_VMIN },
  { "vmax", DFBR_CSS_UNIT_VMAX },
  { "deg",  DFBR_CSS_UNIT_DEG },
  { "rad",  DFBR_CSS_UNIT_RAD },
  { "grad", DFBR_CSS_UNIT_GRAD },
  { "turn", DFBR_CSS_UNIT_TURN },
  { "s",    DFBR_CSS_UNIT_S },
  { "ms",   DFBR_CSS_UNIT_MS },
  { "dpi",  DFBR_CSS_UNIT_DPI },
  { "dpcm", DFBR_CSS_UNIT_DPCM },
  { "dppx", DFBR_CSS_UNIT_DPPX }
};

DfBrCssUnit
DfBrCssUnitFromName (
  const char  *Name,
  usize        Len
  )
{
  usize  i;
  if (Name == DFBR_NULL) return DFBR_CSS_UNIT_UNKNOWN;
  for (i = 0; i < DFBR_ARRAY_LEN (mUnits); i++) {
    if (DfBrCssAsciiEqZ (Name, Len, mUnits[i].Name)) {
      return mUnits[i].Unit;
    }
  }
  return DFBR_CSS_UNIT_UNKNOWN;
}

const char *
DfBrCssUnitName (
  DfBrCssUnit  U
  )
{
  usize  i;
  if (U == DFBR_CSS_UNIT_NONE)    return "";
  if (U == DFBR_CSS_UNIT_PERCENT) return "%";
  for (i = 0; i < DFBR_ARRAY_LEN (mUnits); i++) {
    if (mUnits[i].Unit == U) return mUnits[i].Name;
  }
  return "";
}
