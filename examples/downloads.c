#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Downloads. Nothing lands on disk until the host names a path:
 * ::download-begin is the question and exactly one of accept_download() /
 * reject_download() must answer it — a download left unanswered stays parked
 * and writes no file. This demo accepts everything into the user's download
 * directory, shows a progress row per download, and prints how each one ends
 * on ::download-end — the one place both the accept and the reject route
 * finish, so a host tracking rows has a single place to look.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-downloads
 */
typedef struct {
  int id;
  char *name;
  GtkWidget *row;
  GtkWidget *bar;    /* GtkProgressBar */
  GtkWidget *label;
} Download;

typedef struct {
  GtkWidget *window;
  GtkWidget *list;
  ChromiumSession *session;
  GList *downloads;
  char *dir;
} Downloads;

static Downloads downloads;

static void
download_free (Download *dl)
{
  g_free (dl->name);
  g_free (dl);
}

static Download *
find_download (int id)
{
  GList *l;

  for (l = downloads.downloads; l; l = l->next)
    {
      Download *dl = l->data;

      if (dl->id == id)
        return dl;
    }
  return NULL;
}

static void
download_remove (Download *dl)
{
  GList *link = g_list_find (downloads.downloads, dl);

  if (link)
    downloads.downloads = g_list_delete_link (downloads.downloads, link);
  if (downloads.list && dl->row)
    gtk_box_remove (GTK_BOX (downloads.list), dl->row);
  download_free (dl);
}

/* The host owns the path, so the host owns the sanitising: the name came from
 * a page. Same shape as the host demo — keep the extension, add " (n)". */
static char *
download_path (const char *suggested)
{
  g_autofree char *name = NULL;
  const char *dot;
  int n;

  name = g_path_get_basename (suggested && *suggested ? suggested
                                                      : "download");
  if (!strcmp (name, ".") || !strcmp (name, ".."))
    {
      g_free (name);
      name = g_strdup ("download");
    }

  dot = strrchr (name, '.');
  for (n = 0; n < 999; n++)
    {
      g_autofree char *candidate = n == 0 ? g_strdup (name) :
        (dot && dot > name ? g_strdup_printf ("%.*s (%d)%s",
                                              (int) (dot - name), name, n,
                                              dot) :
                             g_strdup_printf ("%s (%d)", name, n));
      g_autofree char *full = g_build_filename (downloads.dir, candidate,
                                                NULL);

      if (!g_file_test (full, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&full);
    }

  /* 999 collisions is not a case that happens, but the name it falls back to
   * still has to keep the extension the loop above keeps — and the formatted
   * string needs an owner: handing g_strdup_printf() straight to
   * g_build_filename() leaked one string per call. */
  {
    g_autofree char *stamp =
      (dot && dot > name)
        ? g_strdup_printf ("%.*s-%" G_GINT64_FORMAT "%s",
                           (int) (dot - name), name, g_get_real_time (), dot)
        : g_strdup_printf ("%s-%" G_GINT64_FORMAT, name, g_get_real_time ());

    return g_build_filename (downloads.dir, stamp, NULL);
  }
}

static void
session_download_begin (ChromiumSession *session, int id, const char *url,
                        const char *suggested_name, const char *mime_type,
                        gint64 total_bytes, gpointer data)
{
  Download *dl;
  g_autofree char *path = NULL;

  if (!downloads.window)
    {
      /* Nowhere to show the question; refusing by silence writes no file,
       * which is the safe default. */
      g_printerr ("download %d after window closed: left unanswered\n", id);
      return;
    }

  if (g_mkdir_with_parents (downloads.dir, 0700) != 0)
    {
      g_printerr ("download %d: cannot create %s, refusing\n", id,
                  downloads.dir);
      chromium_session_reject_download (session, id);
      return;
    }

  /* The helper makes no parent directory and the host owns the whole path:
   * this is the page's only say in where its file ends up — none. */
  path = download_path (suggested_name);
  chromium_session_accept_download (session, id, path);
  g_printerr ("download %d -> %s\n", id, path);

  dl = g_new0 (Download, 1);
  dl->id = id;
  dl->name = g_strdup (suggested_name && *suggested_name ? suggested_name
                                                         : "download");
  dl->row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  dl->label = gtk_label_new (dl->name);
  gtk_label_set_xalign (GTK_LABEL (dl->label), 0.0);
  dl->bar = gtk_progress_bar_new ();
  gtk_box_append (GTK_BOX (dl->row), dl->label);
  gtk_box_append (GTK_BOX (dl->row), dl->bar);
  gtk_box_append (GTK_BOX (downloads.list), dl->row);
  downloads.downloads = g_list_append (downloads.downloads, dl);

  /* @total_bytes is -1 when the server did not state a length; the progress
   * handler below then shows bytes moved instead of a fraction. */
  if (total_bytes < 0)
    gtk_progress_bar_set_text (GTK_PROGRESS_BAR (dl->bar),
                               "length unknown");
}

static void
session_download_progress (ChromiumSession *session, int id, gint64 received,
                           gint64 total, int percent, gint64 speed,
                           gpointer data)
{
  Download *dl = find_download (id);
  g_autofree char *text = NULL;

  if (!dl)
    return;

  if (percent < 0)
    text = g_strdup_printf ("%" G_GINT64_FORMAT " B, no length stated",
                            received);
  else
    {
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (dl->bar),
                                     percent / 100.0);
      text = g_strdup_printf ("%d %% — %" G_GINT64_FORMAT " B/s",
                              percent, speed);
    }
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (dl->bar), text);
}

static void
session_download_end (ChromiumSession *session, int id, const char *state,
                      const char *path, gint64 received, gpointer data)
{
  Download *dl = find_download (id);

  /* Both routes end here; @path is what Chromium actually wrote — not
   * necessarily the name asked for, because the helper uniquifies on the way
   * to disk. A refused-before-starting download ends with an empty path. */
  g_printerr ("download %d ended: state=%s path=%s bytes=%"
              G_GINT64_FORMAT "\n",
              id, state ? state : "?", path && *path ? path : "(none)",
              received);
  if (dl)
    download_remove (dl);
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  g_printerr ("helper gone: %s\n", reason ? reason : "(no reason)");
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (downloads.session);
  return FALSE;
}

static void
on_window_destroy (GtkWindow *window, gpointer data)
{
  downloads.window = NULL;
  downloads.list = NULL;
  g_list_free_full (downloads.downloads, (GDestroyNotify) download_free);
  downloads.downloads = NULL;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *hint;
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  downloads.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (downloads.window), "CEFNest downloads");
  gtk_window_set_default_size (GTK_WINDOW (downloads.window), 1000, 700);

  downloads.dir = g_strdup (
    g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD));
  if (!downloads.dir)
    downloads.dir = g_strdup (g_get_home_dir ());

  hint = gtk_label_new (NULL);
  {
    g_autofree char *markup =
      g_strdup_printf ("every download is accepted into <b>%s</b>",
                       downloads.dir);
    gtk_label_set_markup (GTK_LABEL (hint), markup);
  }
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 6);
  gtk_widget_set_margin_top (hint, 3);
  gtk_box_append (GTK_BOX (root), hint);

  downloads.list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (downloads.list, 6);
  gtk_widget_set_margin_end (downloads.list, 6);
  gtk_box_append (GTK_BOX (root), downloads.list);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (downloads.window), root);

  downloads.session = chromium_session_new (NULL);
  chromium_session_set_view (downloads.session, view);

  g_signal_connect (downloads.session, "download-begin",
                    G_CALLBACK (session_download_begin), NULL);
  g_signal_connect (downloads.session, "download-progress",
                    G_CALLBACK (session_download_progress), NULL);
  g_signal_connect (downloads.session, "download-end",
                    G_CALLBACK (session_download_end), NULL);
  g_signal_connect (downloads.session, "closed",
                    G_CALLBACK (session_closed), NULL);
  g_signal_connect (downloads.window, "close-request", G_CALLBACK (on_close),
                    NULL);
  g_signal_connect (downloads.window, "destroy",
                    G_CALLBACK (on_window_destroy), NULL);

  gtk_window_present (GTK_WINDOW (downloads.window));

  if (!chromium_session_start (downloads.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (downloads.window), "session",
                          g_object_ref (downloads.session), g_object_unref);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (downloads.session)
    chromium_session_stop (downloads.session);
  g_clear_pointer (&downloads.dir, g_free);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.downloads",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
