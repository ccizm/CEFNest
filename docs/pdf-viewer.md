# The PDF viewer recipe

English | [简体中文](pdf-viewer.zh-CN.md)

Clicking a PDF link renders an empty page — measured 2026-10-26: the distro
package ships no PDF viewer extension, an `application/pdf` commit produces
no viewer `embed` in the page and an empty body. CEF exposes no viewer face,
so the component cannot wire this; but a host can assemble the whole loop
from the existing API: interception + a custom scheme + script serving. This
is a host-side recipe; the component changes nothing.

## The three parts

1. **Interception**: `chromium_session_intercept_add(id, patterns, NULL)`
   with the pattern `"*://*/*.pdf"` (or domains you narrow it to). On a hit
   `::request-intercept` arrives, and the host answers
   `chromium_session_request_respond(request_id,
   CHROMIUM_REQUEST_REDIRECT, "viewer://pdf?src=<original URL>")` — a
   redirect to your own viewer scheme.
2. **The viewer scheme**: `chromium_session_scheme_register("viewer", …)`
   (flags before `start()`, see
   [control-channel](control-channel.md) → `CHROMIUM_HELPER_SCHEMES`), and
   serve the viewer shell page through `::scheme-request` /
   `scheme_response` / `scheme_write` / `scheme_end`. You package the shell
   page yourself — the [pdf.js](https://mozilla.github.io/pdf.js/)
   distribution (~2 MiB) comes from upstream into your own resource
   directory; the component does not carry it, that size is your call.
3. **Getting the PDF bytes**: pdf.js in the shell page needs the original
   PDF. Two roads:
   - the shell page `fetch`es the original URL directly — register `viewer`
     with the `cors` flag for the cross-scheme read, or make the origin
     readable;
   - or the host fetches: the interception hit already names the URL, so
     your own network stack can retrieve the bytes and feed them to the
     shell page via `scheme_write` (the original URL stays a parameter, the
     page never re-requests it). This road keeps intranet PDFs that only
     cookies can see working, because the host process is already sitting
     on the interception point.

## Downloads and printing are untouched

- The intercept pattern only matches `.pdf`: what should *not* redirect
  (`Content-Disposition: attachment`, an explicit download) you decide in
  `::request-intercept` — redirect, or allow (allowing rides the existing
  download flow).
- In-page printing: `window.print()` arrives as `::print-requested` and
  `print_pdf` hands you the bytes; the viewer's own print button takes the
  same road and never enters Chromium's print pipeline.

## Order

```c
/* before start() */
chromium_session_scheme_register (self, "viewer", flags);
/* after start(), before the first navigation */
chromium_session_intercept_add (self, 1, patterns, NULL);
```

Scheme flags only take effect at process start (proxy-shaped); intercept
rules can be added at any time while running. Both halves have their own
gates (`scheme-gate`, `intercept-gate`); this recipe adds no component code
path to test — what you test is your shell page.
