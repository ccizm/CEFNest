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

/* The page's sound leaves the helper through its own audio process and lands on
 * the host's speakers (docs/audio-path.md), so this is the only switch the
 * embedding app has. chromium_session_get_muted () reports what the helper read
 * back: it changes a round trip after the call, and moves ::changed with it.
 * A new helper always starts unmuted. */
void               chromium_session_set_muted     (ChromiumSession *self,
                                                   gboolean muted);
const char        *chromium_session_get_helper_path (ChromiumSession *self);
const char        *chromium_session_get_uri         (ChromiumSession *self);
const char        *chromium_session_get_title       (ChromiumSession *self);
gboolean          chromium_session_get_loading      (ChromiumSession *self);
gboolean          chromium_session_get_can_go_back  (ChromiumSession *self);
gboolean          chromium_session_get_can_go_forward (ChromiumSession *self);
gboolean          chromium_session_get_muted      (ChromiumSession *self);
gboolean          chromium_session_get_running      (ChromiumSession *self);

G_END_DECLS
