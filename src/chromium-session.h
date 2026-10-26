#pragma once

#include <gtk/gtk.h>

#include "chromium-plugin.h"
#include "chromium-view.h"

G_BEGIN_DECLS

#define CHROMIUM_TYPE_SESSION (chromium_session_get_type ())
G_DECLARE_FINAL_TYPE (ChromiumSession, chromium_session, CHROMIUM, SESSION, GObject)

/* The host half of the helper's control channel. The session owns the listening
 * socket, the Chromium process, and the page state the helper reports back —
 * the widget only knows how to composite a surface. */
ChromiumSession   *chromium_session_new           (const char *helper_path);

void               chromium_session_set_view      (ChromiumSession *self,
                                                   ChromiumView *view);
gboolean           chromium_session_start         (ChromiumSession *self,
                                                   const char *initial_url,
                                                   GError **error);
void               chromium_session_stop          (ChromiumSession *self);

/* Where the helper keeps the page's own data — cookies, localStorage,
 * IndexedDB, the HTTP cache — and whether it keeps any at all.
 *
 * The default is no directory: the helper runs an in-memory profile
 * (Chromium's incognito), and the few files CEF must write for the install go
 * into a private directory on tmpfs that is removed when the helper exits. A
 * host that wants a persistent profile sets an absolute @path; everything the
 * page stores then lives under it, and the next start() with the same path
 * sees the cookies and storage the previous run left behind.
 *
 * The path is checked in chromium_session_start (), not here: a relative path
 * — and therefore a "~"-prefixed one, which is not expanded — is refused
 * there, the directory is created with mode 0700 if it does not exist, and a
 * directory a live session already holds, in this process or in another, is
 * refused rather than shared. Setting this while a helper runs changes
 * nothing until the next start(): the helper reads it once, at spawn time.
 *
 * Chromium keeps session cookies in memory even in this mode, so they do not
 * survive a restart; that is Chromium's own rule, not a switch here. There is
 * no way to store the HTTP cache on disk while keeping cookies in memory —
 * this directory is one complete profile. */
void               chromium_session_set_data_directory (ChromiumSession *self,
                                                        const char *path);
const char        *chromium_session_get_data_directory (ChromiumSession *self);

/* Remove the data directory and everything under it. Refused while this
 * session runs and while a helper in any process still holds the directory;
 * TRUE means it is gone, including the case where there was nothing there.
 * The property keeps its value, so the next start() recreates the directory
 * and the host does not have to set it again. */
gboolean           chromium_session_clear_data         (ChromiumSession *self,
                                                        GError **error);

/* Network identity, fixed when the helper starts — Chromium has no runtime setter
 * for either of these.
 *
 * @proxy is a proxy string as Chromium spells it (`scheme://host:port`, e.g.
 * "socks5://127.0.0.1:1080"), or NULL for a direct connection. @proxy_bypass is
 * Chromium's bypass list (`;`-separated host patterns, e.g. "<-loopback>" to stop
 * Chromium's own loopback exemption). Both are read by the next start(); setting
 * them while a helper runs changes nothing until that start().
 *
 * @accept_language is a comma-separated list without spaces ("zh-CN,zh"), and
 * becomes both the Accept-Language header and navigator.language. NULL keeps
 * Chromium's default.
 *
 * Unlike CHROMIUM_USER_AGENT, which is inherited from the host's environment,
 * these are session state: an inherited CHROMIUM_HELPER_PROXY in the host's
 * environment is removed at spawn so a host that asked for no proxy gets none. */
void               chromium_session_set_proxy        (ChromiumSession *self,
                                                      const char *proxy);
const char        *chromium_session_get_proxy        (ChromiumSession *self);
void               chromium_session_set_proxy_bypass (ChromiumSession *self,
                                                      const char *bypass);
const char        *chromium_session_get_proxy_bypass (ChromiumSession *self);
void               chromium_session_set_accept_language (ChromiumSession *self,
                                                         const char *language);
const char        *chromium_session_get_accept_language (ChromiumSession *self);

/* Autoplay policy, the same spawn-time rule as the proxy: Chromium has no
 * runtime and no per-site face, so this is one global value read by the next
 * start() and handed over as --autoplay-policy. @policy is Chromium's own
 * spelling ("user-gesture-required" is the strict one a browser shell wants,
 * "document-user-activation-required" the stricter still); NULL or "" keeps
 * Chromium's default. */
void               chromium_session_set_autoplay_policy (ChromiumSession *self,
                                                         const char *policy);
const char        *chromium_session_get_autoplay_policy (ChromiumSession *self);

/* Runtime accessibility. The session watches org.a11y.Status's
 * ScreenReaderEnabled property on the session bus and calls this itself; a host
 * may also force it. TRUE maps to CefBrowserHost::SetAccessibilityState(
 * STATE_ENABLED), i.e. Chromium's Complete AT-SPI mode for a windowed browser;
 * FALSE returns to STATE_DEFAULT. CHROMIUM_A11Y=1 in the helper remains an
 * explicit startup override and suppresses the automatic watcher. */
void               chromium_session_set_accessibility (ChromiumSession *self,
                                                       gboolean enabled);

/* Developer tools. FALSE (the default) is the whole off switch: the context
 * menu has no 检查 row, F12 and Ctrl+Shift+I do nothing, and the helper never
 * calls ShowDevTools. TRUE adds those entries; the inspector opens in its own
 * associated browser, reported as ::browser-created with @windowed and
 * @devtools TRUE, which the host shows like any other standalone browser.
 * Closing that window, or closing the page it inspects, closes the other side
 * through CEF's own association.
 *
 * Enabling this hands whoever is at the keyboard full debugging power over
 * every page this session loads: arbitrary script in the page, and read access
 * to its storage and cookies. It is a local-user feature, not something to
 * turn on for content you do not trust. The value may be changed at any time:
 * a running helper is told immediately, and a later start() reads the value
 * that is set then.
 *
 * There is deliberately no programmatic show/close call in this version. */
void               chromium_session_set_devtools_enabled (ChromiumSession *self,
                                                          gboolean enabled);
gboolean           chromium_session_get_devtools_enabled (ChromiumSession *self);
/* Optional frame meter: the number of top-level frames the compositor has
 * presented since the session started. It is a counter, not a rate; sample it
 * from a timer if a host wants a graph. */
uint64_t           chromium_session_get_frame_count (ChromiumSession *self);
/* The same counters nested_compositor_get_commit_stats() exposes, for hosts
 * that only hold the session. Any out parameter may be NULL. */
void               chromium_session_get_commit_stats (ChromiumSession *self,
                                                      guint64 *shm_commits,
                                                      guint64 *dmabuf_commits,
                                                      guint64 *shm_bytes_copied,
                                                      guint64 *shm_bytes_wrapped);

/* One helper can host several browsers. The first one is started by
 * chromium_session_start(); these create further browsers in the same helper.
 * @windowed FALSE puts the browser in the shared tab window, TRUE creates a
 * standalone CefWindow. ::browser-created (id, url, windowed, devtools),
 * ::browser-selected (id) and ::browser-closed (id) report the lifecycle; CEF
 * assigns @id, so a host should key its own tab/window map from the signal.
 * @devtools TRUE marks the inspector browser a DevTools request created: it is
 * always windowed, and CEF owns its window (see
 * chromium_session_set_devtools_enabled()).
 *
 * @id is also what ::browser-selected names before ::browser-created does: the
 * helper can report a selection for a browser whose created event is still on
 * the way, so a host must be able to mark a tab it has not seen yet.
 *
 * chromium_session_close_browser () works for a tab and for a windowed
 * browser: the helper drops the slot's reference to the view before
 * CloseBrowser, so neither the window nor the slot is a co-owner when the close
 * runs — Alloy reads that as "close the browser" and OnBeforeClose fires, which
 * is what makes ::browser-closed land for the first tab too
 * (measured by test/multi-browser-gate.sh).
 * An unknown @id answers status: no-such-browser, which is how a host can tell
 * "already gone" from "nothing was asked".
 *
 * chromium_session_new_browser_with_geometry () is the windowed form that
 * opens the CEF window at a size and place — the window.open features a
 * host's own popup policy arrives with. @x/@y/@width/@height are logical
 * pixels; a @width or @height of 0 keeps the helper's default window bounds.
 * The window answers with ::window-properties (browser, x, y, w, h, toolbar,
 * menubar, status, resizable, fullscreen) as it is created and whenever its
 * bounds or fullscreen state change: this helper's windows are frameless, so
 * toolbar/menubar/status are always FALSE and @resizable TRUE — the fields
 * are the host's window.open picture, reported, not granted. */
void               chromium_session_new_browser   (ChromiumSession *self,
                                                   const char *url,
                                                   gboolean windowed);
void               chromium_session_new_browser_with_geometry
                                                  (ChromiumSession *self,
                                                   const char *url,
                                                   gboolean windowed,
                                                   int x, int y,
                                                   int width, int height);
/* A background page: the browser runs with no window and no surface, hidden
 * from Chromium's own point of view, so timers and network keep running but
 * nothing is drawn and no toplevel appears on the nested compositor. This is
 * the mechanism a plugin manifest's background.service_worker rides on; a
 * host may use it for its own always-on pages. Events that describe what the
 * host shows (address, title, loading, favicon) are not sent for it, and it
 * never becomes the active browser. ::browser-created reports it with the
 * background flag TRUE; close it with chromium_session_close_browser(). */
void               chromium_session_new_background_browser (ChromiumSession *self,
                                                            const char *url);
void               chromium_session_select_browser (ChromiumSession *self,
                                                    int id);
void               chromium_session_close_browser  (ChromiumSession *self,
                                                    int id);

/* The toplevel a browser's own CEF window draws into, or NULL while it has
 * none (before the surface exists, after it goes away, or for a tab — every
 * tab shares the one CEF window, which is the first toplevel). The pairing is
 * by creation order: the helper creates the shared tab window first and then
 * one CEF window per windowed browser, and test/surface-pair-gate.sh measures
 * that the compositor reports those toplevels in the same order, including
 * with DevTools windows and windows closed and reopened. ::browser-surface
 * reports each pairing as it is made and NULL when the surface goes away;
 * the returned surface belongs to the session, not to the caller. */
NestedSurface     *chromium_session_get_browser_surface (ChromiumSession *self,
                                                         int id);

/* The browser table, the session-side twin of the helper's slot list and the
 * answer to "what are my background tabs doing right now". The helper pushes
 * the whole table on every change — a browser created, closed or selected, a
 * title or address changing, a mute or zoom landing — and ::browsers-changed
 * says the copy below is new; the reads between changes come from the
 * session's cache, so a host polling background-tab state never has to select
 * anything first. DevTools and background browsers are not in the table: the
 * first is not a tab, the second shows nothing.
 *
 * The returned array is newly allocated and holds @length entries in the
 * helper's own order; free each row with
 * chromium_session_browser_info_free (). NULL (with @length 0) means no
 * helper is running or none has reported yet. */
typedef struct {
  int       id;
  char     *url;
  char     *title;
  gboolean  active;
  gboolean  windowed;
  gboolean  muted;
  double    zoom;      /* CEF zoom level, 0.0 = 100 % */
} ChromiumBrowserInfo;

ChromiumBrowserInfo *chromium_session_browsers_list (ChromiumSession *self,
                                                     gsize *length);
void               chromium_session_browser_info_free
                                                  (ChromiumBrowserInfo *info);

/* Navigation, zoom, find and view-source name the browser they mean: @browser
 * 0 is the active one (the convention the multi-browser commands already
 * keep), any other id is that browser — including a background tab, which is
 * the point. An unknown id answers status: no-such-browser on the channel and
 * nothing else; the navigation simply does not happen. */
void               chromium_session_load_uri      (ChromiumSession *self,
                                                   int browser,
                                                   const char *uri);
void               chromium_session_go_back       (ChromiumSession *self,
                                                   int browser);
void               chromium_session_go_forward    (ChromiumSession *self,
                                                   int browser);
void               chromium_session_reload        (ChromiumSession *self,
                                                   int browser,
                                                   gboolean hard);
void               chromium_session_stop_loading  (ChromiumSession *self,
                                                   int browser);
/* ::load-started (browser, url, transition, is_main_frame) is a navigation
 * that committed — main frame or not, with the @transition bitmask Chromium
 * assigned it and @is_main_frame to sort the two apart. It is the earliest
 * fact a navigation offers: CEF has no "about to start" callback that
 * survives a navigation which fails before commit, and a failed one never
 * reaches here at all. Fire-and-forget.
 *
 * ::load-end (browser, url, http_code) and ::load-error (browser, error_code,
 * url, message) close the navigation. The contract since the lifecycle landed:
 * exactly one ::load-error per failed navigation — Chromium's own net-error
 * code (negative) and its name ("ERR_NAME_NOT_RESOLVED") — followed by exactly
 * one ::load-end with HTTP code 0, and never the ERR_ABORTED echo Chromium
 * sends a second behind the real error. A successful navigation is a
 * ::load-end with the HTTP code and no error. ::renderer-exited (status,
 * error_code, message) is the render process dying: status 0 abnormal exit,
 * 1 killed, 2 crashed (SIGSEGV), 3 out of memory. Nothing else reports it —
 * the surface keeps the last frame and every later command goes unanswered,
 * so a page that stops moving is what it looks like from outside. */
/* Results arrive on the ::script-result signal, with the same @id:
 * (id, ok, value, message), where @value is the result as JSON text — "null"
 * for a script that returned nothing. @ok is TRUE only when the script ran
 * *and* its result was delivered: a result too large for the channel (the
 * helper's ceiling is 64 KiB) arrives as ok FALSE, value "null", and @message
 * naming the drop — never as a success that happens to have no value.
 * @message otherwise explains a failure or a result the page could not
 * serialise. A script that navigates the page away may never answer at all. */
void               chromium_session_run_javascript (ChromiumSession *self,
                                                   const char *code,
                                                   int id);

/* The CSP-safe form: code is embedded as an expression and wrapped without
 * eval(), for pages whose script-src forbids unsafe-eval. Use run_javascript()
 * for statement-shaped code; both answer through ::script-result. */
void               chromium_session_run_javascript_expression (ChromiumSession *self,
                                                               const char *code,
                                                               int id);

/* ::primary-selection (text) — the renderer half of CEF's missing primary
 * source. CEF on Wayland binds the primary-selection device but does not create a
 * source for a page selection, so the helper injects a selection listener and
 * pushes the selected text here. The session writes it to the view's GTK primary
 * clipboard, which the compositor's own primary bridge offers to clients. An
 * empty @text is the page clearing its selection. */

/* ::page-message (json) — the other direction of the same idea: the renderer
 * registers cefnestPostMessage(jsonString) in every context and the host gets the
 * string back whole. The schema inside is the host's; this channel only carries a
 * string. Going back, there is no dedicated command: the host reaches the page
 * with chromium_session_run_javascript () and a dispatched MessageEvent
 * (docs/control-channel.md).
 *
 * Unlike the js-result relay, neither side of this path puts a size limit in its
 * own code, so a big message is a full-length line on the channel and the host's
 * 4 MiB ceiling applies to it as to any other event — over the line the session
 * closes instead of truncating (docs/control-channel.md "通道读这一侧"). What a
 * very large payload does *inside* CEF's process-to-process message has never
 * been measured here, so treat delivery of an oversized one as unverified. */

/* ::popup-requested (url, disposition, gesture, browser) — a target=_blank,
 * window.open or middle-click that asks for a new window. @disposition is
 * Chromium's own numeric window-open disposition (1 = foreground tab,
 * 2 = background tab, 4 = new window among them), @gesture says whether a
 * user action asked, and @browser names the tab that opened it — a popup can
 * be requested from a background tab, and epiphany telling a middle-click
 * background tab from a target=_blank foreground one reads these three.
 * The helper has already acted: foreground requests navigate the asking
 * browser in place, a background tab is reported and left alone
 * (docs/control-channel.md). */

/* The page's sound leaves the helper through its own audio process and lands on
 * the host's speakers, so this is the only switch the
 * embedding app has. chromium_session_get_muted () reports what the helper read
 * back: it changes a round trip after the call, and moves ::changed with it.
 * A new helper always starts unmuted. */
void               chromium_session_set_muted     (ChromiumSession *self,
                                                   gboolean muted);

/* Page zoom. @action is "in", "out" or "reset"; "in"/"out" step one CEF zoom
 * level (0.5, which is one Chrome UI step), and "reset" returns to 100 %.
 * Chromium persists zoom per origin, so a navigation to an origin with a stored
 * level is what the page gets, not necessarily 0. The helper answers with
 * ::zoom-changed (level) after a zoom command and after each main-frame load,
 * and chromium_session_get_zoom_level () returns the last level the *active*
 * browser reported; a background tab's level arrives through the browsers
 * table instead. 0.0 is 100 %. */
void               chromium_session_zoom             (ChromiumSession *self,
                                                      int browser,
                                                      const char *action);
/* Set one explicit zoom level (0.0 = 100 %, 0.5 = one Chrome UI step). The
 * helper reads it back and answers with ::zoom-changed, exactly like the
 * relative commands. */
void               chromium_session_set_zoom_level   (ChromiumSession *self,
                                                      int browser,
                                                      double level);
double             chromium_session_get_zoom_level   (ChromiumSession *self);

/* Auto-resize: the page's own preferred content size drives the toplevel it is
 * shown in, clamped to the bounds below. Off by default. The helper arms CEF's
 * auto-resize notification on every live browser; when the page's preferred
 * size changes the session resizes the surface it has paired with that browser
 * and emits ::auto-resize-updated (browser, width, height), so a host that also
 * draws the window can follow. 0 for a maximum means "no limit"; a 0 minimum
 * means "no floor". The bounds are in logical pixels, the same units
 * chromium_session_new_browser() and the compositor use. */
void               chromium_session_set_auto_resize  (ChromiumSession *self,
                                                      gboolean enabled);
gboolean           chromium_session_get_auto_resize  (ChromiumSession *self);
void               chromium_session_set_auto_resize_bounds (ChromiumSession *self,
                                                            int min_width,
                                                            int min_height,
                                                            int max_width,
                                                            int max_height);
/* Downloads. Nothing lands on disk until the host says where: ::download-begin
 * (id, url, suggested_name, mime_type, total_bytes) is the question, and exactly
 * one of the two calls below has to answer it. A download that is never answered
 * stays parked and no file is written; @total_bytes is -1 when the server did not
 * state a length.
 *
 * chromium_session_accept_download () names the full path to write to, so the
 * directory — and therefore the whole of where a page's file can end up — is the
 * host's choice, never the page's. The helper does not create parent directories.
 * chromium_session_reject_download () refuses a pending download and cancels an
 * in-flight one with the same call.
 *
 * ::download-progress (id, received_bytes, total_bytes, percent, speed) follows
 * each whole percent; @speed is bytes/s. @percent is -1 for a download of unknown
 * length. ::download-end (id, state, path, received_bytes) closes it, with state
 * "complete", "cancelled" or "interrupted" and @path read back from Chromium —
 * a refused-before-starting download ends there with an empty path, and both the
 * accept and the reject routes finish here, so a host tracking rows has one
 * place to look. */
void               chromium_session_accept_download (ChromiumSession *self,
                                                   int id,
                                                   const char *path);
void               chromium_session_reject_download (ChromiumSession *self,
                                                   int id);
/* Pause/resume an in-flight download through CefDownloadItemCallback. A
 * download that has not started (no item callback yet) answers with status
 * "download-not-in-flight"; the terminal ::download-end still comes from CEF. */
void               chromium_session_pause_download  (ChromiumSession *self,
                                                     int id);
void               chromium_session_resume_download (ChromiumSession *self,
                                                     int id);

/* View Source. The context menu's "查看网页源代码" lands here and so can a
 * host: one request for the active browser's main frame source. The answer
 * is not this call but ::view-source (browser, url, source) — or
 * ::view-source-error (browser, message). @source is the DOM as Chromium
 * serialized it, script changes included; it is NOT the network's original
 * bytes, and hosts that care about the difference should say so in their own
 * UI. With the auto-source-window property on (the default) the session also
 * opens its own read-only source window; set it to FALSE to keep only the
 * signal and draw a window of your own. The @source string is only valid
 * during the signal emission — copy it to keep it. */
void               chromium_session_view_source   (ChromiumSession *self,
                                                   int browser);

/* Browser data and page extraction. Each call is answered by one signal keyed
 * by the @id the host passes in — the helper is free to take its time, and two
 * requests with different ids never mix.
 *
 * chromium_session_cookies_get () → ::cookies-result (id, ok, json), where the
 * JSON object carries `cookies` (objects of name, value, domain, path,
 * expires as Unix seconds — 0 for a session cookie — secure, httponly) and
 * `count`. chromium_session_cookies_set () takes the same array shape as JSON
 * text and answers with `count` and, when some were refused, `failed` (their
 * names); a partial refusal is @ok FALSE with the accepted ones already
 * stored. chromium_session_cookies_delete () with an @url deletes that URL's
 * cookies, and with a @name only that one, answering with `deleted`.
 *
 * Cookies come from the running helper's own request context: with a data
 * directory they survive a restart, and with the default in-memory profile
 * they last for the helper's lifetime, like Chromium's own incognito rule.
 *
 * chromium_session_page_text () streams the active frame's text: one
 * ::page-text-begin (id, browser, bytes, truncated), ordered
 * ::page-text-chunk (id, data) pieces cut on UTF-8 boundaries, and one
 * ::page-text-end (id, truncated). The helper caps the text at 4 MiB and says
 * so in @truncated. chromium_session_history () answers once with
 * ::history-result (id, json): `current_index` and `entries`, each an object
 * of index, url, title and current.
 *
 * chromium_session_print_pdf () prints the active page to PDF and streams it
 * back as ::pdf-begin (id, bytes, truncated), ::pdf-chunk (id, GBytes) with
 * the bytes already base64-decoded, and ::pdf-end (id, bytes, truncated,
 * error). The helper prints into its own ephemeral directory and removes the
 * file, so where the PDF ends up is the host's decision alone. The byte cap is
 * 16 MiB — ::pdf-begin's @truncated says the PDF was cut. The plain call uses
 * CEF's defaults; the _with_settings form takes landscape/background, scale,
 * page ranges and a lower cap (0 for the default; a cap below the PDF size is
 * how a host or a test asks for a truncated stream deliberately).
 *
 * ::favicon-changed (browser, urls) reports the icon URLs a page declared,
 * oldest declaration order preserved. Fire-and-forget: the helper downloads
 * nothing, and choosing an icon is the host's. */
typedef struct {
  gboolean    landscape;
  gboolean    print_background;
  double      scale;
  const char *page_ranges;
  int         max_bytes;
} ChromiumPdfSettings;

void               chromium_session_cookies_get    (ChromiumSession *self,
                                                     int id,
                                                     const char *url);
void               chromium_session_cookies_set    (ChromiumSession *self,
                                                     int id,
                                                     const char *url,
                                                     const char *cookies_json);
void               chromium_session_cookies_delete (ChromiumSession *self,
                                                     int id,
                                                     const char *url,
                                                     const char *name);
/* The browser a read names: @browser 0 is the active one, any other id is
 * that browser — a background tab's text, history or PDF is the point. */
void               chromium_session_page_text      (ChromiumSession *self,
                                                     int browser,
                                                     int id);
void               chromium_session_history        (ChromiumSession *self,
                                                     int browser,
                                                     int id);
/* The one history jump CEF's public API has no button for, implemented inside
 * the component over its own DevTools pipe — the host never enables cdp for
 * it and never sees the protocol. @index is the same number space
 * ::history-result reports: 0 is the oldest entry the helper still holds, and
 * `current_index` from the last ::history-result is where the page sits now.
 * There is no receipt: the jump lands as the usual ::address /
 * ::load-started / ::load-end run. */
void               chromium_session_history_go     (ChromiumSession *self,
                                                     int browser,
                                                     int index);
void               chromium_session_print_pdf      (ChromiumSession *self,
                                                     int browser,
                                                     int id);
void               chromium_session_print_pdf_with_settings
                                                   (ChromiumSession *self,
                                                     int browser,
                                                     int id,
                                                     const ChromiumPdfSettings *settings);

/* In-page search. chromium_session_find () starts a search or steps through the
 * one already running: the pattern goes over every time (next=TRUE means "the
 * next / previous match of this same search"), because the helper keeps no find
 * state and a step cannot be about a search the host has forgotten. The answer
 * is not this call but the signal: ::find-result (identifier, count,
 * active_ordinal, final). Measured against real Chromium:
 * every request takes a fresh identifier, so it groups the replies to one
 * request rather than counting searches; a request that finds matches answers
 * twice, a counting update and then the same numbers with @final TRUE; a request
 * that finds nothing answers once, count 0 with @final TRUE, because CEF 154 has
 * no separate no-match flag. A new search reports @active_ordinal 0 — nothing is
 * the active match yet — and a step reports which of the count it landed on.
 * chromium_session_stop_finding () ends the search and answers nothing;
 * @clear_selection=TRUE also drops the highlight Chromium put on the match. */
void               chromium_session_find          (ChromiumSession *self,
                                                   int browser,
                                                   const char *pattern,
                                                   gboolean forward,
                                                   gboolean match_case,
                                                   gboolean next);
void               chromium_session_stop_finding   (ChromiumSession *self,
                                                   int browser,
                                                   gboolean clear_selection);

/* The page's context menu. Chromium builds the model — every item it offers,
 * with the edit state, media and navigation already taken into account — and
 * this component draws it. ::context-menu (x, y, items) arrives first: @x and @y
 * are the click's own coordinates inside the page, in the same logical pixels the
 * pointer is fed in, and @items is a GList of ChromiumMenuItem owned by the
 * session for the duration of the emission only.
 *
 * A handler that draws the menu itself returns TRUE, and then it owns the answer:
 * exactly one of the two calls below has to follow. The answer is the handler's
 * own return value — a gboolean signal keeps the last handler's — not a parameter
 * it is handed. Returning FALSE — or connecting nothing at all — lets the session
 * pop up its own popover over the view, which is what a host that only wants a
 * working right-click menu gets for free. Either way the answer is the same two
 * calls, and they are also how a host that drew the menu hands the choice back.
 *
 * The helper holds one menu request at a time and Chromium will not open a second
 * until the first is answered, so a menu that is neither selected nor dismissed
 * costs every later right click as well.
 *
 * Item ids are CEF's own (100 back, 117 select all, 132 view source, …) plus the
 * helper's link/image/media rows (26501–26507). @type is "command", "check",
 * "radio", "separator", "submenu" or "none", and a separator carries no label.
 * The labels keep CEF's "(&B)" mnemonic spelling, which the session's menu turns
 * into a GTK mnemonic.
 *
 * Two actions are not answers to the menu itself: ::copy-text (text) asks the
 * host to put a copied link/image address on the system clipboard, and
 * ::open-url (url) asks it for a new window. The helper executes everything that
 * stays inside Chromium; these two are host policy. */
typedef struct {
  int          id;
  const char  *label;
  const char  *type;
  gboolean     enabled;
  gboolean     checked;
} ChromiumMenuItem;

void               chromium_session_context_menu_select (ChromiumSession *self,
                                                   int id);
void               chromium_session_context_menu_dismiss (ChromiumSession *self);

/* Permission requests. A page's ask for a microphone, a location or the clipboard
 * is a question CEF lets only the host answer, and leaving it unanswered costs
 * more than silence: the page's promise stays pending, and a second media ask
 * never arrives until the first is answered. See docs/control-channel.md for the
 * interaction shape.
 *
 * ::permission-request (id, kind, origin, permissions, raw) arrives first. @kind is
 * "media" for a getUserMedia ask and "prompt" for one of Chromium's permission
 * prompts, and @permissions is a GList of owned-by-the-session name strings
 * ("device-audio", "geolocation", "clipboard", "midi-sysex", …) — @raw is the
 * mask CEF was asked about, for a host that would rather match bits than names.
 *
 * A handler that shows its own prompt returns TRUE and owns the answer, exactly
 * as for ::context-menu, and chromium_session_permission_respond () is how it
 * sends it. Returning FALSE — or connecting nothing — gets a deny from the
 * session, which is what a page with no host at all sees today.
 *
 * A prompt still held when the page asks for the next permission is dismissed
 * under the host's feet: ::permission-dismissed (id, result) says so, with @result
 * "ignore" for that case, and answering an id this side no longer holds does
 * nothing. Media asks have no dismiss callback at all. */
typedef enum {
  CHROMIUM_PERMISSION_ACCEPT,
  CHROMIUM_PERMISSION_DENY,
  CHROMIUM_PERMISSION_DISMISS
} ChromiumPermissionResult;

void               chromium_session_permission_respond (ChromiumSession *self,
                                                   int id,
                                                   ChromiumPermissionResult result);

/* JavaScript dialogs. alert(), confirm(), prompt() and the beforeunload question
 * stop the page's script where they stand, and Alloy has no dialog UI of its own
 * that can be relied on in this process — with no handler a page parked on an
 * alert() never moves again. So every dialog is parked and reported:
 *
 * ::js-dialog (id, browser, kind, origin, message, default_value) arrives first.
 * @kind is "alert", "confirm", "prompt" or "beforeunload", and @default_value is
 * the prompt's initial text (empty for the others). A handler that draws its own
 * dialog returns TRUE and owns the answer, which it sends with
 * chromium_session_js_dialog_respond (); the value is used for prompts only. A
 * handler that returns FALSE — or a host that connects nothing — gets Chrome's
 * default: ok for alert and beforeunload, cancel with an empty value for confirm
 * and prompt. The default matters: it is the difference between a page that
 * continues and a page that hangs.
 *
 * ::js-dialog-closed (id, browser) says the dialog was taken away instead of
 * answered — the page navigated, reloaded, or its browser closed. A host panel
 * has to come down on it, and answering that id afterwards does nothing.
 *
 * Chromium holds one js dialog per browser: a second one requested while the
 * first is parked is suppressed by the helper, as the default CEF handler would.
 * The page stays blocked for as long as the request is parked, so a host that
 * shows a dialog must answer it (or close the browser) to let the page run. */

void               chromium_session_js_dialog_respond (ChromiumSession *self,
                                                   int id,
                                                   gboolean ok,
                                                   const char *value);

/* File choosers. A page's <input type="file">, a folder picker or a save prompt
 * is a question only a host can draw a chooser for:
 *
 * ::file-dialog (id, browser, mode, title, default_path, accept) arrives parked.
 * @mode is "open", "open-multiple", "open-folder" or "save"; @accept is a GList
 * of filter strings ("image/…", ".txt") owned by the session for the duration of
 * the emission. A handler that draws a chooser returns TRUE and answers with
 * chromium_session_file_dialog_respond (); an empty or NULL @paths list is the
 * cancel answer. Returning FALSE — or connecting nothing — cancels the chooser,
 * which is the only default that does not invent a path the page never asked for.
 *
 * ::file-dialog-closed (id, browser) says the browser took the request away
 * (it closed); nothing can be answered for that id any more. */

void               chromium_session_file_dialog_respond (ChromiumSession *self,
                                                   int id,
                                                   const char *const *paths);

/* Certificate errors. A certificate the platform does not trust stops the
 * navigation where it stands, and the decision is the host's:
 *
 * ::certificate-error (id, browser, error, url, subject, issuer, fingerprint)
 * arrives parked. @error is Chromium's net-error name ("ERR_CERT_AUTHORITY_INVALID",
 * …); @fingerprint is the SHA-256 of the leaf's DER as hex ("" when the
 * certificate could not be read). A handler that decides returns TRUE and
 * answers with chromium_session_certificate_respond (); returning FALSE — or
 * connecting nothing — denies, which is Chrome's own default and the safe
 * one: the navigation ends in the page's own certificate error.
 *
 * The decision repeats for every error. A host that wants "trust this
 * certificate for this run" takes the @fingerprint it saw and calls
 * chromium_session_certificate_authorize (); the helper then continues that
 * certificate without asking, and forgets it when the helper goes away —
 * persistence across restarts is the host's own policy store, not this
 * table's job.
 *
 * ::certificate-error-closed (id, browser) says the question went away
 * (the browser navigated or closed): a panel waiting on that @id comes down,
 * and answering it afterwards does nothing. */

void               chromium_session_certificate_respond (ChromiumSession *self,
                                                   int id,
                                                   gboolean allow);
void               chromium_session_certificate_authorize (ChromiumSession *self,
                                                   const char *fingerprint);

/* Forget every fingerprint chromium_session_certificate_authorize () put
 * into the helper's table; the next bad certificate asks again. The helper's
 * table is the only place the exemptions live, so this one call is the whole
 * undo. */
void               chromium_session_certificate_clear_authorized
                                                  (ChromiumSession *self);

/* A renderer that stopped answering input for 15+ seconds parks this
 * question:
 *
 * ::render-unresponsive (id, browser) arrives; the page's script is stuck,
 * its surface shows the last frame it painted. The host answers with
 * chromium_session_render_unresponsive_respond (): CHROMIUM_HANG_WAIT resets
 * the helper's timer and keeps waiting (the page may still recover —
 * ::render-responsive (id, browser) says it did, and the panel comes down),
 * CHROMIUM_HANG_TERMINATE kills the renderer, which surfaces as the usual
 * ::renderer-exited. Connecting nothing — or not answering — is CEF's own
 * keep-waiting: the page stays hung until the host or the person decides
 * something, which is the only safe default for a kill decision.
 *
 * One question per browser at a time; ::render-unresponsive-closed
 * (id, browser) says the browser went away with the question unanswered. */
typedef enum {
  CHROMIUM_HANG_WAIT,
  CHROMIUM_HANG_TERMINATE
} ChromiumHangAction;

void               chromium_session_render_unresponsive_respond
                                                  (ChromiumSession *self,
                                                   int id,
                                                   ChromiumHangAction action);

/* Resource interception, the consultative shape. With no rule registered the
 * feature does not exist: no handler, no callback, no overhead, and Web
 * plugins never see this API. Rules are the host's own ids and patterns:
 *
 * chromium_session_intercept_add () registers id → patterns (Chrome
 * match-pattern syntax, "scheme://host/path" with '*' wildcards, as
 * docs/control-channel.md defines it) with an optional list of resource type
 * names ("image", "xhr", "main_frame", …); an empty type list matches every
 * type. Re-adding an id replaces it. chromium_session_intercept_remove ()
 * takes it back out. Nothing is retained across helper restarts.
 *
 * Every matching request reports ::request-intercept (request_id, browser,
 * url, method, resource_type, is_main_frame, headers) and STOPS until the
 * host answers with chromium_session_request_respond (): ALLOW lets it
 * proceed, CANCEL kills it, REDIRECT swaps the URL first. There is no
 * timeout — the page waits for the host, so a host that registers rules owes
 * quick answers — and a browser that closes cancels what it left, reported
 * as ::request-intercept-closed.
 *
 * chromium_session_intercept_set_passive (TRUE) is the observe-only mode:
 * every event still arrives, nothing stops. */
typedef enum {
  CHROMIUM_REQUEST_ALLOW,
  CHROMIUM_REQUEST_CANCEL,
  CHROMIUM_REQUEST_REDIRECT
} ChromiumRequestAction;

void               chromium_session_intercept_add (ChromiumSession *self,
                                                   guint id,
                                                   const char *const *patterns,
                                                   const char *const *resource_types);
void               chromium_session_intercept_remove (ChromiumSession *self,
                                                   guint id);
void               chromium_session_intercept_set_passive (ChromiumSession *self,
                                                   gboolean passive);
void               chromium_session_request_respond (ChromiumSession *self,
                                                   int request_id,
                                                   ChromiumRequestAction action,
                                                   const char *url);

/* The DevTools protocol pipe. CDP is full control of a page — script,
 * storage, network, all of it — so the pipe is off until the host turns it
 * on, per browser, with chromium_session_cdp_enable (); until then every
 * execute is refused by name (cdp-not-enabled). @browser 0 means the active
 * browser.
 *
 * chromium_session_cdp_execute () sends one method call with the host's own
 * @id; the answer arrives as ::cdp-result (id, ok, json, message) — the same
 * @id, the result or error dictionary as one JSON text, and @message set to
 * "cdp-too-large" when the answer could not fit the channel's line limit.
 * ::cdp-event (browser, method, params, message) carries every protocol
 * event the browser emits while the observer is on. The helper is a separate
 * process: nothing here validates @params_json beyond JSON itself, and the
 * protocol's own errors come back in the result like any other answer. */
void               chromium_session_cdp_enable (ChromiumSession *self,
                                                   int browser);
void               chromium_session_cdp_disable (ChromiumSession *self,
                                                   int browser);
void               chromium_session_cdp_execute (ChromiumSession *self,
                                                   int id, int browser,
                                                   const char *method,
                                                   const char *params_json);

/* HTTP authentication. A 401 (or a proxy's 407) stops the request where it
 * stands and only the host can answer it:
 *
 * ::auth-required (id, browser, is_proxy, scheme, realm, origin) arrives
 * parked. @scheme is "basic", "digest", … and @realm the challenge's realm;
 * @origin is the URL that asked. A handler that draws its own prompt returns
 * %TRUE and owes exactly one of the two calls below. Returning %FALSE — or
 * connecting nothing — cancels, which is Chrome's own default: the request
 * fails with its 401 and the page shows what a browser without the password
 * would show.
 *
 * ::auth-closed (id, browser) says the question went away — the browser
 * navigated, closed, or answered the challenge itself — and answering that
 * @id afterwards does nothing. Unlike a certificate error, the same origin
 * re-asks per request: a host that wants to remember takes the credentials
 * into its own store. */
void               chromium_session_auth_respond (ChromiumSession *self,
                                                   int id,
                                                   const char *user,
                                                   const char *password);
void               chromium_session_auth_cancel  (ChromiumSession *self,
                                                   int id);

/* Client certificate selection. An HTTPS server (or proxy) that asks for a
 * client certificate parks this question:
 *
 * ::client-cert-request (id, browser, origin, certs) — @origin is
 * "host:port", @certs a GList of ChromiumClientCert (owned by the session for
 * the duration of the emission) already pruned by Chromium to the issuers the
 * server trusts. The host answers by index with
 * chromium_session_client_cert_respond (); -1 (and the default, when no
 * handler returns TRUE) means "continue without a certificate", which is the
 * Chrome answer and the only one that cannot leak a private key nobody asked
 * the user about. */
typedef struct {
  char *subject;
  char *issuer;
  char *fingerprint;   /* SHA-256 of the DER, hex; "" when unreadable */
} ChromiumClientCert;

/* One certificate out of a ::client-cert-request list, and the list itself
 * (a host that kept the list past the emission owns these frees). */
void               chromium_client_cert_free       (ChromiumClientCert *cert);
void               chromium_client_cert_list_free  (GList *certs);

void               chromium_session_client_cert_respond
                                                  (ChromiumSession *self,
                                                   int id,
                                                   int index);

/* The certificate facts about a *successful* page — the address-bar lock's
 * other half. Error paths have ::certificate-error; a normal https page has
 * this one read. The answer is one ::tls-info (id, browser, ok,
 * security_state, subject, issuer, fingerprint, valid_from, valid_to):
 * @security_state is Chromium's own word ("secure", "insecure", …), the
 * certificate fields describe the leaf ("" when the state carries none —
 * plain http, for one), @fingerprint is the SHA-256 of the DER as hex, and
 * @valid_from/@valid_to are Unix seconds. @ok FALSE means the read could not
 * be made (no browser, no page); the fields are then all "". A page that is
 * still loading answers with what it has at the moment of the call.
 * @browser 0 is the active browser. */
void               chromium_session_get_tls_info  (ChromiumSession *self,
                                                   int id,
                                                   int browser);

/* Host-served schemes. epiphany's ephy-about:, ephy-reader: and friends are
 * URLs Chromium would otherwise refuse; this registers the scheme with
 * Chromium *and* names the host as its data source:
 *
 * chromium_session_scheme_register () declares the scheme — the flags are
 * CEF's scheme options, and they are read by the next start() (the registrar
 * CEF hands out exists only at process start, exactly like proxy and
 * accept-language); the data factory half is attached to a running helper
 * immediately and re-attached on every ::ready. With no flags set the scheme
 * is standard + secure + fetchable, the shape an about-style page wants;
 * add CHROMIUM_SCHEME_DISPLAY_ISOLATED to keep other origins from reading it
 * (the about-page security model) and CHROMIUM_SCHEME_CORS to allow cross-
 * origin reads. The name must match [a-z][a-z0-9+.-]* and may not override a
 * built-in scheme or cefnest-extension.
 *
 * Every request to a registered scheme then parks:
 *   ::scheme-request (id, browser, url, method, headers)  @headers is one
 *     flat JSON object text; a handler returns TRUE and answers with
 *     chromium_session_scheme_response () (status, reason, content_type and
 *     optional response headers in the same flat shape), then streams the
 *     body with chromium_session_scheme_write () — 48 KiB pieces or less —
 *     and closes with chromium_session_scheme_end (). Returning FALSE — or
 *     connecting nothing — aborts the request: the component does not invent
 *     page data. chromium_session_scheme_abort () takes a request back before
 *     the host has answered. ::scheme-closed (id, browser) says the browser
 *     went away mid-request.
 *
 * Nothing is retained across helper restarts except the registration itself,
 * which rides start() like the rest of the network identity. */
typedef enum {
  CHROMIUM_SCHEME_LOCAL            = 1 << 0,
  CHROMIUM_SCHEME_DISPLAY_ISOLATED = 1 << 1,
  CHROMIUM_SCHEME_SECURE           = 1 << 2,
  CHROMIUM_SCHEME_CORS             = 1 << 3,
  CHROMIUM_SCHEME_FETCH            = 1 << 4,
} ChromiumSchemeFlags;

void               chromium_session_scheme_register   (ChromiumSession *self,
                                                       const char *name,
                                                       guint flags);
void               chromium_session_scheme_unregister (ChromiumSession *self,
                                                       const char *name);
void               chromium_session_scheme_response   (ChromiumSession *self,
                                                       int id,
                                                       int status,
                                                       const char *reason,
                                                       const char *content_type,
                                                       const char *headers_json);
void               chromium_session_scheme_write      (ChromiumSession *self,
                                                       int id,
                                                       GBytes *data);
void               chromium_session_scheme_end        (ChromiumSession *self,
                                                       int id);
void               chromium_session_scheme_abort      (ChromiumSession *self,
                                                       int id);

/* Host scripts — the user-script half, the component-side twin of a plugin's
 * content scripts. chromium_session_script_add () registers @id → @code with
 * @patterns (Chrome match-pattern syntax) and a run time ("document_start",
 * "document_end", "document_idle"; the default is "document_idle"). @browser
 * 0 means every browser of this session, any other id only that one;
 * @all_frames widens the run from main frames to every frame matching.
 * Re-adding an id replaces it; chromium_session_script_remove () takes it
 * back out. Nothing is retained across helper restarts.
 *
 * document_start runs before the page's own inline scripts (the renderer-side
 * path the plugin content scripts measured); document_end and document_idle
 * both land at main-frame load end. There is no isolated world: the script
 * runs in the page's own JavaScript domain and can be read, broken or
 * overwritten by the page — that is the point of a user script, and the
 * boundary a host must not pretend is harder than it is. */
void               chromium_session_script_add    (ChromiumSession *self,
                                                   guint id,
                                                   int browser,
                                                   const char *const *patterns,
                                                   const char *code,
                                                   const char *run_at,
                                                   gboolean all_frames);
void               chromium_session_script_remove (ChromiumSession *self,
                                                   guint id);

/* The user agent, as a property instead of an environment variable. The
 * global form covers every browser of this session, current and future; the
 * per-browser form overrides one browser. An empty string is "Chromium's
 * own". The component applies it over its own DevTools pipe — the host never
 * enables cdp for it — and re-applies it after a renderer crash, so the
 * switcher a UA-swap env var used to require a restart for is now a live
 * property. Reading back returns the global form only. */
void               chromium_session_set_user_agent (ChromiumSession *self,
                                                    const char *user_agent);
const char        *chromium_session_get_user_agent (ChromiumSession *self);
void               chromium_session_browser_set_user_agent
                                                  (ChromiumSession *self,
                                                   int browser,
                                                   const char *user_agent);

/* A screenshot of one browser's page as PNG. The answer is one
 * ::screenshot-result (id, browser, ok, png, message): @png holds the bytes
 * on success (@ok TRUE) and is NULL otherwise, with @message saying why —
 * including "screenshot-too-large", the channel's line ceiling. @browser 0 is
 * the active browser. The image is the page as Chromium composes it, so a
 * snapshot-service thumbnail does not need the compositor's texture readback
 * to get one. */
void               chromium_session_capture_screenshot (ChromiumSession *self,
                                                        int id,
                                                        int browser);

/* Typed data clearing, the "清除数据" dialog's granularity. @types is a
 * bitmask of the values below; @origins (used only with
 * CHROMIUM_CLEAR_ORIGIN_STORAGE) is a list of origins ("https://example.com")
 * whose storage to forget — an empty list clears nothing for that type. Cache,
 * cookies and the HTTP auth cache are profile-wide facts, not per-tab ones.
 * This runs over the component's own pipe and is fire-and-forget: a host
 * confirms with its own reads (cookies-get, a refetched page). It coexists
 * with chromium_session_clear_data (), which is the whole-directory form. */
typedef enum {
  CHROMIUM_CLEAR_CACHE            = 1 << 0,
  CHROMIUM_CLEAR_COOKIES          = 1 << 1,
  CHROMIUM_CLEAR_HTTP_AUTH_CACHE  = 1 << 2,
  CHROMIUM_CLEAR_ORIGIN_STORAGE   = 1 << 3,
} ChromiumClearDataType;

void               chromium_session_clear_data_types (ChromiumSession *self,
                                                      guint types,
                                                      const char *const *origins);

/* The control-channel version the running helper reported in ::ready — the
 * major half of the handshake. 0 means no helper is running or none has
 * reported. The session refuses a helper whose major differs from its own
 * (the whole protocol has moved under the host); the minor is informational,
 * which is what a host that only wants to log the mismatch reads. */
int                chromium_session_get_helper_version (ChromiumSession *self);

/* ::loading-progress (progress) reports a load in flight as a number between 0
 * and 1, for a host that draws its own progress bar. Fire-and-forget: the next
 * value supersedes this one.
 *
 * ::fullscreen-changed (fullscreen) reports a page entering or leaving
 * fullscreen. The helper already moves its CEF window to match; a host that owns
 * the toplevel can follow here. */

/* ::closed (reason) is the helper being gone for good — it never comes back on
 * this session, and a new page needs chromium_session_start () again. It goes
 * out once per start, and never for a session that was stillborn or is already
 * being disposed, so a host that tears a toolbar down here has exactly one
 * notification to react to.
 *
 * @reason says who ended it, and is one of these four, in the caller's own
 * words: "stopped by the host" (chromium_session_stop () was called, which is
 * what closing the window does), "helper closed the browser" (the helper said
 * so over the channel — CEF's own CloseWindow / quit), "control channel closed"
 * (the socket hung up), "process exited" (the child was reaped). The last two
 * are the same death seen from two sides and whichever arrives first reports
 * it, so a host should treat every reason but its own as "the page is gone,
 * and I did not ask for that" rather than branch on the difference.
 *
 * ::helper-exited () follows once the helper process itself has been reaped —
 * not when it announced it was leaving, but when the wait has completed and no
 * process is left. The gap matters to a host that owns the display: a helper
 * stopped by SIGTERM/SIGHUP/SIGINT says {"event":"closed"} and then spends
 * CefShutdown() still reading the Wayland connection its CefWindow was given,
 * so a host that destroys the nested compositor on ::closed makes the helper's
 * GDK end the process with "Lost connection to Wayland compositor" before the
 * tail of main() runs. Destroy the display here instead.
 *
 * Emitted once for the helper that was still this session's child when its
 * wait completed, after ::closed and after the exit status has been logged.
 * Not emitted when the session is disposed before its child is reaped — the
 * host has already dropped the session — nor for a child that a ::closed
 * handler replaced with a fresh chromium_session_start (), since the new
 * helper has its own exit to report. chromium_session_stop () still reaches
 * it, because stopping force-exits the child unless the helper was already on
 * its way out. */

const char        *chromium_session_get_helper_path (ChromiumSession *self);
const char        *chromium_session_get_uri         (ChromiumSession *self);
const char        *chromium_session_get_title       (ChromiumSession *self);
gboolean          chromium_session_get_loading      (ChromiumSession *self);
gboolean          chromium_session_get_can_go_back  (ChromiumSession *self);
gboolean          chromium_session_get_can_go_forward (ChromiumSession *self);
gboolean          chromium_session_get_muted      (ChromiumSession *self);
gboolean          chromium_session_get_running      (ChromiumSession *self);

/* The plugin manager this session drives: the manifest scan, the permission
 * table, and — through the native frontend — the .so plugins enabled on it.
 * Created on the first call and owned by the session; a host that never calls
 * this runs no plugin code at all.
 *
 * The manager's install root is anchored on this session's data directory,
 * not on this component's name: with a data directory set, scan() also reads
 * <data-dir>/cefnest-plugins (before any host-added search path, so the
 * user's own install wins a duplicate id), and the directory is allowed not
 * to exist yet — an empty profile has an empty plugin list. With no data
 * directory there is no default root at all: nothing on disk, which is the
 * same contract the data directory itself keeps. Hosts that want plugins
 * from somewhere else add their own paths with add_search_path(). Moving the
 * data-directory property moves the root; the model refreshes on the next
 * scan().
 *
 * Enabling a plugin here reaches a running helper immediately (its script
 * table and scheme root go over as one plugins-load command, its menu rows as
 * menu-add-user-item), and a helper started later is told at ::ready. The
 * manager's own signals — ::changed, ::grant-request, ::plugin-error — are
 * where the management UI lives; this call only wires the model to the
 * channel. What enable() does *not* do is grant anything: that is the host's
 * answer to ::grant-request, and an unanswered request is a deny. */
ChromiumPluginManager *chromium_session_get_plugin_manager (ChromiumSession *self);

G_END_DECLS
