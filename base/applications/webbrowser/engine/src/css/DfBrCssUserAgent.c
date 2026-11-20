/** @file
  DfBrCssUserAgent.c - built-in user-agent stylesheet.

  Baked in as a C string literal and parsed lazily on first use.
  Covers the minimum rules every real browser hard-codes:
    - default display modes per HTML element category
    - list markers, table display
    - blockish defaults for semantic sectioning
    - inline defaults for phrasing content
    - font weights for headings and strong/b
    - preformatted whitespace for pre, code blocks, textarea
    - hidden for the [hidden] attribute selector

  Kept deliberately small and human-readable; adding rules is a text
  edit with no rebuild-time codegen.
**/

#include "DfBrCssInternal.h"

static DfBrCssStylesheet *mUASheet = DFBR_NULL;

static const char mUASource[] =
  "/* DefyBrowser built-in user agent stylesheet (v1) */\n"
  "\n"
  "html, body, address, article, aside, blockquote, details, dialog,\n"
  "dir, div, dl, fieldset, figcaption, figure, footer, form, header,\n"
  "hgroup, hr, main, menu, nav, ol, p, pre, section, summary, ul,\n"
  "h1, h2, h3, h4, h5, h6, center, listing, plaintext, xmp, noframes,\n"
  "frameset, frame { display: block; }\n"
  "\n"
  "/* Body gets the universally-expected 8px gutter that no HTML\n"
  " * 4.01 / CSS 2.1 spec mandates but every production browser\n"
  " * has. Lots of sites assume it and rely on it to breathe. */\n"
  "body { margin: 8px; }\n"
  "\n"
  "head, link, meta, script, style, title, template, source, track,\n"
  "param, base { display: none; }\n"
  "\n"
  "li { display: list-item; }\n"
  "\n"
  "table { display: table; }\n"
  "caption { display: table-caption; }\n"
  "colgroup { display: table-column-group; }\n"
  "col { display: table-column; }\n"
  "thead { display: table-header-group; }\n"
  "tbody { display: table-row-group; }\n"
  "tfoot { display: table-footer-group; }\n"
  "tr { display: table-row; }\n"
  "td, th { display: table-cell; }\n"
  "\n"
  "[hidden] { display: none; }\n"
  "\n"
  "h1 { font-size: 2em;    font-weight: bold; margin: 0.67em 0; }\n"
  "h2 { font-size: 1.5em;  font-weight: bold; margin: 0.83em 0; }\n"
  "h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }\n"
  "h4 { font-size: 1em;    font-weight: bold; margin: 1.33em 0; }\n"
  "h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }\n"
  "h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }\n"
  "\n"
  "p, blockquote, pre, ul, ol, dl, figure { margin: 1em 0; }\n"
  "blockquote, figure { margin-left: 40px; margin-right: 40px; }\n"
  "ul, ol { padding-left: 40px; }\n"
  "ul { list-style-type: disc; }\n"
  "ol { list-style-type: decimal; }\n"
  "ul ul { list-style-type: circle; }\n"
  "ul ul ul { list-style-type: square; }\n"
  "dd { margin-left: 40px; }\n"
  "\n"
  "b, strong { font-weight: bold; }\n"
  "i, em, cite, dfn, var, address { font-style: italic; }\n"
  "u, ins { text-decoration: underline; }\n"
  "s, strike, del { text-decoration: line-through; }\n"
  "small { font-size: smaller; }\n"
  "big   { font-size: larger; }\n"
  "mark  { background-color: yellow; color: black; }\n"
  "sub   { vertical-align: sub;   font-size: smaller; }\n"
  "sup   { vertical-align: super; font-size: smaller; }\n"
  "\n"
  "pre, code, kbd, samp, tt { font-family: monospace; }\n"
  "pre { white-space: pre; }\n"
  "textarea { white-space: pre-wrap; font-family: monospace; }\n"
  "\n"
  "a { color: #0000ee; text-decoration: underline; }\n"
  "a:visited { color: #551a8b; }\n"
  "\n"
  "hr { border: 1px inset; margin: 0.5em auto; }\n"
  "\n"
  "center { text-align: center; }\n"
  "\n"
  "input, button, select, textarea { font-family: inherit; font-size: inherit; }\n"
  "button { padding: 2px 6px; }\n"
  "\n"
  "fieldset { border: 2px groove; padding: 0.35em 0.625em 0.75em; margin: 0 2px; }\n"
  "legend { padding: 0 2px; }\n"
  ;

const DfBrCssStylesheet *
DfBrCssUserAgentStylesheet (void) {
  if (mUASheet == DFBR_NULL) {
    DfBrCssParseOptions Opts;
    Opts.Origin              = DFBR_CSS_ORIGIN_UA;
    Opts.KeepUnknownProps    = DFBR_FALSE;
    Opts.EmitSourceLocations = DFBR_FALSE;
    mUASheet = DfBrCssParse (mUASource, sizeof (mUASource) - 1, &Opts);
  }
  return mUASheet;
}

void
DfBrCssUserAgentShutdown (void) {
  if (mUASheet != DFBR_NULL) {
    DfBrCssStylesheetDestroy (mUASheet);
    mUASheet = DFBR_NULL;
  }
}
