#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Resilience. Three signals say the page is not what it was, and each wants a
 * different reaction:
 *
 *   ::load-error       a navigation that never committed — Chromium's net error
 *                      code is negative, and the same failure reports twice:
 *                      the real code first, then ERR_ABORTED (-3) about a
 *                      second later. Filter the second.
 *   ::renderer-exited  the render process died: status 0 abnormal exit, 1
 *                      killed, 2 crashed (SIGSEGV), 3 out of memory. The
 *                      surface keeps its last frame and later commands go
 *                      unanswered — "the page stopped moving" is the symptom.
 *                      CEF restarts the renderer on the next navigation, so a
 *                      reload button is a working recovery.
 *   ::closed           the helper is gone for good; a new page needs
 *                      chromium_session_start() again. The reason is host
 *                      ("stopped by the host") or the page's death seen from
 *                      either side of the socket.
 *
 * The demo puts a banner over the view for each case, with a reload button.
 *
 *   ./builddir/examples/cefnest-errors [url]
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *banner;     /* GtkRevealer holding the error box */
  GtkWidget *message;
  GtkWidget *reload;     /* the button the banner offers */
  ChromiumSession *session;
  char url[2048];
} Errors;

static Errors errors;

static void
show_banner (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;

  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  gtk_label_set_text (GTK_LABEL (errors.message), text);
  gtk_widget_set_visible (errors.reload, TRUE);
  gtk_revealer_set_reveal_child (GTK_REVEALER (errors.banner), TRUE);
}

static void
hide_banner (void)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (errors.banner), FALSE);
}

static void
session_load_end (ChromiumSession *session, const char *uri, int http_code,
                  gpointer data)
{
  /* A good commit clears the banner. http_code 0 also lands here — that is
   * "never committed", and ::load-error below is the one saying why. */
  if (http_code != 0)
    hide_banner ();
}

static void
session_load_error (ChromiumSession *session, int error_code, const char *uri,
                    const char *message, gpointer data)
{
  /* The double report is measured behaviour: the real code first, then
   * ERR_ABORTED (-3) about a second later as the failed navigation is
   * cleaned up. */
  if (error_code == -3)
    return;
  show_banner ("did not load: %s (%d)", message ? message : "unknown",
               error_code);
}

static void
session_renderer_exited (ChromiumSession *session, int status,
                         int error_code, const char *message, gpointer data)
{
  show_banner ("the render process is gone (status %d: %s) — reload to come "
               "back", status,
               status == 2 ? "SIGSEGV" : status == 3 ? "out of memory" :
               status == 1 ? "killed" : "abnormal exit");
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  show_banner ("the helper is gone: %s — press reload to start a new one",
               reason ? reason : "no reason given");
}

static void
on_reload (GtkButton *button, gpointer data)
{
  GError *error = NULL;

  hide_banner ();
  if (chromium_session_get_running (errors.session))
    {
      chromium_session_reload (errors.session, 0, TRUE);   /* hard: skip caches */
      return;
    }

  /* After ::closed the session object is reusable: start() again. */
  if (!chromium_session_start (errors.session, errors.url, &error))
    {
      g_warning ("cannot restart the helper: %s", error->message);
      g_clear_error (&error);
      show_banner ("restart failed — see stderr");
    }
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (errors.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *banner_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay;
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");
  const char *arg = data && ((char **) data)[0] ? ((char **) data)[0] : NULL;

  errors.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (errors.window), "CEFNest errors");
  gtk_window_set_default_size (GTK_WINDOW (errors.window), 1000, 700);

  overlay = gtk_overlay_new ();
  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), GTK_WIDGET (view));

  errors.banner = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (errors.banner),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_widget_set_valign (errors.banner, GTK_ALIGN_START);
  errors.message = gtk_label_new ("");
  gtk_label_set_wrap (GTK_LABEL (errors.message), TRUE);
  gtk_widget_set_hexpand (errors.message, TRUE);
  errors.reload = gtk_button_new_with_label ("Reload");
  g_signal_connect (errors.reload, "clicked", G_CALLBACK (on_reload), NULL);
  gtk_box_append (GTK_BOX (banner_box), errors.message);
  gtk_box_append (GTK_BOX (banner_box), errors.reload);
  gtk_widget_add_css_class (banner_box, "error");
  gtk_widget_set_margin_start (banner_box, 8);
  gtk_widget_set_margin_end (banner_box, 8);
  gtk_widget_set_margin_top (banner_box, 8);
  gtk_revealer_set_child (GTK_REVEALER (errors.banner), banner_box);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), errors.banner);

  gtk_box_append (GTK_BOX (root), overlay);
  gtk_window_set_child (GTK_WINDOW (errors.window), root);

  errors.session = chromium_session_new (NULL);
  chromium_session_set_view (errors.session, view);

  g_signal_connect (errors.session, "load-end",
                    G_CALLBACK (session_load_end), NULL);
  g_signal_connect (errors.session, "load-error",
                    G_CALLBACK (session_load_error), NULL);
  g_signal_connect (errors.session, "renderer-exited",
                    G_CALLBACK (session_renderer_exited), NULL);
  g_signal_connect (errors.session, "closed", G_CALLBACK (session_closed),
                    NULL);
  g_signal_connect (errors.window, "close-request", G_CALLBACK (on_close),
                    NULL);

  gtk_window_present (GTK_WINDOW (errors.window));

  g_strlcpy (errors.url,
             (arg && *arg) ? arg :
             (url && *url) ? url : "https://example.invalid/no-such-host",
             sizeof (errors.url));
  if (!chromium_session_start (errors.session, errors.url, &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (errors.window), "session",
                          g_object_ref (errors.session), g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.errors",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate),
                    argc > 1 ? argv + 1 : NULL);
  status = g_application_run (G_APPLICATION (app), 0, NULL);
  g_object_unref (app);
  return status;
}
