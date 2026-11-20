/** @file
  DfBrNetUrl.c - URL parsing + relative-ref resolution.

  Pure C99, no platform deps. RFC 3986 is the reference. We only
  implement what the browser actually needs; the rest (userinfo,
  IPv6 zone ids, IDN, percent-encode normalization) is intentionally
  deferred.
**/

#include "DfBrNet.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 *  Parsing helpers
 * -----------------------------------------------------------------*/

static b32
IsSchemeStart (char C)
{
  return ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z'))
           ? DFBR_TRUE : DFBR_FALSE;
}

static b32
IsSchemeChar (char C)
{
  return (IsSchemeStart (C) ||
          (C >= '0' && C <= '9') ||
          C == '+' || C == '-' || C == '.')
           ? DFBR_TRUE : DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Public: DfBrUrlParse
 * -----------------------------------------------------------------*/

DfBrResult
DfBrUrlParse (const char *Url, usize UrlLen, DfBrUrl *Out)
{
  usize  i;
  usize  SchemeEnd;
  usize  AfterScheme;

  if (Url == DFBR_NULL || Out == DFBR_NULL) return DFBR_ERR_INVALID_ARG;
  if (UrlLen == 0) UrlLen = strlen (Url);

  memset (Out, 0, sizeof (*Out));

  /* ---- Scheme. Must match [A-Za-z][A-Za-z0-9+.-]* before a ':'. - */
  if (UrlLen == 0 || !IsSchemeStart (Url[0])) return DFBR_ERR_INVALID_ARG;
  for (i = 1; i < UrlLen; i++) {
    if (Url[i] == ':') break;
    if (!IsSchemeChar (Url[i])) return DFBR_ERR_INVALID_ARG;
  }
  if (i == UrlLen) return DFBR_ERR_INVALID_ARG;   /* no colon */
  SchemeEnd    = i;
  Out->Scheme    = Url;
  Out->SchemeLen = SchemeEnd;
  AfterScheme    = SchemeEnd + 1;                  /* past ':' */

  /* ---- Authority. Only present when followed by "//". Schemes like
   *      data: and mailto: jump straight to the path. */
  if (AfterScheme + 1 < UrlLen &&
      Url[AfterScheme] == '/' && Url[AfterScheme + 1] == '/')
  {
    usize HostStart = AfterScheme + 2;
    usize HostEnd;
    usize PortStart = 0;
    usize j;

    /* Host ends at the first of: '/' (path), '?' (query), '#' (fragment),
     * or end of string. We tolerate but don't parse userinfo (user@host)
     * or IPv6 literals ('['..']'); a future pass can extend this. */
    for (j = HostStart; j < UrlLen; j++) {
      char C = Url[j];
      if (C == '/' || C == '?' || C == '#') break;
    }
    HostEnd = j;

    /* Port: right-most ':' inside the host span. */
    for (j = HostEnd; j > HostStart; j--) {
      if (Url[j - 1] == ':') { PortStart = j; break; }
    }

    if (PortStart > 0) {
      u32 P = 0;
      for (j = PortStart; j < HostEnd; j++) {
        if (Url[j] < '0' || Url[j] > '9') { P = 0; break; }
        P = P * 10 + (u32)(Url[j] - '0');
        if (P > 65535) { P = 0; break; }
      }
      Out->Port = (u16)P;
      Out->Host    = Url + HostStart;
      Out->HostLen = PortStart - 1 - HostStart;
    } else {
      Out->Host    = Url + HostStart;
      Out->HostLen = HostEnd - HostStart;
    }

    /* Path starts at HostEnd. If there is no path, emit "/" as the
     * canonical "root" - WinHTTP requires a non-empty path. Fragment
     * is stripped; query is kept. */
    {
      usize PathStart = HostEnd;
      usize FragStart = UrlLen;
      usize k;
      for (k = PathStart; k < UrlLen; k++) {
        if (Url[k] == '#') { FragStart = k; break; }
      }
      if (PathStart >= FragStart) {
        Out->Path    = "/";
        Out->PathLen = 1;
      } else {
        Out->Path    = Url + PathStart;
        Out->PathLen = FragStart - PathStart;
      }
    }
  } else {
    /* No authority. The entire remainder (minus fragment) is the
     * path-ish part. data:, mailto:, tel:, javascript: land here. */
    usize FragStart = UrlLen;
    usize k;
    for (k = AfterScheme; k < UrlLen; k++) {
      if (Url[k] == '#') { FragStart = k; break; }
    }
    Out->Path    = Url + AfterScheme;
    Out->PathLen = FragStart - AfterScheme;
  }

  return DFBR_OK;
}

/* ------------------------------------------------------------------
 *  RFC 3986 §5.2 reference resolution, simplified.
 *
 *  We handle the cases a browser actually hits:
 *    * Ref is absolute with scheme         -> return Ref verbatim.
 *    * Ref starts with "//"                -> Base scheme + Ref.
 *    * Ref starts with "/"                 -> Base scheme+host + Ref.
 *    * Otherwise (relative)                -> merge with Base's dir.
 *
 *  Dot-segment normalization ("foo/./bar", "foo/../bar") is minimal;
 *  a production parser should do the full §5.2.4 dance. We do enough
 *  for typical link shapes and leave odd relative paths for later.
 * -----------------------------------------------------------------*/

static b32
HasScheme (const char *S)
{
  usize i;
  if (S == DFBR_NULL) return DFBR_FALSE;
  if (!IsSchemeStart (S[0])) return DFBR_FALSE;
  for (i = 1; S[i] != '\0'; i++) {
    if (S[i] == ':') return DFBR_TRUE;
    if (!IsSchemeChar (S[i])) return DFBR_FALSE;
  }
  return DFBR_FALSE;
}

char *
DfBrUrlResolve (const char *Base, const char *Ref)
{
  DfBrUrl  BaseU;
  usize    RefLen;
  char    *Out;

  if (Ref == DFBR_NULL) return DFBR_NULL;
  RefLen = strlen (Ref);

  /* Absolute URL -> return a copy. */
  if (HasScheme (Ref)) {
    Out = (char *)malloc (RefLen + 1);
    if (Out == DFBR_NULL) return DFBR_NULL;
    memcpy (Out, Ref, RefLen + 1);
    return Out;
  }

  if (Base == DFBR_NULL || DfBrUrlParse (Base, 0, &BaseU) != DFBR_OK) {
    return DFBR_NULL;
  }

  /* "//host/path" -> inherit scheme only. */
  if (RefLen >= 2 && Ref[0] == '/' && Ref[1] == '/') {
    usize Total = BaseU.SchemeLen + 1 + RefLen;
    Out = (char *)malloc (Total + 1);
    if (Out == DFBR_NULL) return DFBR_NULL;
    memcpy (Out, BaseU.Scheme, BaseU.SchemeLen);
    Out[BaseU.SchemeLen] = ':';
    memcpy (Out + BaseU.SchemeLen + 1, Ref, RefLen);
    Out[Total] = '\0';
    return Out;
  }

  /* "/path" -> scheme + host + path. */
  if (RefLen >= 1 && Ref[0] == '/') {
    usize Total = BaseU.SchemeLen + 3 + BaseU.HostLen;
    char  PortBuf[8] = {0};
    usize PortBufLen = 0;
    if (BaseU.Port != 0) {
      int n = snprintf (PortBuf, sizeof (PortBuf), ":%u", (unsigned)BaseU.Port);
      if (n > 0) PortBufLen = (usize)n;
    }
    Total += PortBufLen + RefLen;
    Out = (char *)malloc (Total + 1);
    if (Out == DFBR_NULL) return DFBR_NULL;
    {
      char *W = Out;
      memcpy (W, BaseU.Scheme, BaseU.SchemeLen); W += BaseU.SchemeLen;
      memcpy (W, "://", 3);                      W += 3;
      memcpy (W, BaseU.Host, BaseU.HostLen);     W += BaseU.HostLen;
      if (PortBufLen) { memcpy (W, PortBuf, PortBufLen); W += PortBufLen; }
      memcpy (W, Ref, RefLen);                   W += RefLen;
      *W = '\0';
    }
    return Out;
  }

  /* Relative path -> replace last segment of Base's path. */
  {
    const char *BaseDirEnd = BaseU.Path + BaseU.PathLen;
    const char *P;
    usize       DirLen;
    usize       Total;
    char        PortBuf[8] = {0};
    usize       PortBufLen = 0;

    /* Walk the base path backwards looking for the last '/' - the
     * directory portion is everything up to and including it. We
     * track FoundSlash explicitly because the old sentinel-based
     * detection (`BaseDirEnd == end`) couldn't distinguish
     * `path="/"` (slash found at position 0, dir = "/") from
     * `path="foo"` (no slash, dir should be empty). */
    {
      b32 FoundSlash = DFBR_FALSE;
      for (P = BaseDirEnd; P > BaseU.Path; P--) {
        if (P[-1] == '/') { BaseDirEnd = P; FoundSlash = DFBR_TRUE; break; }
      }
      if (!FoundSlash) BaseDirEnd = BaseU.Path;
    }
    DirLen = (usize)(BaseDirEnd - BaseU.Path);

    if (BaseU.Port != 0) {
      int n = snprintf (PortBuf, sizeof (PortBuf), ":%u", (unsigned)BaseU.Port);
      if (n > 0) PortBufLen = (usize)n;
    }

    /* RFC 3986 §5.2.3: when the base URL has no path (e.g.
     * `http://host`), a relative reference resolves against `/`.
     * Without this the resolver used to glue "empty.css" straight
     * onto the hostname, producing the infamous
     * `http://acid3.acidtests.orgempty.css` that acid3 hit. */
    {
      b32 NeedLeadingSlash = (DirLen == 0 || BaseU.Path[0] != '/')
                             ? DFBR_TRUE : DFBR_FALSE;
      usize Slash = NeedLeadingSlash ? 1u : 0u;

      Total = BaseU.SchemeLen + 3 + BaseU.HostLen + PortBufLen
              + Slash + DirLen + RefLen;
      Out = (char *)malloc (Total + 1);
      if (Out == DFBR_NULL) return DFBR_NULL;
      {
        char *W = Out;
        memcpy (W, BaseU.Scheme, BaseU.SchemeLen); W += BaseU.SchemeLen;
        memcpy (W, "://", 3);                      W += 3;
        memcpy (W, BaseU.Host, BaseU.HostLen);     W += BaseU.HostLen;
        if (PortBufLen) { memcpy (W, PortBuf, PortBufLen); W += PortBufLen; }
        if (NeedLeadingSlash) { *W++ = '/'; }
        memcpy (W, BaseU.Path, DirLen);            W += DirLen;
        memcpy (W, Ref, RefLen);                   W += RefLen;
        *W = '\0';
      }
    }
    return Out;
  }
}
