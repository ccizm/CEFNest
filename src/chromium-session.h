#pragma once

#include <gtk/gtk.h>

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

/* Runtime accessibility. The session watches org.a11y.Status's
 * ScreenReaderEnabled property on the session bus and calls this itself; a host
 * may also force it. TRUE maps to CefBrowserHost::SetAccessibilityState(
 * STATE_ENABLED), i.e. Chromium's Complete AT-SPI mode for a windowed browser;
 * FALSE returns to STATE_DEFAULT. CHROMIUM_A11Y=1 in the helper remains an
 * explicit startup override and suppresses the automatic watcher. */
void               chromium_session_set_accessibility (ChromiumSession *self,
                                                       gboolean enabled);
/* Optional frame meter: the number of top-level frames the compositor has
 * presented since the session started. It is a counter, not a rate; sample it
 * from a timer if a host wants a graph. */
uint64_t           chromium_session_get_frame_count (ChromiumSession *self);

/* One helper can host several browsers. The first one is started by
 * chromium_session_start(); these create further browsers in the same helper.
 * @windowed FALSE puts the browser in the shared tab window, TRUE creates a
 * standalone CefWindow. ::browser-created (id, url, windowed),
 * ::browser-selected (id) and ::browser-closed (id) report the lifecycle; CEF
 * assigns @id, so a host should key its own tab/window map from the signal.
 *
 * @id is also what ::browser-selected names before ::browser-created does: the
 * helper can report a selection for a browser whose created event is still on
 * the way, so a host must be able to mark a tab it has not seen yet.
 *
 * chromium_session_close_browser () is only effective for a windowed browser
 * today. For a tab the helper detaches the view and calls CloseBrowser, and
 * nothing comes back: no OnBeforeClose, so no ::browser-closed either
 * (code-review-2026-10-22.md M22). An unknown @id answers
 * status: no-such-browser, which is how a host can tell "already gone" from
 * "silently refused". */
void               chromium_session_new_browser   (ChromiumSession *self,
                                                   const char *url,
                                                   gboolean windowed);
void               chromium_session_select_browser (ChromiumSession *self,
                                                    int id);
void               chromium_session_close_browser  (ChromiumSession *self,
                                                    int id);

void               chromium_session_load_uri      (ChromiumSession *self,
                                                   const char *uri);
void               chromium_session_go_back       (ChromiumSession *self);
void               chromium_session_go_forward    (ChromiumSession *self);
void               chromium_session_reload        (ChromiumSession *self,
                                                   gboolean hard);
void               chromium_session_stop_loading  (ChromiumSession *self);
/* ::load-error (error_code, url, message) is a navigation that never committed:
 * Chromium's own net error code (negative) and its name ("ERR_NAME_NOT_RESOLVED").
 * ::load-end still fires for the same navigation with HTTP code 0, so a host that
 * needs to tell "loaded" from "did not load" has to listen here — and one failure
 * reports twice, the real code first and then ERR_ABORTED (-3) about a second
 * later. Both are passed through as they arrive, unfiltered.
 *
 * ::renderer-exited (status, error_code, message) is the render process dying:
 * status 0 abnormal exit, 1 killed, 2 crashed (SIGSEGV), 3 out of memory. Nothing
 * else reports it — the surface keeps the last frame and every later command goes
 * unanswered, so a page that stops moving is what it looks like from outside. */
/* Results arrive on the ::script-result signal, with the same @id:
 * (id, ok, value, message), where @value is the result as JSON text — "null"
 * for a script that returned nothing. @message explains a failure, a result the
 * page could not serialise, or a result too large for the channel. A script
 * that navigates the page away may never answer at all. */
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
 * (docs/guides/control-channel.md).
 *
 * Unlike the js-result relay, neither side of this path puts a size limit in its
 * own code, so a big message is a full-length line on the channel and the host's
 * 4 MiB ceiling applies to it as to any other event — over the line the session
 * closes instead of truncating (docs/guides/control-channel.md "通道读这一侧"). What a
 * very large payload does *inside* CEF's process-to-process message has never
 * been measured here, so treat delivery of an oversized one as unverified. */

/* ::popup-requested (url) — a target=_blank, window.open or middle-click that
 * asks for a new window. Only the URL is in the signal: the disposition and the
 * gesture stay on the socket event, so the host cannot tell from here whether the
 * page wanted a background tab or a foreground window, and the helper has already
 * acted — foreground requests navigate this view in place, a background tab is
 * reported and left alone (docs/guides/control-channel.md). */

/* The page's sound leaves the helper through its own audio process and lands on
 * the host's speakers (docs/features/audio-path.md), so this is the only switch the
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
 * and chromium_session_get_zoom_level () returns the last level it reported.
 * 0.0 is 100 %. */
void               chromium_session_zoom             (ChromiumSession *self,
                                                      const char *action);
double             chromium_session_get_zoom_level   (ChromiumSession *self);
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

/* In-page search. chromium_session_find () starts a search or steps through the
 * one already running: the pattern goes over every time (next=TRUE means "the
 * next / previous match of this same search"), because the helper keeps no find
 * state and a step cannot be about a search the host has forgotten. The answer
 * is not this call but the signal: ::find-result (identifier, count,
 * active_ordinal, final). Measured against real Chromium (docs/features/find-path.md):
 * every request takes a fresh identifier, so it groups the replies to one
 * request rather than counting searches; a request that finds matches answers
 * twice, a counting update and then the same numbers with @final TRUE; a request
 * that finds nothing answers once, count 0 with @final TRUE, because CEF 154 has
 * no separate no-match flag. A new search reports @active_ordinal 0 — nothing is
 * the active match yet — and a step reports which of the count it landed on.
 * chromium_session_stop_finding () ends the search and answers nothing;
 * @clear_selection=TRUE also drops the highlight Chromium put on the match. */
void               chromium_session_find          (ChromiumSession *self,
                                                   const char *pattern,
                                                   gboolean forward,
                                                   gboolean match_case,
                                                   gboolean next);
void               chromium_session_stop_finding   (ChromiumSession *self,
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
 * never arrives until the first is answered (docs/features/permission-prompt.md has the
 * measurements).
 *
 * ::permission-request (id, kind, origin, permissions) arrives first. @kind is
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
 * and I did not ask for that" rather than branch on the difference. */

const char        *chromium_session_get_helper_path (ChromiumSession *self);
const char        *chromium_session_get_uri         (ChromiumSession *self);
const char        *chromium_session_get_title       (ChromiumSession *self);
gboolean          chromium_session_get_loading      (ChromiumSession *self);
gboolean          chromium_session_get_can_go_back  (ChromiumSession *self);
gboolean          chromium_session_get_can_go_forward (ChromiumSession *self);
gboolean          chromium_session_get_muted      (ChromiumSession *self);
gboolean          chromium_session_get_running      (ChromiumSession *self);

G_END_DECLS
