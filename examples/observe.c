#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Observability. Two knobs a host reaches for:
 *
 *   chromium_session_get_frame_count() — how many top-level frames the
 *     compositor has presented since start. It is a counter, not a rate: the
 *     host samples it on a timer and diffs, which is also how "the page froze"
 *     becomes measurable — the counter stops moving while the process lives.
 *   chromium_session_set_accessibility() — bridges the page's accessibility
 *     tree into AT-SPI. The session already watches org.a11y.Status and turns
 *     it on by itself when a screen reader appears; this is the manual
 *     override. It is not free, which is why the default is off.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-observe
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *fps;      /* frames/s, diffed from the counter */
  GtkWidget *total;    /* the raw counter */
  ChromiumView *view;
  ChromiumSession *session;
  guint timer;
  uint64_t last;
} Observe;

static Observe observe;

static gboolean
tick (gpointer data)
{
  uint64_t now = chromium_session_get_frame_count (observe.session);
  uint64_t rate = now >= observe.last ? now - observe.last : 0;

  observe.last = now;

  if (observe.fps)
    {
      g_autofree char *text =
        g_strdup_printf ("%" G_GUINT64_FORMAT " fps", rate);
      gtk_label_set_text (GTK_LABEL (observe.fps), text);
    }
  if (observe.total)
    {
      g_autofree char *text =
        g_strdup_printf ("frames: %" G_GUINT64_FORMAT, now);
      gtk_label_set_text (GTK_LABEL (observe.total), text);
    }
  return G_SOURCE_CONTINUE;
}

static void
on_a11y_toggled (GtkToggleButton *button, gpointer data)
{
  chromium_session_set_accessibility (observe.session,
                                      gtk_toggle_button_get_active (button));
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (observe.session);
  return FALSE;
}

static void
on_window_destroy (GtkWindow *window, gpointer data)
{
  observe.window = NULL;
  observe.fps = NULL;
  observe.total = NULL;
  g_clear_handle_id (&observe.timer, g_source_remove);
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *a11y = gtk_check_button_new_with_label ("accessibility (AT-SPI)");
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  observe.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (observe.window), "CEFNest observe");
  gtk_window_set_default_size (GTK_WINDOW (observe.window), 1000, 700);

  observe.fps = gtk_label_new ("0 fps");
  observe.total = gtk_label_new ("frames: 0");
  gtk_box_append (GTK_BOX (toolbar), observe.fps);
  gtk_box_append (GTK_BOX (toolbar), observe.total);
  gtk_widget_set_tooltip_text (a11y,
      "on top of the session's own screen-reader watcher");
  g_signal_connect (a11y, "toggled", G_CALLBACK (on_a11y_toggled), NULL);
  gtk_box_append (GTK_BOX (toolbar), a11y);
  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  observe.view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (observe.view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (observe.view));
  gtk_window_set_child (GTK_WINDOW (observe.window), root);

  observe.session = chromium_session_new (NULL);
  chromium_session_set_view (observe.session, observe.view);
  g_signal_connect (observe.window, "close-request", G_CALLBACK (on_close),
                    NULL);
  g_signal_connect (observe.window, "destroy", G_CALLBACK (on_window_destroy),
                    NULL);

  gtk_window_present (GTK_WINDOW (observe.window));

  /* A one-second sample is enough for a meter; a graph wants 100-250 ms. */
  observe.timer = g_timeout_add_seconds (1, tick, NULL);

  if (!chromium_session_start (observe.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  g_object_set_data_full (G_OBJECT (observe.window), "session",
                          g_object_ref (observe.session), g_object_unref);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (observe.session)
    chromium_session_stop (observe.session);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.observe",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
