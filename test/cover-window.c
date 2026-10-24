/* Opens a fullscreen opaque window for N seconds and closes itself.
 *
 * test/occlusion-gate.sh uses it to cover a browser window completely: that
 * is the signal mutter needs before it stops sending the covered window
 * frame callbacks, which is what the widget's frame clock goes quiet on.
 *
 *   cover-window 6
 */

#include <gtk/gtk.h>

static gboolean
close_now (gpointer data)
{
  gtk_window_destroy (GTK_WINDOW (data));
  return G_SOURCE_REMOVE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  int seconds = GPOINTER_TO_INT (data);
  GtkWidget *window = gtk_application_window_new (app);
  GtkWidget *label =
    gtk_label_new ("occlusion cover — the gate is measuring the window behind this one");

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_window_set_title (GTK_WINDOW (window), "occlusion cover");
  gtk_window_set_child (GTK_WINDOW (window), label);
  gtk_window_fullscreen (GTK_WINDOW (window));
  gtk_window_present (GTK_WINDOW (window));

  g_timeout_add_seconds (seconds > 0 ? seconds : 1, close_now, window);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.cover-window",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;
  int seconds = argc > 1 ? atoi (argv[1]) : 5;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate),
                    GINT_TO_POINTER (seconds));
  /* Not g_application_run(app, argc, argv): the seconds argument is ours,
   * and GtkApplication would try to open it as a file. */
  status = g_application_run (G_APPLICATION (app), 0, NULL);
  g_object_unref (app);
  return status;
}
