/** @file
  DfBrNet.h - minimal networking facade for DefyBrowser.

  Scope
  -----
  The browser needs three things from "the network":

    1. Parse a URL into scheme / host / port / path so we can decide
       what to do with it (local file? data: URI? real HTTP fetch?).
    2. Resolve a possibly-relative reference against a base URL, the
       same way an HTML parser resolves <a href="...">.
    3. Perform a blocking HTTP(S) GET and hand back the body bytes,
       status code, and content-type.

  That surface is deliberately tiny. It is enough to:
    * Navigate the URL bar to a real site.
    * Fetch <script src> and <img src> over the network.
    * Follow server-absolute paths relative to the current page.

  Everything that real browsers layer on top of this - cookies,
  connection pooling, HTTP/2, streaming, async, caching, CORS - is
  out of scope for v1. When any of those becomes necessary, it gets
  added to this facade, not grafted onto call sites.

  Backends
  --------
  Three implementations live under src/net/:

    * mbedtls_impersonate (default on hosted OSes) - cross-platform
      stack built on vendored mbedtls 3.6.x + zlib 1.3.1. A custom
      BIO intercepts the first handshake flight, discards mbedtls's
      generated ClientHello, and emits a byte-for-byte Chrome 120
      shaped hello (cipher order, GREASE, extension order, key-share
      layout, padding to 517 bytes). After splicing, the library's
      running TLS 1.3 transcript hash is reset and re-fed with the
      rewritten bytes, and its sent_extensions bitmap is OR'd with
      the extra extensions Chrome advertises (status_request,
      signed_certificate_timestamp, ...) so the server's responses
      don't get rejected as "unrequested". Result: JA3/JA4-based bot
      detectors see Chrome, but the rest of the TLS state machine
      remains pristine mbedtls. Speaks HTTP/1.1 only; if a server
      selects "h2" in ALPN we tear down and reconnect with an
      "http/1.1"-only ALPN list.

          Opt-out: set DFBR_IMPERSONATE_HELLO=0 to send mbedtls's
          native ClientHello instead - useful for A/B comparison and
          for servers where the impersonation layer interacts badly
          with middleboxes.

          If the chrome-shaped ClientHello is rejected (fatal alert
          during the first TLS handshake), the backend automatically
          tears down the socket and retries once with mbedtls's native
          ClientHello on a fresh connection so bursty multi-fetch
          pages (e.g. Google xjs + CSS) still load.
    * win32_winhttp - WinHTTP + Schannel. Kept as an escape hatch
      behind -DDFBR_NET_BACKEND=win32_winhttp for comparison and for
      Windows-specific proxy/PAC cases.
    * stub - always-error placeholder used when a real transport is
      not available yet (or intentionally disabled via
      DFBR_FORCE_STUB_NET=ON).

  An eventual UEFI backend will wrap EFI_HTTP_PROTOCOL behind the
  same signatures. Nothing in the public API exposes platform-
  specific types, so callers stay portable.

  Ownership
  ---------
  * DfBrUrlParse writes pointer slices into the CALLER's input
    buffer - it does not copy. The caller must keep the input alive
    while the DfBrUrl is read.
  * DfBrUrlResolve returns a freshly malloc'd NUL-terminated string
    the caller must free with free().
  * DfBrHttpGet returns a DfBrHttpResponse whose Body is malloc'd
    by the net library. Release it via DfBrHttpResponseFree (which
    also clears the struct, making double-free safe).
**/

#ifndef DFBR_NET_H_
#define DFBR_NET_H_

#include "../common/DfBrTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 *  URL parsing
 * ================================================================*/

/** Parsed URL slices. All pointers alias into the caller's input
    buffer; none of the fields are NUL-terminated. Use XxxLen for
    lengths. Port is 0 when the URL did not specify one. **/
typedef struct {
  const char  *Scheme;     /* "http", "https", "file", "data", ... */
  usize        SchemeLen;
  const char  *Host;       /* may be empty for schemes without authority */
  usize        HostLen;
  u16          Port;       /* 0 means "use scheme default" */
  const char  *Path;       /* includes leading '/' and query; no fragment */
  usize        PathLen;
} DfBrUrl;

/** Parse @p Url into @p Out. Succeeds for any well-formed absolute
    URL; for schemes without an authority section (e.g. `data:`) the
    Host / Port slices are empty and Path points at the part after
    the colon. @p UrlLen may be 0 to imply strlen(@p Url). **/
DfBrResult DfBrUrlParse (const char *Url, usize UrlLen, DfBrUrl *Out);

/** Resolve @p Ref against @p Base using RFC 3986 reference
    resolution. Both must be NUL-terminated.

    Returns a fresh malloc'd, NUL-terminated absolute URL the caller
    must free (), or NULL on OOM / when Ref could not be parsed
    (relative ref with no base, etc.). Works for:
      * `path/to/foo`         (relative to Base's directory)
      * `/absolute/path`      (Base's scheme + host)
      * `//other.host/x`      (Base's scheme)
      * `https://other/x`     (absolute, returned essentially as-is)
**/
char *DfBrUrlResolve (const char *Base, const char *Ref);

/* ==================================================================
 *  HTTP fetch
 * ================================================================*/

typedef struct {
  u16         StatusCode;        /* 200, 301, 404, ...; 0 on transport error */
  char        ContentType[128];  /* NUL-terminated; empty if server omitted */
  char        FinalUrl[1024];    /* after redirects; NUL-terminated */
  u8         *Body;              /* malloc'd; may be NULL if BodyLen == 0 */
  usize       BodyLen;
} DfBrHttpResponse;

/** Perform a blocking HTTP(S) GET against @p Url.

    On return, @p Out is populated regardless of the status code:
    a 404 with an HTML error page still gives you StatusCode=404 and
    the page bytes in Body. Transport-level failures (DNS error, TLS
    handshake fail, connection refused) return a non-OK DfBrResult
    and Body == NULL.

    Caller MUST release the response with DfBrHttpResponseFree even
    if the call returned non-OK, to be safe against partial fills.
**/
DfBrResult DfBrHttpGet (const char *Url, DfBrHttpResponse *Out);

/** Blocking HTTP request with method + optional header block + body.
    @p Method is usually "GET" or "POST" (others may be rejected).
    @p ExtraRequestHeaders: extra "Name: value\r\n" lines, or NULL.
    @p Body / BodyLen: request entity (ignored for GET in practice).
    On success populates @p Out like DfBrHttpGet (including redirects).
    Caller must DfBrHttpResponseFree. **/
DfBrResult DfBrHttpRequestEx (
  const char        *Url,
  const char        *Method,
  const char        *ExtraRequestHeaders,
  const u8          *Body,
  usize              BodyLen,
  DfBrHttpResponse  *Out
  );

/** Release any buffer owned by @p R and zero the struct. Safe to
    call with a zeroed / never-filled struct. **/
void DfBrHttpResponseFree (DfBrHttpResponse *R);

/* ==================================================================
 *  One-time init
 * ================================================================*/

/** Initialize the backend. Idempotent; safe to call more than once.
    Most callers don't need this - DfBrHttpGet auto-initializes. **/
DfBrResult DfBrNetInit (void);

/** Tear down the backend. Safe to call without a matching Init. **/
void DfBrNetShutdown (void);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_NET_H_ */
