#include <errno.h>
#include <string.h>

#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* The opposite of examples/minimal.c: a host that wants the page to remember.
 * chromium_session_set_data_directory() gives the helper one complete Chromium
 * profile — cookies, localStorage, IndexedDB, the HTTP cache — and the next
 * start() with the same path reads it back. chromium_session_clear_data()
 * removes it, which is also the only deletion the component will ever do.
 *
 * The page lives in the same directory and counts its own launches in
 * localStorage, so a restart visibly continues where the last run stopped;
 * "Clear data" stops the helper, waits for the process (the directory's lock
 * is still held until it is gone), removes the directory and starts again.
 *
 *   ./builddir/examples/cefnest-persistent
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *view;
  GtkWidget *clear;
  GtkWidget *status;
  ChromiumSession *session;
  char *data_directory;
  /* "Clear data" was pressed while the helper was alive: the removal waits for
   * ::helper-exited, because until the process is gone its flock on the
   * directory is still held. */
  gboolean clear_when_exited;
  gboolean gone;
} Persistent;

static Persistent persistent;

static char *
page_path (void)
{
  return g_build_filename (persistent.data_directory, "page.html", NULL);
}

/* Written before every start: "Clear data" removes the whole directory, this
 * page included. */
static void
write_page (void)
{
  static const char html[] =
    "<!doctype html><meta charset=\"utf-8\">"
    "<title>CEFNest persistence</title>"
    "<body style=\"font: 16px sans-serif; margin: 3em\">"
    "<h1>localStorage counter</h1>"
    "<p id=\"out\">reading…</p>"
    "<script>\n"
    "const key = 'cefnest-persistent-visits';\n"
    "const n = Number(localStorage.getItem(key) || 0) + 1;\n"
    "localStorage.setItem(key, n);\n"
    "document.getElementById('out').textContent =\n"
    "  'This profile has been opened ' + n + ' time(s).';\n"
    "document.title = 'visits: ' + n;\n"
    "</script>";
  g_autofree char *path = page_path ();
  GError *error = NULL;

  if (!g_file_set_contents (path, html, -1, &error))
    {
      g_warning ("persistent: cannot write %s: %s", path, error->message);
      g_clear_error (&error);
    }
}

static void
set_status (const char *text)
{
  if (persistent.status)
    gtk_label_set_text (GTK_LABEL (persistent.status), text ? text : "");
}

static void
read_visits (void)
{
  /* The answer is JSON text ("3" with its quotes); ::script-result hands it
   * over whole. */
  chromium_session_run_javascript (persistent.session,
                                   "localStorage.getItem('cefnest-persistent-visits')",
                                   1);
}

static void
start_session (void)
{
  g_autofree char *page = page_path ();
  g_autofree char *uri = NULL;
  GError *error = NULL;

  write_page ();
  uri = g_filename_to_uri (page, NULL, NULL);
  persistent.gone = FALSE;

  if (!uri || !chromium_session_start (persistent.session, uri, &error))
    {
      set_status (error ? error->message : "cannot build the page URI");
      g_clear_error (&error);
      persistent.gone = TRUE;
      return;
    }

  set_status ("watching the page");
}

static void
clear_and_restart (void)
{
  GError *error = NULL;

  if (!chromium_session_clear_data (persistent.session, &error))
    {
      set_status (error->message);
      g_clear_error (&error);
      return;
    }

  /* The property kept its value, so this starts a fresh profile at the same
   * path and the page writes its counter from 1 again. */
  start_session ();
}

static void
on_clear (GtkButton *button, gpointer data)
{
  if (chromium_session_get_running (persistent.session))
    {
      /* stop() only signals the helper; the directory stays locked until the
       * process has actually been reaped. */
      persistent.clear_when_exited = TRUE;
      set_status ("stopping the helper before clearing its data…");
      chromium_session_stop (persistent.session);
      return;
    }

  clear_and_restart ();
}

static void
on_ready (ChromiumSession *session, gpointer data)
{
  read_visits ();
}

static void
on_script_result (ChromiumSession *session, int id, gboolean ok,
                  const char *value, const char *message, gpointer data)
{
  if (id != 1)
    return;

  if (!ok)
    {
      set_status (message && *message ? message : "localStorage read failed");
      return;
    }

  {
    g_autofree char *visits = g_strdup (value ? value : "");
    char *text;

    /* JSON string in, plain text out — this is a display, not a parser. */
    if (visits[0] == '"' && strlen (visits) >= 2)
      {
        visits[strlen (visits) - 1] = '\0';
        memmove (visits, visits + 1, strlen (visits));
      }
    text = g_strdup_printf ("localStorage visits: %s — data in %s",
                            visits, persistent.data_directory);
    set_status (text);
    g_free (text);
  }
}

static void
on_changed (ChromiumSession *session, gpointer data)
{
  if (persistent.gone || !persistent.window)
    return;

  gtk_window_set_title (GTK_WINDOW (persistent.window),
                        chromium_session_get_title (persistent.session));
}

static void
on_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  if (!persistent.clear_when_exited)
    {
      persistent.gone = TRUE;
      set_status (reason ? reason : "the helper is gone");
    }
}

static void
on_helper_exited (ChromiumSession *session, gpointer data)
{
  if (!persistent.clear_when_exited)
    return;

  persistent.clear_when_exited = FALSE;
  clear_and_restart ();
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (persistent.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  ChromiumView *view;

  persistent.data_directory =
    g_build_filename (g_get_user_data_dir (), "cefnest-persistent-example", NULL);
  if (g_mkdir_with_parents (persistent.data_directory, 0700) == -1)
    g_warning ("persistent: cannot create %s: %s", persistent.data_directory,
               g_strerror (errno));

  persistent.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (persistent.window),
                        "CEFNest persistent profile");
  gtk_window_set_default_size (GTK_WINDOW (persistent.window), 900, 640);

  persistent.clear = gtk_button_new_with_label ("Clear data");
  gtk_widget_set_tooltip_text (persistent.clear,
                               "Stop the helper, remove the profile, start again");
  g_signal_connect (persistent.clear, "clicked", G_CALLBACK (on_clear), NULL);
  gtk_box_append (GTK_BOX (toolbar), persistent.clear);

  persistent.status = gtk_label_new ("starting…");
  gtk_label_set_xalign (GTK_LABEL (persistent.status), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (persistent.status), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (persistent.status, TRUE);
  gtk_box_append (GTK_BOX (toolbar), persistent.status);

  gtk_box_append (GTK_BOX (root), toolbar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (persistent.window), root);

  persistent.session = chromium_session_new (NULL);
  chromium_session_set_view (persistent.session, view);
  chromium_session_set_data_directory (persistent.session,
                                       persistent.data_directory);

  g_signal_connect (persistent.session, "ready", G_CALLBACK (on_ready), NULL);
  g_signal_connect (persistent.session, "script-result",
                    G_CALLBACK (on_script_result), NULL);
  g_signal_connect (persistent.session, "changed", G_CALLBACK (on_changed), NULL);
  g_signal_connect (persistent.session, "closed", G_CALLBACK (on_closed), NULL);
  g_signal_connect (persistent.session, "helper-exited",
                    G_CALLBACK (on_helper_exited), NULL);
  g_signal_connect (persistent.window, "close-request", G_CALLBACK (on_close), NULL);

  gtk_window_present (GTK_WINDOW (persistent.window));
  start_session ();
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.persistent",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  g_free (persistent.data_directory);
  return status;
}
