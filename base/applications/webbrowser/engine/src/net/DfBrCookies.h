/** @file
  DfBrCookies.h - tiny persistent cookie jar for DefyBrowser.

  What lives here
  ---------------
  * A process-wide cookie store keyed on (domain, path, name) tuples.
  * Minimal RFC 6265 semantics: domain match, path match, Secure +
    HttpOnly attributes, Max-Age / Expires-based expiry, session vs
    persistent classification.
  * Two entry points the network backend cares about:
      - DfBrCookiesHeaderFor (URL)        - build a "Cookie: a=b; c=d"
        value to attach to an outgoing request, filtered by
        domain/path/secure and sorted longest-path-first the way
        RFC 6265 §5.4 expects.
      - DfBrCookiesAcceptSetCookie (URL, SetCookieValue) - fold one
        server Set-Cookie: response header into the jar.
  * Optional on-disk persistence: a line-oriented text file, one
    cookie per line ("host\tTRUE/FALSE\tpath\tsecure\texpires\tname\tvalue"
    a la Netscape's cookies.txt), so cookies survive browser restarts.

  Why in-tree and not libcurl / WinHTTP-native
  --------------------------------------------
  WinHTTP *can* persist cookies (INTERNET_COOKIE_PERSIST) but only in
  Windows' per-user WinINet jar, which is shared with Edge/IE/etc. We
  want DefyBrowser's cookie state to be independent and inspectable
  (for privacy and for deterministic testing). Rolling our own is ~200
  lines of parser + some bookkeeping, which is cheap.

  Thread safety
  -------------
  None. The jar is a single global table; the browser's render loop
  is single-threaded. If async fetches ever appear, guard the
  functions below with a mutex and keep the public API unchanged.
**/

#ifndef DFBR_COOKIES_H_
#define DFBR_COOKIES_H_

#include "../common/DfBrTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the cookie jar. Safe to call more than once; subsequent
    calls are no-ops. When @p PersistPath is non-NULL the jar reads
    that file at startup and rewrites it after every Accept. Pass
    NULL for pure in-memory operation (useful in tests). **/
DfBrResult
DfBrCookiesInit (
  const char  *PersistPath
  );

/** Drop every cookie and close the persistent file (if any). Safe
    to call even if Init was never called. **/
void
DfBrCookiesShutdown (void);

/** Clear all cookies (both session and persistent). The on-disk
    file, if any, is rewritten as empty. **/
void
DfBrCookiesClear (void);

/** Build a "Cookie: ..." request header value for @p Url. Filters
    the jar by (domain-match, path-match, Secure if https) and
    concatenates "name=value" pairs separated by "; " in longest-path
    order as RFC 6265 §5.4 specifies.

    Returns a malloc'd NUL-terminated string the caller must free(),
    or NULL if no cookie matches @p Url. **/
char *
DfBrCookiesHeaderFor (
  const char  *Url
  );

/** Fold a single Set-Cookie: header line into the jar. @p ResponseUrl
    provides the default domain (its host) and path (directory of its
    path) when the cookie omits either. Invalid cookies (missing
    name, bad domain) are silently dropped.

    @p SetCookieValue must be the value *after* the "Set-Cookie:"
    prefix - just the "name=value; attr; ..." portion.

    Multiple Set-Cookie headers? Call this function once per header
    line; cookies don't fold into each other. **/
void
DfBrCookiesAcceptSetCookie (
  const char  *ResponseUrl,
  const char  *SetCookieValue
  );

/** Debug: number of cookies currently held (includes expired ones
    that haven't been swept yet; they are filtered on GET). **/
usize
DfBrCookiesCount (void);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_COOKIES_H_ */
