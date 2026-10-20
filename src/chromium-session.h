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

/* In-page search. chromium_session_find () starts a search or steps through the
 * one already running: the pattern goes over every time (next=TRUE means "the
 * next / previous match of this same search"), because the helper keeps no find
 * state and a step cannot be about a search the host has forgotten. The answer
 * is not this call but the signal: ::find-result (identifier, count,
 * active_ordinal, final). Measured against real Chromium (docs/find-path.md):
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

const char        *chromium_session_get_helper_path (ChromiumSession *self);
const char        *chromium_session_get_uri         (ChromiumSession *self);
const char        *chromium_session_get_title       (ChromiumSession *self);
gboolean          chromium_session_get_loading      (ChromiumSession *self);
gboolean          chromium_session_get_can_go_back  (ChromiumSession *self);
gboolean          chromium_session_get_can_go_forward (ChromiumSession *self);
gboolean          chromium_session_get_muted      (ChromiumSession *self);
gboolean          chromium_session_get_running      (ChromiumSession *self);

G_END_DECLS
