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
void               chromium_session_run_javascript (ChromiumSession *self,
                                                   const char *code,
                                                   int id);

const char        *chromium_session_get_helper_path (ChromiumSession *self);
const char        *chromium_session_get_uri         (ChromiumSession *self);
const char        *chromium_session_get_title       (ChromiumSession *self);
gboolean          chromium_session_get_loading      (ChromiumSession *self);
gboolean          chromium_session_get_can_go_back  (ChromiumSession *self);
gboolean          chromium_session_get_can_go_forward (ChromiumSession *self);
gboolean          chromium_session_get_running      (ChromiumSession *self);

G_END_DECLS
