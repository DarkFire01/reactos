/** @file
  DfBrCookies.c - implementation of the DefyBrowser cookie jar.

  Data structure
  --------------
  A flat, heap-allocated dynamic array of DfBrCookie structs. Cookie
  counts in real browsing rarely exceed a few hundred total; a linear
  scan on every request is comfortably cheaper than any index scheme
  we'd have to maintain. If profiling ever surfaces this as hot we
  can replace the array with a host-keyed hash without touching the
  public API.

  Parse shape
  -----------
  The "name=value; attr=val; attr; ..." grammar is small but littered
  with edge cases (attribute names are case-insensitive, Expires
  dates have five different formats in the wild, Max-Age trumps
  Expires). We do a single forward scan that splits on ';' then
  further splits each attribute on '=', trimming OWS. Unknown
  attributes are ignored.

  Persistence format (Netscape-style cookies.txt)
  -----------------------------------------------
  One cookie per line, TAB-separated, as:

      host\tTRUE|FALSE\tpath\tsecure\texpires\tname\tvalue

  - host:     domain (with or without leading '.'; '.' means "includes
              subdomains").
  - TRUE/FALSE: "TRUE" when the cookie should be sent to subdomains
              (i.e. Domain attribute was explicitly set). "FALSE" for
              host-only cookies.
  - path:     the Path attribute; defaults to '/'.
  - secure:   "TRUE" | "FALSE".
  - expires:  unix timestamp, or 0 for session cookies (not persisted).
  - name, value: raw bytes. Tabs inside values are rejected at parse
              time because the format is tab-delimited; browsers very
              rarely produce tabs in cookie values in practice.

  This matches the format Netscape (then Mozilla, then curl) invented
  in 1998 and which a pile of cookie editors understand.

  Expiry
  ------
  A cookie with ExpiresEpoch == 0 is a session cookie; it lives as
  long as the process does but is not persisted. Persistent cookies
  with ExpiresEpoch <= now are treated as deleted on lookup and
  garbage-collected at the next Accept / Save.
**/

#include "DfBrCookies.h"
#include "DfBrNet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 *  Internal types
 * -----------------------------------------------------------------*/

typedef struct {
  char   *Domain;         /* lower-cased; never includes leading dot */
  char   *Path;           /* always starts with '/' */
  char   *Name;
  char   *Value;
  b32     IncludeSubs;    /* domain cookie (Domain= was set) */
  b32     Secure;
  b32     HttpOnly;
  /* Unix epoch seconds. 0 means session cookie. */
  i64     ExpiresEpoch;
} DfBrCookie;

typedef struct {
  DfBrCookie *Data;
  usize       Count;
  usize       Cap;
  char       *PersistPath;  /* malloc'd, or NULL for in-memory */
  b32         Initialized;
} DfBrCookieJar;

static DfBrCookieJar mJar;

/* Forward decls. */
static void  CookieFree   (DfBrCookie *C);
static void  SaveToDisk   (void);
static void  LoadFromDisk (const char *Path);
static i64   ParseHttpDate (const char *S);

/* ------------------------------------------------------------------
 *  Tiny helpers
 * -----------------------------------------------------------------*/

static char *
DupN (const char *S, usize N)
{
  char *Out = (char *)malloc (N + 1);
  if (Out == NULL) return NULL;
  if (N > 0) memcpy (Out, S, N);
  Out[N] = '\0';
  return Out;
}

static char *
Dup (const char *S) { return DupN (S, strlen (S)); }

static char *
DupLowerN (const char *S, usize N)
{
  char *Out = DupN (S, N);
  usize i;
  if (Out == NULL) return NULL;
  for (i = 0; i < N; i++) {
    u8 C = (u8)Out[i];
    if (C >= 'A' && C <= 'Z') Out[i] = (char)(C + ('a' - 'A'));
  }
  return Out;
}

/* Case-insensitive ASCII strcmp. */
static int
AsciiCmpI (const char *A, const char *B)
{
  while (*A != '\0' && *B != '\0') {
    u8 Ca = (u8)*A;
    u8 Cb = (u8)*B;
    if (Ca >= 'A' && Ca <= 'Z') Ca = (u8)(Ca + ('a' - 'A'));
    if (Cb >= 'A' && Cb <= 'Z') Cb = (u8)(Cb + ('a' - 'A'));
    if (Ca != Cb) return (int)Ca - (int)Cb;
    A++; B++;
  }
  return (int)(u8)*A - (int)(u8)*B;
}

/* Trim leading and trailing OWS (space / tab). Returns a pointer
 * INTO the original string; writes a NUL at the new end so `*End`
 * in `[Start, End)` is untouched. Does not malloc. */
static void
TrimOws (const char **StartInOut, const char **EndInOut)
{
  const char *S = *StartInOut;
  const char *E = *EndInOut;
  while (S < E && (*S == ' ' || *S == '\t')) S++;
  while (E > S && (E[-1] == ' ' || E[-1] == '\t')) E--;
  *StartInOut = S;
  *EndInOut   = E;
}

/* Append to the jar's dynamic array. Returns DFBR_FALSE on OOM. */
static b32
JarAppend (DfBrCookie C)
{
  if (mJar.Count == mJar.Cap) {
    usize       NewCap = (mJar.Cap == 0) ? 16 : mJar.Cap * 2;
    DfBrCookie *New    = (DfBrCookie *)realloc (mJar.Data,
                                                NewCap * sizeof (*New));
    if (New == NULL) return DFBR_FALSE;
    mJar.Data = New;
    mJar.Cap  = NewCap;
  }
  mJar.Data[mJar.Count++] = C;
  return DFBR_TRUE;
}

/* Remove the i-th cookie, swapping the last one into its slot. */
static void
JarRemoveAt (usize Index)
{
  if (Index >= mJar.Count) return;
  CookieFree (&mJar.Data[Index]);
  if (Index != mJar.Count - 1) {
    mJar.Data[Index] = mJar.Data[mJar.Count - 1];
  }
  mJar.Count--;
}

static void
CookieFree (DfBrCookie *C)
{
  if (C == NULL) return;
  free (C->Domain); free (C->Path); free (C->Name); free (C->Value);
  memset (C, 0, sizeof (*C));
}

/* ------------------------------------------------------------------
 *  Domain + path matching (RFC 6265 §5.1.3 / §5.1.4)
 * -----------------------------------------------------------------*/

/* RFC 6265 §5.1.3: domain match. All inputs are lowercase ASCII.
 *   host-only cookies require exact equality.
 *   subdomain-including cookies match when RequestHost == CookieDomain
 *   or ends in ("." + CookieDomain). */
static b32
DomainMatch (const char *RequestHost, const char *CookieDomain,
             b32 IncludeSubs)
{
  usize Rh, Cd;
  if (RequestHost == NULL || CookieDomain == NULL) return DFBR_FALSE;
  if (AsciiCmpI (RequestHost, CookieDomain) == 0) return DFBR_TRUE;
  if (!IncludeSubs) return DFBR_FALSE;
  Rh = strlen (RequestHost);
  Cd = strlen (CookieDomain);
  if (Rh <= Cd) return DFBR_FALSE;
  if (RequestHost[Rh - Cd - 1] != '.') return DFBR_FALSE;
  return (b32)(AsciiCmpI (RequestHost + Rh - Cd, CookieDomain) == 0);
}

/* RFC 6265 §5.1.4: path match. Case-sensitive. */
static b32
PathMatch (const char *RequestPath, const char *CookiePath)
{
  usize R, C;
  if (RequestPath == NULL || CookiePath == NULL) return DFBR_FALSE;
  R = strlen (RequestPath);
  C = strlen (CookiePath);
  if (C == 0) return DFBR_TRUE;
  if (R < C) return DFBR_FALSE;
  if (memcmp (RequestPath, CookiePath, C) != 0) return DFBR_FALSE;
  /* Full prefix; exact match or boundary at '/'. */
  if (R == C) return DFBR_TRUE;
  if (CookiePath[C - 1] == '/') return DFBR_TRUE;
  if (RequestPath[C] == '/') return DFBR_TRUE;
  return DFBR_FALSE;
}

/* Default path from a request URL's path (RFC 6265 §5.1.4). If the
 * path is empty or does not start with '/', returns "/". Otherwise
 * returns the path up to (but not including) the rightmost '/', or
 * just "/" when that leaves no segment. The returned string is
 * malloc'd. */
static char *
DefaultPath (const char *UrlPath, usize UrlPathLen)
{
  const char *P;
  const char *LastSlash = NULL;
  if (UrlPath == NULL || UrlPathLen == 0 || UrlPath[0] != '/') {
    return Dup ("/");
  }
  for (P = UrlPath; P < UrlPath + UrlPathLen; P++) {
    if (*P == '/') LastSlash = P;
  }
  if (LastSlash == NULL || LastSlash == UrlPath) return Dup ("/");
  return DupN (UrlPath, (usize)(LastSlash - UrlPath));
}

/* ------------------------------------------------------------------
 *  Persistence
 * -----------------------------------------------------------------*/

/* Save the entire persistent slice of the jar back to PersistPath.
 * Session cookies (Expires == 0) are skipped - they by design vanish
 * with the process. */
static void
SaveToDisk (void)
{
  FILE *F;
  usize i;
  if (mJar.PersistPath == NULL) return;
  F = fopen (mJar.PersistPath, "wb");
  if (F == NULL) return;

  fprintf (F,
    "# DefyBrowser cookies file (Netscape-style). Do not edit while\n"
    "# the browser is running.\n");

  for (i = 0; i < mJar.Count; i++) {
    DfBrCookie *C = &mJar.Data[i];
    if (C->ExpiresEpoch == 0) continue;   /* session cookie, not persisted */
    /* Domain with optional leading dot marker for "include subs". */
    fprintf (F, "%s%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
             C->IncludeSubs ? "." : "",
             C->Domain,
             C->IncludeSubs ? "TRUE" : "FALSE",
             C->Path,
             C->Secure ? "TRUE" : "FALSE",
             (long long)C->ExpiresEpoch,
             C->Name,
             C->Value);
  }
  fclose (F);
}

/* Load persistent cookies from disk. Expired entries are discarded
 * during load. Missing or malformed files are tolerated silently
 * (first run, corrupt file on crash, etc.). */
static void
LoadFromDisk (const char *Path)
{
  FILE  *F;
  char   Line[4096];
  i64    Now = (i64)time (NULL);

  F = fopen (Path, "rb");
  if (F == NULL) return;

  while (fgets (Line, sizeof (Line), F) != NULL) {
    char       *Tok[7];
    usize       N = 0;
    char       *P = Line;
    usize       Len;
    DfBrCookie  C;
    i64         Exp;

    if (Line[0] == '#' || Line[0] == '\n' || Line[0] == '\r' ||
        Line[0] == '\0') continue;

    /* Strip trailing newline. */
    Len = strlen (Line);
    while (Len > 0 && (Line[Len - 1] == '\n' || Line[Len - 1] == '\r')) {
      Line[--Len] = '\0';
    }

    /* Split on TAB. We need exactly 7 fields. */
    while (N < 7) {
      char *Tab = strchr (P, '\t');
      Tok[N++] = P;
      if (Tab == NULL) break;
      *Tab = '\0';
      P = Tab + 1;
    }
    if (N != 7) continue;

    memset (&C, 0, sizeof (C));
    {
      const char *Host = Tok[0];
      b32 Sub = (Host[0] == '.');
      C.IncludeSubs = Sub;
      C.Domain      = DupLowerN (Sub ? Host + 1 : Host,
                                 strlen (Sub ? Host + 1 : Host));
    }
    C.Path   = Dup (Tok[2]);
    C.Secure = (b32)(AsciiCmpI (Tok[3], "TRUE") == 0);
    Exp      = (i64)strtoll (Tok[4], NULL, 10);
    C.ExpiresEpoch = Exp;
    C.Name         = Dup (Tok[5]);
    C.Value        = Dup (Tok[6]);

    if (C.Domain == NULL || C.Path == NULL || C.Name == NULL ||
        C.Value  == NULL) { CookieFree (&C); continue; }
    if (Exp != 0 && Exp <= Now) { CookieFree (&C); continue; }
    if (!JarAppend (C))         { CookieFree (&C); continue; }
  }

  fclose (F);
}

/* ------------------------------------------------------------------
 *  Public init / shutdown
 * -----------------------------------------------------------------*/

DfBrResult
DfBrCookiesInit (const char *PersistPath)
{
  if (mJar.Initialized) return DFBR_OK;
  memset (&mJar, 0, sizeof (mJar));
  mJar.Initialized = DFBR_TRUE;
  if (PersistPath != NULL && PersistPath[0] != '\0') {
    mJar.PersistPath = Dup (PersistPath);
    if (mJar.PersistPath == NULL) return DFBR_ERR_OUT_OF_MEMORY;
    LoadFromDisk (mJar.PersistPath);
  }
  return DFBR_OK;
}

void
DfBrCookiesShutdown (void)
{
  usize i;
  if (!mJar.Initialized) return;
  for (i = 0; i < mJar.Count; i++) CookieFree (&mJar.Data[i]);
  free (mJar.Data);
  free (mJar.PersistPath);
  memset (&mJar, 0, sizeof (mJar));
}

void
DfBrCookiesClear (void)
{
  usize i;
  for (i = 0; i < mJar.Count; i++) CookieFree (&mJar.Data[i]);
  mJar.Count = 0;
  SaveToDisk ();
}

usize
DfBrCookiesCount (void) { return mJar.Count; }

/* ------------------------------------------------------------------
 *  Parse Set-Cookie:
 * -----------------------------------------------------------------*/

/* Returns whether the attribute name (Start..End, no copy) matches
 * @p Lit case-insensitively. */
static b32
AttrNameIs (const char *Start, const char *End, const char *Lit)
{
  usize N = (usize)(End - Start);
  usize L = strlen (Lit);
  usize i;
  if (N != L) return DFBR_FALSE;
  for (i = 0; i < N; i++) {
    u8 Ca = (u8)Start[i];
    u8 Cb = (u8)Lit[i];
    if (Ca >= 'A' && Ca <= 'Z') Ca = (u8)(Ca + ('a' - 'A'));
    if (Cb >= 'A' && Cb <= 'Z') Cb = (u8)(Cb + ('a' - 'A'));
    if (Ca != Cb) return DFBR_FALSE;
  }
  return DFBR_TRUE;
}

/* Parse the value of Max-Age (a signed decimal number of seconds).
 * Missing digits => 0 (treat as expired per RFC). */
static i64
ParseMaxAge (const char *S, const char *E)
{
  i64 Acc = 0;
  b32 Neg = DFBR_FALSE;
  const char *P = S;
  while (P < E && (*P == ' ' || *P == '\t')) P++;
  if (P < E && *P == '-') { Neg = DFBR_TRUE; P++; }
  else if (P < E && *P == '+')             { P++; }
  while (P < E && *P >= '0' && *P <= '9') {
    Acc = Acc * 10 + (*P - '0');
    P++;
  }
  return Neg ? -Acc : Acc;
}

/* HTTP date parser. Accepts (loosely) RFC 1123, RFC 850, and asctime.
 * Returns an epoch timestamp (seconds since 1970-01-01 UTC), or 0 if
 * the string couldn't be parsed. Good enough for the three formats
 * actually seen in the wild; industry cookies universally use RFC
 * 1123 now. */
static const char kMonths[12][4] = {
  "Jan","Feb","Mar","Apr","May","Jun",
  "Jul","Aug","Sep","Oct","Nov","Dec"
};

static int
MonthIndex (const char *S)
{
  int i;
  for (i = 0; i < 12; i++) {
    if ((S[0] | 0x20) == (kMonths[i][0] | 0x20) &&
        (S[1] | 0x20) == (kMonths[i][1] | 0x20) &&
        (S[2] | 0x20) == (kMonths[i][2] | 0x20))
      return i;
  }
  return -1;
}

/* Pseudo-GMT: days since epoch for a calendar date. Standard Julian
 * day trick, faster + cheaper than invoking timegm(). */
static i64
DaysFromCivil (i32 Y, i32 M, i32 D)
{
  /* Howard Hinnant's algorithm (public domain). */
  i32 Yy = (M <= 2) ? Y - 1 : Y;
  i32 Era = (Yy >= 0 ? Yy : Yy - 399) / 400;
  u32 Yoe = (u32)(Yy - Era * 400);
  u32 Doy = (u32)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
  u32 Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return (i64)Era * 146097 + (i64)Doe - 719468;
}

static i64
ParseHttpDate (const char *S)
{
  const char *P = S;
  int D = 0, Y = 0, Mo = -1, Hh = 0, Mm = 0, Ss = 0;
  /* Skip weekday + any commas / whitespace. */
  while (*P != '\0' && !(*P >= '0' && *P <= '9')) P++;
  /* Day. */
  while (*P >= '0' && *P <= '9') { D = D * 10 + (*P - '0'); P++; }
  while (*P == ' ' || *P == '-' || *P == '\t') P++;
  /* Month (3 letters). */
  if (!(*P)) return 0;
  Mo = MonthIndex (P);
  if (Mo < 0) {
    /* asctime variant: "Sun Nov  6 08:49:37 1994" ? Retry scan. */
    return 0;
  }
  P += 3;
  while (*P == ' ' || *P == '-' || *P == '\t') P++;
  /* Year. */
  while (*P >= '0' && *P <= '9') { Y = Y * 10 + (*P - '0'); P++; }
  if (Y < 70)         Y += 2000;
  else if (Y < 100)   Y += 1900;
  while (*P == ' ' || *P == '\t') P++;
  /* hh:mm:ss. */
  while (*P >= '0' && *P <= '9') { Hh = Hh * 10 + (*P - '0'); P++; }
  if (*P == ':') P++;
  while (*P >= '0' && *P <= '9') { Mm = Mm * 10 + (*P - '0'); P++; }
  if (*P == ':') P++;
  while (*P >= '0' && *P <= '9') { Ss = Ss * 10 + (*P - '0'); P++; }
  if (D < 1 || D > 31 || Mo < 0 || Mo > 11) return 0;

  return DaysFromCivil (Y, Mo + 1, D) * 86400 +
         (i64)Hh * 3600 + (i64)Mm * 60 + (i64)Ss;
}

/* Parse a single Set-Cookie: value. Returns DFBR_TRUE on success
 * and fills @p Out (Domain / Path / Name / Value are malloc'd and
 * freshly owned by the caller). On failure @p Out is zeroed. */
static b32
ParseSetCookie (const char *SetCookie, const char *ResponseUrl,
                DfBrCookie *Out)
{
  const char   *S;
  const char   *Eq;
  const char   *Sc;
  const char   *End;
  DfBrUrl       UrlParts;
  b32           OK = DFBR_FALSE;
  b32           HasMaxAge = DFBR_FALSE;
  i64           MaxAge = 0;
  i64           ExpiresEpoch = 0;
  const char   *NameStart, *NameEnd, *ValueStart, *ValueEnd;

  memset (Out, 0, sizeof (*Out));

  if (SetCookie == NULL || ResponseUrl == NULL) return DFBR_FALSE;
  if (DfBrUrlParse (ResponseUrl, 0, &UrlParts) != DFBR_OK) return DFBR_FALSE;

  S   = SetCookie;
  End = SetCookie + strlen (SetCookie);

  /* First segment: "name=value" (name must be non-empty, value can be). */
  Sc = strchr (S, ';');
  if (Sc == NULL) Sc = End;
  Eq = memchr (S, '=', (usize)(Sc - S));
  if (Eq == NULL) return DFBR_FALSE;
  NameStart = S;  NameEnd = Eq;
  ValueStart = Eq + 1; ValueEnd = Sc;
  TrimOws (&NameStart, &NameEnd);
  TrimOws (&ValueStart, &ValueEnd);
  if (NameEnd - NameStart <= 0) return DFBR_FALSE;

  Out->Name  = DupN (NameStart,  (usize)(NameEnd - NameStart));
  Out->Value = DupN (ValueStart, (usize)(ValueEnd - ValueStart));
  if (Out->Name == NULL || Out->Value == NULL) goto fail;

  /* Strip surrounding double-quotes from the value if present. A
   * lot of servers emit cookie values wrapped in quotes. */
  {
    usize Vl = strlen (Out->Value);
    if (Vl >= 2 && Out->Value[0] == '"' && Out->Value[Vl - 1] == '"') {
      memmove (Out->Value, Out->Value + 1, Vl - 2);
      Out->Value[Vl - 2] = '\0';
    }
  }

  /* Attribute loop. */
  S = (Sc < End) ? Sc + 1 : End;
  while (S < End) {
    const char *AttrStart;
    const char *AttrEnd;
    const char *AValStart;
    const char *AValEnd;
    const char *AEq;

    Sc = strchr (S, ';');
    if (Sc == NULL) Sc = End;

    AttrStart = S;
    AttrEnd   = Sc;
    TrimOws (&AttrStart, &AttrEnd);
    AEq = memchr (AttrStart, '=', (usize)(AttrEnd - AttrStart));
    if (AEq != NULL) {
      AValStart = AEq + 1;
      AValEnd   = AttrEnd;
      AttrEnd   = AEq;
      TrimOws (&AttrStart, &AttrEnd);
      TrimOws (&AValStart, &AValEnd);
    } else {
      AValStart = AValEnd = AttrEnd;
    }

    if (AttrNameIs (AttrStart, AttrEnd, "Domain")) {
      usize Ln = (usize)(AValEnd - AValStart);
      /* Leading dot is allowed and signals "include subs"; strip it
       * in storage per RFC 6265 §5.2.3. */
      if (Ln > 0 && *AValStart == '.') { AValStart++; Ln--; }
      if (Ln > 0) {
        free (Out->Domain);
        Out->Domain      = DupLowerN (AValStart, Ln);
        Out->IncludeSubs = DFBR_TRUE;
      }
    } else if (AttrNameIs (AttrStart, AttrEnd, "Path")) {
      usize Ln = (usize)(AValEnd - AValStart);
      if (Ln > 0) {
        free (Out->Path);
        Out->Path = DupN (AValStart, Ln);
      }
    } else if (AttrNameIs (AttrStart, AttrEnd, "Max-Age")) {
      HasMaxAge = DFBR_TRUE;
      MaxAge    = ParseMaxAge (AValStart, AValEnd);
    } else if (AttrNameIs (AttrStart, AttrEnd, "Expires")) {
      char Tmp[128];
      usize Ln = (usize)(AValEnd - AValStart);
      if (Ln < sizeof (Tmp)) {
        memcpy (Tmp, AValStart, Ln); Tmp[Ln] = '\0';
        ExpiresEpoch = ParseHttpDate (Tmp);
      }
    } else if (AttrNameIs (AttrStart, AttrEnd, "Secure")) {
      Out->Secure = DFBR_TRUE;
    } else if (AttrNameIs (AttrStart, AttrEnd, "HttpOnly")) {
      Out->HttpOnly = DFBR_TRUE;
    }
    /* SameSite / Priority / etc. ignored for v1. */

    S = (Sc < End) ? Sc + 1 : End;
  }

  /* Defaults: Domain = request host, host-only; Path = default-path. */
  if (Out->Domain == NULL) {
    Out->Domain      = DupLowerN (UrlParts.Host, UrlParts.HostLen);
    Out->IncludeSubs = DFBR_FALSE;
  }
  if (Out->Path == NULL) {
    Out->Path = DefaultPath (UrlParts.Path, UrlParts.PathLen);
  }
  if (Out->Path == NULL || Out->Domain == NULL) goto fail;

  /* Resolve expiry. Max-Age takes precedence per RFC. 0 / negative
   * Max-Age means "delete this cookie immediately". */
  if (HasMaxAge) {
    if (MaxAge <= 0) {
      Out->ExpiresEpoch = 1;   /* past; triggers deletion */
    } else {
      Out->ExpiresEpoch = (i64)time (NULL) + MaxAge;
    }
  } else if (ExpiresEpoch != 0) {
    Out->ExpiresEpoch = ExpiresEpoch;
  } else {
    Out->ExpiresEpoch = 0;     /* session cookie */
  }

  OK = DFBR_TRUE;

fail:
  if (!OK) CookieFree (Out);
  return OK;
}

/* ------------------------------------------------------------------
 *  Public: Accept
 * -----------------------------------------------------------------*/

void
DfBrCookiesAcceptSetCookie (const char *ResponseUrl, const char *SetCookieValue)
{
  DfBrCookie C;
  usize i;
  i64    Now;

  if (!mJar.Initialized) DfBrCookiesInit (NULL);
  if (ResponseUrl == NULL || SetCookieValue == NULL) return;
  if (!ParseSetCookie (SetCookieValue, ResponseUrl, &C)) return;

  Now = (i64)time (NULL);

  /* Replace any existing cookie with the same (domain, path, name). */
  for (i = 0; i < mJar.Count; ) {
    DfBrCookie *E = &mJar.Data[i];
    if (AsciiCmpI (E->Domain, C.Domain) == 0 &&
        strcmp    (E->Path,   C.Path)   == 0 &&
        strcmp    (E->Name,   C.Name)   == 0)
    {
      JarRemoveAt (i);
      continue;
    }
    i++;
  }

  /* A freshly expired cookie: having replaced the old entry, we're
   * done. Don't add the tombstone. */
  if (C.ExpiresEpoch != 0 && C.ExpiresEpoch <= Now) {
    CookieFree (&C);
    SaveToDisk ();
    return;
  }

  if (!JarAppend (C)) CookieFree (&C);
  SaveToDisk ();
}

/* ------------------------------------------------------------------
 *  Public: HeaderFor
 * -----------------------------------------------------------------*/

/* Selection sort by path length, longest first. Count is tiny in
 * practice (rarely > 5 matches per URL), so quadratic is fine. */
static void
SortByPathLenDesc (usize *Indices, usize N)
{
  usize i, j;
  for (i = 0; i < N; i++) {
    usize Best = i;
    for (j = i + 1; j < N; j++) {
      usize La = strlen (mJar.Data[Indices[Best]].Path);
      usize Lb = strlen (mJar.Data[Indices[j]].Path);
      if (Lb > La) Best = j;
    }
    if (Best != i) {
      usize T = Indices[i]; Indices[i] = Indices[Best]; Indices[Best] = T;
    }
  }
}

char *
DfBrCookiesHeaderFor (const char *Url)
{
  DfBrUrl Parts;
  b32     IsSecure;
  char   *Host;
  char   *Path;
  usize  *Matches;
  usize   MatchCount = 0;
  usize   i;
  usize   TotalLen = 0;
  char   *Out      = NULL;
  i64     Now      = (i64)time (NULL);

  if (!mJar.Initialized) return NULL;
  if (Url == NULL) return NULL;
  if (DfBrUrlParse (Url, 0, &Parts) != DFBR_OK) return NULL;
  if (Parts.HostLen == 0) return NULL;

  IsSecure = (b32)(Parts.SchemeLen == 5 &&
                   memcmp (Parts.Scheme, "https", 5) == 0);
  Host = DupLowerN (Parts.Host, Parts.HostLen);
  Path = (Parts.PathLen > 0) ? DupN (Parts.Path, Parts.PathLen) : Dup ("/");
  if (Host == NULL || Path == NULL) goto done;

  Matches = (usize *)malloc (sizeof (usize) * (mJar.Count > 0 ? mJar.Count : 1));
  if (Matches == NULL) goto done;

  /* Evict any expired cookies as a side-effect; cheap to do here. */
  for (i = 0; i < mJar.Count; ) {
    DfBrCookie *C = &mJar.Data[i];
    if (C->ExpiresEpoch != 0 && C->ExpiresEpoch <= Now) {
      JarRemoveAt (i);
      continue;
    }
    i++;
  }

  for (i = 0; i < mJar.Count; i++) {
    DfBrCookie *C = &mJar.Data[i];
    if (C->Secure && !IsSecure) continue;
    if (!DomainMatch (Host, C->Domain, C->IncludeSubs)) continue;
    if (!PathMatch  (Path, C->Path))                    continue;
    Matches[MatchCount++] = i;
  }
  if (MatchCount == 0) { free (Matches); goto done; }

  SortByPathLenDesc (Matches, MatchCount);

  /* Compute buffer size: per cookie "name=value" plus "; " between. */
  for (i = 0; i < MatchCount; i++) {
    DfBrCookie *C = &mJar.Data[Matches[i]];
    TotalLen += strlen (C->Name) + 1 + strlen (C->Value);
    if (i + 1 < MatchCount) TotalLen += 2;
  }

  Out = (char *)malloc (TotalLen + 1);
  if (Out == NULL) { free (Matches); goto done; }
  {
    char *W = Out;
    for (i = 0; i < MatchCount; i++) {
      DfBrCookie *C = &mJar.Data[Matches[i]];
      usize Ln = strlen (C->Name);
      usize Lv = strlen (C->Value);
      memcpy (W, C->Name, Ln);  W += Ln;
      *W++ = '=';
      memcpy (W, C->Value, Lv); W += Lv;
      if (i + 1 < MatchCount) { *W++ = ';'; *W++ = ' '; }
    }
    *W = '\0';
  }
  free (Matches);

done:
  free (Host);
  free (Path);
  return Out;
}
