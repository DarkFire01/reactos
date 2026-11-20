/** @file
  DfBrImage.h - image decoding for DefyBrowser.

  Decodes PNG / JPEG / GIF / BMP / TGA / … via stb_image, WebP via
  libwebp, and SVG (rasterized) via nanosvg into a tightly-packed
  32-bit RGBA buffer. Fetching (http, file://, data:) for remote URLs
  is implemented in DfBrImageDecodeFromUrl.

  Design notes
  ============
  * Pure C. stb_image is the only vendored dependency, already sitting
    under 3rdparty/stb/.
  * Output is always RGBA8 with premultiplied=no; the paint engine
    handles alpha compositing.
  * DfBrImageDecoded owns its pixel buffer; call DfBrImageFree to
    release (uses the correct allocator: stb, WebP, or libc).
  * DfBrImageDecodeDataUrl parses `data:[<mime>[;base64]],<payload>`
    per RFC 2397. Payload is base64-decoded (when the `;base64`
    marker is present) or URL-decoded otherwise, then handed to
    stb_image.
**/

#ifndef DFBR_IMAGE_H_
#define DFBR_IMAGE_H_

#include "../common/DfBrTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  u8   *Pixels;      /* RGBA8, row-major, top-down. Freed by DfBrImageFree. */
  u32   Width;
  u32   Height;
  u8    PixelsOwner; /* 0=stbi_image_free, 1=WebPFree, 2=free(). Leave 0 if zero-init. */
} DfBrImageDecoded;

/** Decode a blob of encoded image bytes. Returns DFBR_TRUE on
    success and fills *Out; on failure returns DFBR_FALSE and leaves
    *Out untouched. */
b32
DfBrImageDecode (const u8 *Data, usize Len, DfBrImageDecoded *Out);

/** Decode an RFC 2397 data: URL. Accepts both `;base64` payloads
    and percent-encoded ASCII payloads. Returns DFBR_FALSE if the
    URL is malformed or the decoded bytes don't parse as an image. */
b32
DfBrImageDecodeDataUrl (const char *Url, DfBrImageDecoded *Out);

/** Fetch and decode an image referenced by @p Url, resolving against
    @p BaseUrl if @p Url is relative. Dispatches on scheme:

      * `data:` URLs                    -> DfBrImageDecodeDataUrl
      * `http:` / `https:` URLs         -> DfBrNet blocking GET + decode
      * anything else (relative when
        BaseUrl is NULL or file-based,
        `file://`, ...)                 -> returns DFBR_FALSE

    The net-fetch path is synchronous. This is fine for v1 - pages
    load a handful of images and the browser blocks on layout anyway -
    but a future async/streaming image pipeline will swap this out.

    @p BaseUrl may be NULL when the caller has no base context (e.g.
    Url is already absolute). Returns DFBR_TRUE on success with *Out
    populated; on failure leaves *Out zeroed and returns DFBR_FALSE. */
b32
DfBrImageDecodeFromUrl (const char      *Url,
                        const char      *BaseUrl,
                        DfBrImageDecoded *Out);

/** Release the pixel buffer inside `Img`. Safe to call on a
    zero-initialized struct or one whose Pixels is already NULL. */
void
DfBrImageFree (DfBrImageDecoded *Img);

#ifdef __cplusplus
}
#endif

#endif /* DFBR_IMAGE_H_ */
