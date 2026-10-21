#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* A host is three objects: a window, the view that owns the nested compositor,
 * and the session that owns the Chromium helper. The view must be in the window
 * before the session starts; the helper takes its WAYLAND_DISPLAY from the
 * view's compositor. */
static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (CHROMIUM_SESSION (data));
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *window = gtk_application_window_new (app);
  ChromiumView *view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  ChromiumSession *session = chromium_session_new (NULL);
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  chromium_session_set_view (session, view);
  gtk_window_set_title (GTK_WINDOW (window), "CEFNest minimal");
  gtk_window_set_default_size (GTK_WINDOW (window), 1000, 700);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));

  if (!chromium_session_start (session, url && *url ? url : "https://example.com",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  /* The session is owned by the window, and stops when the window closes. */
  g_object_set_data_full (G_OBJECT (window), "session", session, g_object_unref);
  g_signal_connect_swapped (window, "close-request",
                            G_CALLBACK (on_close), session);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.minimal",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
