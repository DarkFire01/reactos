/** @file
  DfBrNetWinHttp.c - Win32 backend of DfBrNet, built on WinHTTP.

  Why WinHTTP over WinInet? WinHTTP is the "service-mode" HTTP stack
  ships with Windows since 5.1; it does TLS 1.2/1.3, automatic
  redirects, and has no implicit dependency on user-profile state
  (cookies, proxies) the way WinInet does. That keeps the browser
  reproducible between machines and easier to reason about.

  Threading: all calls here are synchronous. WinHTTP supports async
  I/O with callbacks, but we don't need it yet - the browser is a
  single-threaded render loop, and blocking on fetch in response to
  a user navigation is fine. The async path is purely additive if we
  ever want pipelined resource loading.
**/

#include "../DfBrNet.h"
#include "../DfBrCookies.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Windows headers must be included in this order. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

/* ------------------------------------------------------------------
 *  Init / shutdown
 *
 *  We hold a single HINTERNET "session" handle for the lifetime of
 *  the process. Each fetch opens a connection+request under it.
 *  Creating one session per request works but burns a handle per
 *  call and triggers proxy-config probing every time.
 * -----------------------------------------------------------------*/

static HINTERNET mSession;
static b32       mInitialized;

DfBrResult
DfBrNetInit (void)
{
  if (mInitialized) return DFBR_OK;

  /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY asks WinHTTP to honor the
   * system proxy settings (WPAD / PAC / manual). That's the least
   * surprising default; if we ever want to force direct we can
   * switch to WINHTTP_ACCESS_TYPE_NO_PROXY. */
  mSession = WinHttpOpen (
    L"DefyBrowser/0.1",
    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
    WINHTTP_NO_PROXY_NAME,
    WINHTTP_NO_PROXY_BYPASS,
    0);

  if (mSession == NULL) return DFBR_ERR_IO;

  /* Reasonable default timeouts (ms). WinHTTP defaults are 60s each
   * which is too long when the user typed a dead host. 15/15/30/30
   * feels snappy for interactive use. */
  WinHttpSetTimeouts (mSession, 15000, 15000, 30000, 30000);
  mInitialized = DFBR_TRUE;
  return DFBR_OK;
}

void
DfBrNetShutdown (void)
{
  if (mSession != NULL) {
    WinHttpCloseHandle (mSession);
    mSession = NULL;
  }
  mInitialized = DFBR_FALSE;
}

/* ------------------------------------------------------------------
 *  Narrow -> wide helpers
 *
 *  WinHTTP takes UTF-16. We never accept user-authored non-ASCII
 *  hostnames yet (IDN would need punycode), so we widen via
 *  MultiByteToWideChar with CP_UTF8 and hope for the best for the
 *  path/query, which typically contains percent-encoded bytes.
 * -----------------------------------------------------------------*/

static wchar_t *
WidenN (const char *S, usize SLen)
{
  int      NeedChars;
  wchar_t *W;
  if (S == DFBR_NULL) return DFBR_NULL;
  /* MultiByteToWideChar expects an int length; -1 means NUL-terminated.
   * We pass SLen explicitly so our non-terminated slices convert too. */
  NeedChars = MultiByteToWideChar (CP_UTF8, 0, S, (int)SLen, NULL, 0);
  if (NeedChars <= 0) return DFBR_NULL;
  W = (wchar_t *)malloc ((usize)(NeedChars + 1) * sizeof (wchar_t));
  if (W == DFBR_NULL) return DFBR_NULL;
  MultiByteToWideChar (CP_UTF8, 0, S, (int)SLen, W, NeedChars);
  W[NeedChars] = L'\0';
  return W;
}

/* ------------------------------------------------------------------
 *  Public: DfBrHttpGet
 * -----------------------------------------------------------------*/

void
DfBrHttpResponseFree (DfBrHttpResponse *R)
{
  if (R == DFBR_NULL) return;
  if (R->Body != DFBR_NULL) free (R->Body);
  memset (R, 0, sizeof (*R));
}

static void
ReadContentType (HINTERNET Req, char OutZ[128])
{
  wchar_t  Wide[256];
  DWORD    Len = sizeof (Wide);
  if (WinHttpQueryHeaders (Req, WINHTTP_QUERY_CONTENT_TYPE,
                           WINHTTP_HEADER_NAME_BY_INDEX, Wide, &Len,
                           WINHTTP_NO_HEADER_INDEX)) {
    int N = WideCharToMultiByte (CP_UTF8, 0, Wide, -1, OutZ, 127, NULL, NULL);
    if (N > 0) OutZ[N - 1] = '\0';
    else       OutZ[0] = '\0';
  } else {
    OutZ[0] = '\0';
  }
}

/* WinHttpQueryHeaders with a repeating header index: one call per
 * Set-Cookie: header value. We feed each one into the cookie jar,
 * using the request's post-redirect URL as the "response URL" so
 * default Domain / Path resolve correctly. */
static void
DrainSetCookies (HINTERNET Req, const char *ResponseUrl)
{
  DWORD Index = 0;

  for (;;) {
    wchar_t  Wide[4096];
    DWORD    Len = sizeof (Wide);
    if (!WinHttpQueryHeaders (Req, WINHTTP_QUERY_SET_COOKIE,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              Wide, &Len, &Index))
    {
      break;
    }
    /* Convert to UTF-8 + feed to the jar. Header lines from servers
     * are ASCII-only in practice, but do the full conversion to keep
     * the path uniform. */
    {
      int   NeedBytes = WideCharToMultiByte (
                          CP_UTF8, 0, Wide, -1, NULL, 0, NULL, NULL);
      char *Utf8;
      if (NeedBytes <= 0) continue;
      Utf8 = (char *)malloc ((usize)NeedBytes);
      if (Utf8 == NULL) continue;
      WideCharToMultiByte (CP_UTF8, 0, Wide, -1,
                           Utf8, NeedBytes, NULL, NULL);
      DfBrCookiesAcceptSetCookie (ResponseUrl, Utf8);
      free (Utf8);
    }
  }
}

static void
ReadFinalUrl (HINTERNET Req, char OutZ[1024])
{
  wchar_t  Wide[2048];
  DWORD    Len = sizeof (Wide);
  if (WinHttpQueryOption (Req, WINHTTP_OPTION_URL, Wide, &Len)) {
    int N = WideCharToMultiByte (CP_UTF8, 0, Wide, -1, OutZ, 1023, NULL, NULL);
    if (N > 0) OutZ[N - 1] = '\0';
    else       OutZ[0] = '\0';
  } else {
    OutZ[0] = '\0';
  }
}

DfBrResult
DfBrHttpGet (const char *Url, DfBrHttpResponse *Out)
{
  DfBrUrl     Parsed;
  DfBrResult  Rc;
  HINTERNET   Conn = NULL;
  HINTERNET   Req  = NULL;
  b32         IsHttps;
  u16         Port;
  wchar_t    *WHost = NULL;
  wchar_t    *WPath = NULL;
  DWORD       StatusCode = 0;
  DWORD       StatusLen  = sizeof (DWORD);
  u8         *Body       = NULL;
  usize       BodyLen    = 0;
  usize       BodyCap    = 0;

  if (Url == DFBR_NULL || Out == DFBR_NULL) return DFBR_ERR_INVALID_ARG;
  memset (Out, 0, sizeof (*Out));

  Rc = DfBrNetInit ();
  if (Rc != DFBR_OK) return Rc;

  Rc = DfBrUrlParse (Url, 0, &Parsed);
  if (Rc != DFBR_OK) return Rc;

  /* Only http / https for now. */
  IsHttps = DFBR_FALSE;
  if (Parsed.SchemeLen == 5 && memcmp (Parsed.Scheme, "https", 5) == 0) {
    IsHttps = DFBR_TRUE;
  } else if (Parsed.SchemeLen == 4 && memcmp (Parsed.Scheme, "http", 4) == 0) {
    IsHttps = DFBR_FALSE;
  } else {
    return DFBR_ERR_INVALID_ARG;
  }

  Port = Parsed.Port;
  if (Port == 0) Port = IsHttps ? 443 : 80;

  WHost = WidenN (Parsed.Host, Parsed.HostLen);
  WPath = WidenN (Parsed.Path, Parsed.PathLen);
  if (WHost == NULL || WPath == NULL) {
    Rc = DFBR_ERR_OUT_OF_MEMORY;
    goto done;
  }

  Conn = WinHttpConnect (mSession, WHost, Port, 0);
  if (Conn == NULL) { Rc = DFBR_ERR_IO; goto done; }

  Req = WinHttpOpenRequest (
    Conn,
    L"GET",
    WPath,
    NULL,                          /* HTTP/1.1 */
    WINHTTP_NO_REFERER,
    WINHTTP_DEFAULT_ACCEPT_TYPES,
    IsHttps ? WINHTTP_FLAG_SECURE : 0);
  if (Req == NULL) { Rc = DFBR_ERR_IO; goto done; }

  /* Accept automatic redirects. Disable only if we ever need to see
   * Location headers ourselves (unlikely for a v1 browser). */
  {
    DWORD Flag = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption (Req, WINHTTP_OPTION_REDIRECT_POLICY, &Flag, sizeof (Flag));
  }

  /* Disable WinHTTP's built-in cookie engine so it doesn't race with
   * DfBrCookies. Without this flag WinHTTP transparently stores +
   * replays cookies for us, but the browser has no way to inspect or
   * persist them, and we'd see duplicate Cookie: headers once we
   * start sending our own. */
  {
    DWORD Flag = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption (Req, WINHTTP_OPTION_DISABLE_FEATURE, &Flag, sizeof (Flag));
  }

  /* Send the exact header set a clean desktop-Chrome install
   * produces. Google bot-detection fingerprints on the full tuple,
   * and empirically a Windows Chrome UA paired with WinHTTP's
   * Schannel TLS still gets served the 89 KB "sorry" shim on the
   * first hit to google.com/search - the shim then runs
   * `location.replace('?sei=…')` which 429s straight to the
   * captcha.
   *
   * Using a *Linux* desktop-Chrome UA instead consistently gets
   * us the full 1.1 MB search page. Two reasons, best I can tell:
   *   1. Google's win/mac Chrome fingerprints include JA3/JA4
   *      signals from BoringSSL that we can't reproduce with
   *      Schannel; the Linux cohort has a wider tolerance.
   *   2. The "sei" session-echo redirect is not emitted in the
   *      Linux Chrome response, so there's no bot-detection
   *      follow-up step.
   * Neither is documented - both are observed. If Google tightens
   * this, we'll have to ship a real TLS client. */
  {
    static const wchar_t  UA[] =
      L"User-Agent: Mozilla/5.0 (X11; Linux x86_64) "
      L"AppleWebKit/537.36 (KHTML, like Gecko) "
      L"Chrome/120.0.0.0 Safari/537.36\r\n"
      L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
      L"image/avif,image/webp,image/apng,*/*;q=0.8\r\n"
      L"Accept-Language: en-US,en;q=0.9\r\n"
      L"sec-ch-ua: \"Not_A Brand\";v=\"8\", \"Chromium\";v=\"120\", "
      L"\"Google Chrome\";v=\"120\"\r\n"
      L"sec-ch-ua-mobile: ?0\r\n"
      L"sec-ch-ua-platform: \"Linux\"\r\n"
      L"sec-fetch-site: none\r\n"
      L"sec-fetch-mode: navigate\r\n"
      L"sec-fetch-user: ?1\r\n"
      L"sec-fetch-dest: document\r\n"
      L"Upgrade-Insecure-Requests: 1\r\n";
    WinHttpAddRequestHeaders (
      Req,
      UA,
      (DWORD)-1L,
      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  }

  /* Attach cookies if our jar has any match for this URL. Built by
   * DfBrCookies as a raw "name=value; ..." string; we prefix with
   * "Cookie: " and terminate with CRLF so WinHTTP treats it as a
   * complete header. Safe to skip silently if the jar is empty. */
  {
    char *CookieBody = DfBrCookiesHeaderFor (Url);
    if (CookieBody != NULL && CookieBody[0] != '\0') {
      usize    LenA = strlen ("Cookie: ") + strlen (CookieBody) + 2;
      char    *HeaderA = (char *)malloc (LenA + 1);
      wchar_t *HeaderW;
      if (HeaderA != NULL) {
        memcpy (HeaderA, "Cookie: ", 8);
        memcpy (HeaderA + 8, CookieBody, strlen (CookieBody));
        memcpy (HeaderA + 8 + strlen (CookieBody), "\r\n", 2);
        HeaderA[LenA] = '\0';
        HeaderW = WidenN (HeaderA, LenA);
        if (HeaderW != NULL) {
          WinHttpAddRequestHeaders (
            Req, HeaderW, (DWORD)-1L,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
          free (HeaderW);
        }
        free (HeaderA);
      }
#if defined (DFBR_NET_VERBOSE)
      /* Diagnostic: mirror what went over the wire so we can spot
       * when the JS-set SG_SS (or any other challenge cookie) is
       * actually being replayed on the follow-up request that
       * Google 429s. Truncate: some names carry kilobyte payloads.
       * Gated so production runs stay quiet. */
      {
        usize CbLen = strlen (CookieBody);
        if (CbLen > 120) {
          fprintf (stdout, "[cookies] send %s <- %.120s... (+%u)\n",
                   Url, CookieBody, (unsigned)(CbLen - 120));
        } else {
          fprintf (stdout, "[cookies] send %s <- %s\n", Url, CookieBody);
        }
        fflush (stdout);
      }
#endif
    }
    if (CookieBody != NULL) free (CookieBody);
  }

  if (!WinHttpSendRequest (
        Req,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA,       0,
        0,
        0))
  {
    Rc = DFBR_ERR_IO;
    goto done;
  }

  if (!WinHttpReceiveResponse (Req, NULL)) {
    Rc = DFBR_ERR_IO;
    goto done;
  }

  if (!WinHttpQueryHeaders (
        Req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &StatusCode, &StatusLen,
        WINHTTP_NO_HEADER_INDEX))
  {
    StatusCode = 0;
  }

  /* Stream the body. We double Cap each time we fill it; that is
   * O(amortized) and avoids a syscall for the exact size. */
  for (;;) {
    DWORD Avail = 0;
    DWORD Read  = 0;

    if (!WinHttpQueryDataAvailable (Req, &Avail)) {
      Rc = DFBR_ERR_IO;
      goto done;
    }
    if (Avail == 0) break;

    if (BodyLen + Avail > BodyCap) {
      usize  NewCap = (BodyCap == 0) ? 4096 : BodyCap;
      u8    *New;
      while (NewCap < BodyLen + Avail) NewCap *= 2;
      New = (u8 *)realloc (Body, NewCap);
      if (New == NULL) { Rc = DFBR_ERR_OUT_OF_MEMORY; goto done; }
      Body   = New;
      BodyCap = NewCap;
    }

    if (!WinHttpReadData (Req, Body + BodyLen, Avail, &Read)) {
      Rc = DFBR_ERR_IO;
      goto done;
    }
    BodyLen += Read;
  }

  /* Shrink to exact on success so we return a tight buffer. */
  if (Body != NULL && BodyLen < BodyCap) {
    u8 *Trim = (u8 *)realloc (Body, BodyLen > 0 ? BodyLen : 1);
    if (Trim != NULL) Body = Trim;
  }

  Out->StatusCode = (u16)StatusCode;
  Out->Body       = Body;
  Out->BodyLen    = BodyLen;
  Body            = NULL;  /* ownership transferred */

  ReadContentType (Req, Out->ContentType);
  ReadFinalUrl    (Req, Out->FinalUrl);
  /* Fold any Set-Cookie headers into the jar. Doing this after
   * FinalUrl read so the cookie default-domain/path uses the
   * post-redirect URL, which is what browsers spec. */
  DrainSetCookies (Req,
    (Out->FinalUrl[0] != '\0') ? Out->FinalUrl : Url);

  Rc = DFBR_OK;

done:
  if (Req  != NULL) WinHttpCloseHandle (Req);
  if (Conn != NULL) WinHttpCloseHandle (Conn);
  if (WHost != NULL) free (WHost);
  if (WPath != NULL) free (WPath);
  if (Body != NULL) free (Body);
  return Rc;
}

DfBrResult
DfBrHttpRequestEx (
  const char        *Url,
  const char        *Method,
  const char        *ExtraRequestHeaders,
  const u8          *ReqBody,
  usize              ReqBodyLen,
  DfBrHttpResponse  *Out
  )
{
  if (Method != NULL && strcmp (Method, "GET") == 0
      && (ReqBody == NULL || ReqBodyLen == 0)
      && (ExtraRequestHeaders == NULL || ExtraRequestHeaders[0] == '\0')) {
    return DfBrHttpGet (Url, Out);
  }
  if (Out != DFBR_NULL) {
    memset (Out, 0, sizeof (*Out));
  }
  return DFBR_ERR_UNSUPPORTED;
}
