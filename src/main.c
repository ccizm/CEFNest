#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib-unix.h>

#include "chromium-session.h"
#include "chromium-view.h"

typedef struct {
  GtkApplication *app;
  GtkWindow *window;
  ChromiumView *view;
  GSubprocess *child;
  char **client_argv;
  /* What the process hands back once the run is over. Only a scripted run that
   * was cut short gives it anything but 0; see script_helper_gone(). */
  int exit_status;

  /* Set when the page comes from a Chromium helper process rather than from a
   * plain Wayland client: then the header bar is a browser toolbar and the
   * session owns the process. */
  ChromiumSession *session;
  GtkWidget *address;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *downloads;
  GtkWidget *permissions;
  GtkWidget *find_bar;
  GtkWidget *find_entry;
  GtkWidget *find_case;
  GtkWidget *find_label;
} Demo;

static Demo demo;

#ifndef CLIENT_PATH
#define CLIENT_PATH "shm-client"
#endif

#ifndef CLIENT_DEVELOPMENT
#define CLIENT_DEVELOPMENT ""
#endif

static void session_ready (ChromiumSession *session, gpointer data);
static void script_note_helper_gone (const char *reason);

static void
update_title (void)
{
  NestedCompositor *compositor = demo.view ? chromium_view_get_compositor (demo.view) : NULL;
  NestedSurface *surface = compositor ? nested_compositor_get_surface (compositor) : NULL;
  const char *socket = compositor ? chromium_view_get_socket_name (demo.view) : NULL;
  const char *title = demo.session ? chromium_session_get_title (demo.session)
                                   : (surface ? nested_surface_get_title (surface) : NULL);
  char buf[512];

  g_snprintf (buf, sizeof buf, "%s — nested on %s",
              title && *title ? title : "no client surface",
              socket ? socket : "(no socket)");

  if (demo.window)
    gtk_window_set_title (demo.window, buf);
}

static void
child_waited (GObject *source, GAsyncResult *result, gpointer data)
{
  GError *error = NULL;

  g_subprocess_wait_finish (G_SUBPROCESS (source), result, &error);
  if (error)
    {
      g_warning ("client: %s", error->message);
      g_clear_error (&error);
    }
  else if (g_subprocess_get_if_exited (G_SUBPROCESS (source)))
    g_message ("client exited with status %d",
               g_subprocess_get_exit_status (G_SUBPROCESS (source)));
  else if (g_subprocess_get_if_signaled (G_SUBPROCESS (source)))
    g_message ("client killed by signal %d",
               g_subprocess_get_term_sig (G_SUBPROCESS (source)));
  else
    g_message ("client exited");

  g_clear_object (&demo.child);
  update_title ();
}

static void
spawn_client (GtkButton *button, gpointer data)
{
  GSubprocessLauncher *launcher;
  const char *socket;
  GError *error = NULL;

  if (demo.child)
    return;

  socket = chromium_view_get_socket_name (demo.view);
  if (!socket)
    {
      g_warning ("the nested compositor has no socket");
      return;
    }

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);

  /* Native Wayland only: hand the child our socket and hide the X11 display,
   * otherwise Ozone picks XWayland. */
  g_subprocess_launcher_setenv (launcher, "WAYLAND_DISPLAY", socket, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_SESSION_TYPE", "wayland", TRUE);
  g_subprocess_launcher_unsetenv (launcher, "DISPLAY");
  g_subprocess_launcher_unsetenv (launcher, "WAYLAND_SOCKET");

  demo.child = g_subprocess_launcher_spawnv (launcher,
                                             (const char * const *) demo.client_argv,
                                             &error);
  g_object_unref (launcher);

  if (!demo.child)
    {
      g_warning ("cannot start the client: %s", error ? error->message : "?");
      g_clear_error (&error);
      return;
    }

  update_title ();
  g_subprocess_wait_async (demo.child, NULL, child_waited, NULL);
}

static void
stop_everything (void)
{
  if (demo.session)
    chromium_session_stop (demo.session);
  if (demo.child)
    g_subprocess_force_exit (demo.child);
}

static void
stop_client (GtkButton *button, gpointer data)
{
  stop_everything ();
}

/* ------------------------------------------------------------------------ *
 * Session mode: the page is a Chromium process the widget navigates over a
 * socket. Everything below is the toolbar for that; the compositor path is
 * unchanged, so the same widget still works with a plain Wayland client.
 * ------------------------------------------------------------------------ */

static void
session_changed (ChromiumSession *session, gpointer data)
{
  const gboolean running = chromium_session_get_running (session);

  /* Only follow the page while the user is not mid-typing in the entry. */
  if (demo.address && !gtk_widget_has_focus (demo.address))
    gtk_editable_set_text (GTK_EDITABLE (demo.address),
                           chromium_session_get_uri (session));

  gtk_widget_set_sensitive (demo.back,
                            running && chromium_session_get_can_go_back (session));
  gtk_widget_set_sensitive (demo.forward,
                            running && chromium_session_get_can_go_forward (session));
  update_title ();

  g_printerr ("demo: uri=%s title=%s loading=%d back=%d forward=%d muted=%d\n",
              chromium_session_get_uri (session),
              chromium_session_get_title (session),
              chromium_session_get_loading (session),
              chromium_session_get_can_go_back (session),
              chromium_session_get_can_go_forward (session),
              chromium_session_get_muted (session));
}

static void
session_zoom_changed (ChromiumSession *session, double level, gpointer data)
{
  /* The helper read the level back after SetZoomLevel; this is the only place
   * a scripted run can see that the command reached Chromium. */
  g_printerr ("demo: zoom level=%.2f\n", level);
}

static void
session_load_end (ChromiumSession *session, const char *uri, int http_code,
                  gpointer data)
{
  g_printerr ("demo: loaded %s (HTTP %d)\n", uri, http_code);
}

static void
session_load_error (ChromiumSession *session, int error_code, const char *uri,
                    const char *message, gpointer data)
{
  g_printerr ("demo: load failed %d %s %s\n", error_code, uri,
              message ? message : "");
}

static void
session_renderer_exited (ChromiumSession *session, int status, int error_code,
                         const char *message, gpointer data)
{
  /* The page does not change when this happens — the surface keeps its last
   * frame — so a transcript is the only place the difference shows. */
  g_printerr ("demo: renderer exited status=%d error=%d %s\n", status,
              error_code, message ? message : "");
}

static void
session_popup_requested (ChromiumSession *session, const char *url, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: popup-requested %s\n", url);
}

/* A menu action that the helper executes is only half of a browser action:
 * copying is the host clipboard, and "new window" is a second widget with its
 * own helper. The demo implements the latter by starting another copy of
 * itself with a unique CEF profile, so two windows never fight over
 * ~/.cache/cefnest-helper-hosted. */
static void
open_url_in_new_window (const char *url)
{
  GError *error = NULL;
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", &error);
  g_autofree char *uuid = NULL;
  g_autofree char *cache = NULL;
  g_autofree char *config = NULL;
  GSubprocessLauncher *launcher;
  GSubprocess *child;
  const char *argv[] = { NULL, NULL };

  if (!exe)
    {
      g_warning ("demo: cannot resolve own executable: %s",
                 error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  uuid = g_uuid_string_random ();
  cache = g_build_filename (g_get_user_cache_dir (), "cefnest-window", uuid, NULL);
  config = g_build_filename (g_get_user_config_dir (), "cefnest-window", uuid, NULL);
  g_mkdir_with_parents (cache, 0700);
  g_mkdir_with_parents (config, 0700);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "XDG_CACHE_HOME", cache, TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_CONFIG_HOME", config, TRUE);
  g_subprocess_launcher_setenv (launcher, "CHROMIUM_URL", url, TRUE);
  g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_SESSION_SCRIPT");
  g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_DUMP");
  g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_DUMP_DELAY");
  g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_WINDOW");

  if (demo.session)
    {
      const char *helper = chromium_session_get_helper_path (demo.session);

      if (helper && *helper)
        g_subprocess_launcher_setenv (launcher, "CEFNEST_HELPER", helper, TRUE);
    }

  argv[0] = exe;
  child = g_subprocess_launcher_spawnv (launcher, argv, &error);
  g_object_unref (launcher);

  if (!child)
    {
      g_warning ("demo: cannot open a new window: %s",
                 error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  g_printerr ("demo: opened new window for %s (pid %s)\n", url,
              g_subprocess_get_identifier (child));
  g_object_unref (child);
}

static void
session_copy_text (ChromiumSession *session, const char *text, gpointer data)
{
  GdkClipboard *clipboard;

  (void) session; (void) data;

  if (!demo.window || !text)
    return;

  clipboard = gdk_display_get_clipboard (
    gtk_widget_get_display (GTK_WIDGET (demo.window)));
  gdk_clipboard_set_text (clipboard, text);
  g_printerr ("demo: copy-text %s\n", text);
}

static void
session_open_url (ChromiumSession *session, const char *url, gpointer data)
{
  (void) session; (void) data;

  if (!url || !*url)
    return;

  g_printerr ("demo: open-url %s\n", url);
  open_url_in_new_window (url);
}

static void
session_page_message (ChromiumSession *session, const char *json, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: page-message %s\n", json);
}

static void
session_script_result (ChromiumSession *session, int id, gboolean ok,
                       const char *value, const char *message, gpointer data)
{
  g_printerr ("demo: script %d %s value=%s%s%s\n", id, ok ? "ok" : "failed",
              value, message && *message ? " message=" : "",
              message && *message ? message : "");
}

/* ------------------------------------------------------------------------ *
 * Downloads
 *
 * ::download-begin is a question the host has to answer with a path, so this
 * panel is the only place in the project that decides where a page's file
 * lands. By default every download is accepted into the user's download
 * directory, the way a browser does it; CEFNEST_DOWNLOAD_DIR moves the target
 * and CEFNEST_DOWNLOAD_ASK=1 makes each one wait for a click (or for a
 * download-accept script step). Both routes print the same transcript lines, so
 * a scripted run can assert on the flow without a hand on the mouse.
 * ------------------------------------------------------------------------ */

typedef struct {
  int id;
  char *name;
  gboolean decided;   /* a path or a refusal has gone to the helper */
  gboolean finished;  /* download-end arrived */
  GtkWidget *info;
  GtkWidget *accept;
  GtkWidget *reject;
} DownloadRow;

static void
free_download_row (gpointer data)
{
  DownloadRow *row = data;

  g_free (row->name);
  g_free (row);
}

static DownloadRow *
download_row_for (int id)
{
  GtkWidget *child;

  for (child = demo.downloads ? gtk_widget_get_first_child (demo.downloads) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      DownloadRow *row = g_object_get_data (G_OBJECT (child), "cefnest-download");

      if (row && row->id == id)
        return row;
    }
  return NULL;
}

static DownloadRow *
download_first_row (gboolean (*pick) (const DownloadRow *row))
{
  GtkWidget *child;

  for (child = demo.downloads ? gtk_widget_get_first_child (demo.downloads) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      DownloadRow *row = g_object_get_data (G_OBJECT (child), "cefnest-download");

      if (row && pick (row))
        return row;
    }
  return NULL;
}

static gboolean
row_undecided (const DownloadRow *row)
{
  return !row->decided && !row->finished;
}

static gboolean
row_open (const DownloadRow *row)
{
  return !row->finished;
}

static void
download_row_set_text (DownloadRow *row, const char *state)
{
  g_autofree char *text = g_strdup_printf ("%s — %s", row->name, state);

  if (row->info)
    gtk_label_set_text (GTK_LABEL (row->info), text);
}

static char *
format_bytes (gint64 bytes)
{
  if (bytes < 0)
    return g_strdup ("长度未知");
  if (bytes >= 1024 * 1024)
    return g_strdup_printf ("%.1f MiB", (double) bytes / (1024.0 * 1024.0));
  if (bytes >= 1024)
    return g_strdup_printf ("%.0f KiB", (double) bytes / 1024.0);
  return g_strdup_printf ("%" G_GINT64_FORMAT " 字节", bytes);
}

/* The suggested name is a hint from the page, so it is reduced to one file name
 * before it is ever joined to a directory: only the last component counts, both
 * separators are honoured because a name can arrive with backslashes in it, and
 * a control character in a terminal path is more trouble than it is worth. */
static char *
sanitise_download_name (const char *suggested)
{
  const char *base = suggested ? suggested : "";
  const char *scan;
  char *out;
  char *p;

  for (scan = base; *scan; scan++)
    if (*scan == '/' || *scan == '\\')
      base = scan + 1;

  out = g_strdup (base);
  for (p = out; *p; p++)
    if (g_ascii_iscntrl ((unsigned char) *p))
      *p = '_';
  g_strstrip (out);

  if (!*out || !strcmp (out, ".") || !strcmp (out, ".."))
    {
      g_free (out);
      out = g_strdup ("download");
    }
  return out;
}

static const char *
download_directory (void)
{
  const char *env = g_getenv ("CEFNEST_DOWNLOAD_DIR");
  const char *dir;

  if (env && *env)
    dir = env;
  else
    {
      dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
      if (!dir || !*dir)
        dir = g_get_home_dir ();
    }

  /* The helper does not create the directory for us, and XDG happily points at
   * a Downloads that a fresh account has never opened. */
  if (g_file_test (dir, G_FILE_TEST_IS_DIR))
    return dir;
  if (!g_mkdir_with_parents (dir, 0700))
    return dir;

  g_warning ("demo: cannot use %s as the download directory", dir);
  return NULL;
}

/* A page can offer the same name twice, and the second file must not quietly
 * replace a path the host already handed out and told the user about. */
static char *
download_target_path (const char *dir, const char *name)
{
  const char *dot = strrchr (name, '.');
  g_autofree char *stem = dot && dot > name ? g_strndup (name, (gsize) (dot - name))
                                           : g_strdup (name);
  g_autofree char *ext = dot && dot > name ? g_strdup (dot) : g_strdup ("");
  int n;

  for (n = 0; n < 1000; n++)
    {
      g_autofree char *leaf = n ? g_strdup_printf ("%s (%d)%s", stem, n, ext)
                               : g_strconcat (stem, ext, NULL);
      char *path = g_build_filename (dir, leaf, NULL);

      if (!g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
      g_free (path);
    }
  return g_build_filename (dir, stem, ext, NULL);
}

static void
download_accept (DownloadRow *row, const char *path)
{
  if (!row || row->decided || row->finished)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->accept, FALSE);
  g_printerr ("demo: download %d accept path=%s\n", row->id, path);
  chromium_session_accept_download (demo.session, row->id, path);
}

static void
download_reject (DownloadRow *row)
{
  if (!row || row->finished)
    return;

  /* One call covers both halves of "no": refusing a download that has not
   * started and cancelling one that is halfway through. */
  row->decided = TRUE;
  gtk_widget_set_visible (row->accept, FALSE);
  g_printerr ("demo: download %d reject\n", row->id);
  chromium_session_reject_download (demo.session, row->id);
}

static void
accept_clicked (GtkButton *button, gpointer data)
{
  DownloadRow *row = data;
  const char *dir = download_directory ();
  g_autofree char *path = NULL;

  if (!dir)
    return;
  path = download_target_path (dir, row->name);
  download_accept (row, path);
}

static void
reject_clicked (GtkButton *button, gpointer data)
{
  download_reject ((DownloadRow *) data);
}

typedef struct {
  int id;
} SaveImageRequest;

static void
save_image_finished (GObject *source, GAsyncResult *result, gpointer data)
{
  SaveImageRequest *request = data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  GError *error = NULL;
  GFile *file = gtk_file_dialog_save_finish (dialog, result, &error);
  DownloadRow *row = download_row_for (request->id);

  if (file)
    {
      g_autofree char *path = g_file_get_path (file);

      if (path && *path)
        {
          if (row)
            download_accept (row, path);
          else
            chromium_session_accept_download (demo.session, request->id, path);
        }
      else
        {
          g_warning ("demo: save-image returned no local path");
          if (row)
            download_reject (row);
          else
            chromium_session_reject_download (demo.session, request->id);
        }
      g_object_unref (file);
    }
  else
    {
      /* Dismissed or failed: the page's image is not written anywhere. */
      g_clear_error (&error);
      if (row)
        download_reject (row);
      else
        chromium_session_reject_download (demo.session, request->id);
    }

  g_free (request);
}

static void
save_image_prompt (int id, const char *suggested_name)
{
  GtkFileDialog *dialog;
  SaveImageRequest *request;

  if (!demo.window)
    return;

  dialog = gtk_file_dialog_new ();
  request = g_new0 (SaveImageRequest, 1);
  request->id = id;

  gtk_file_dialog_set_title (dialog, "图片另存为");
  if (suggested_name && *suggested_name)
    gtk_file_dialog_set_initial_name (dialog, suggested_name);
  gtk_file_dialog_save (dialog, demo.window, NULL, save_image_finished, request);
  g_object_unref (dialog);
}

static void
session_download_begin (ChromiumSession *session, int id, const char *url,
                        const char *suggested_name, const char *mime_type,
                        gint64 total_bytes, gpointer data)
{
  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *info = gtk_label_new ("");
  GtkWidget *accept = gtk_button_new_with_label ("保存");
  GtkWidget *reject = gtk_button_new_with_label ("丢弃");
  DownloadRow *row = g_new0 (DownloadRow, 1);
  g_autofree char *name = sanitise_download_name (suggested_name);
  g_autofree char *human = format_bytes (total_bytes);
  g_autofree char *path = NULL;

  row->id = id;
  row->name = g_steal_pointer (&name);
  row->info = info;
  row->accept = accept;
  row->reject = reject;

  gtk_label_set_xalign (GTK_LABEL (info), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (info), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_tooltip_text (accept, "保存到下载目录 / Save to the download directory");
  gtk_widget_set_tooltip_text (reject, "拒绝或取消 / Refuse or cancel");
  g_signal_connect (accept, "clicked", G_CALLBACK (accept_clicked), row);
  g_signal_connect (reject, "clicked", G_CALLBACK (reject_clicked), row);

  gtk_box_append (GTK_BOX (row_box), info);
  gtk_box_append (GTK_BOX (row_box), accept);
  gtk_box_append (GTK_BOX (row_box), reject);
  g_object_set_data_full (G_OBJECT (row_box), "cefnest-download", row,
                          free_download_row);

  gtk_widget_set_visible (demo.downloads, TRUE);
  gtk_box_append (GTK_BOX (demo.downloads), row_box);

  g_printerr ("demo: download %d begin name=%s mime=%s bytes=%" G_GINT64_FORMAT
              " url=%s\n", id, row->name, mime_type, total_bytes, url);

  download_row_set_text (row, human);

  /* A menu "save image as" action starts an image download; ask for a path
   * instead of silently dropping it in the downloads directory. Page-initiated
   * image downloads take the same path, which is the browser-like default. */
  if (g_str_has_prefix (mime_type ? mime_type : "", "image/") &&
      !g_getenv ("CEFNEST_DOWNLOAD_NO_SAVE_AS"))
    {
      save_image_prompt (id, row->name);
      return;
    }

  if (!g_getenv ("CEFNEST_DOWNLOAD_ASK"))
    {
      const char *dir = download_directory ();

      if (!dir)
        return;
      path = download_target_path (dir, row->name);
      download_accept (row, path);
    }
}

static void
session_download_progress (ChromiumSession *session, int id, gint64 received,
                           gint64 total, int percent, gint64 speed,
                           gpointer data)
{
  DownloadRow *row = download_row_for (id);

  g_printerr ("demo: download %d progress percent=%d received=%" G_GINT64_FORMAT
              " total=%" G_GINT64_FORMAT " speed=%" G_GINT64_FORMAT "\n",
              id, percent, received, total, speed);

  if (row)
    {
      g_autofree char *human = format_bytes (received);
      g_autofree char *text = percent >= 0
        ? g_strdup_printf ("%d%% %s", percent, human)
        : g_strdup (human);

      download_row_set_text (row, text);
    }
}

static void
session_download_end (ChromiumSession *session, int id, const char *state,
                      const char *path, gint64 received, gpointer data)
{
  DownloadRow *row = download_row_for (id);

  g_printerr ("demo: download %d end state=%s path=%s bytes=%" G_GINT64_FORMAT "\n",
              id, state, path, received);

  if (!row)
    return;

  row->decided = TRUE;
  row->finished = TRUE;
  gtk_widget_set_visible (row->accept, FALSE);
  gtk_widget_set_visible (row->reject, FALSE);
  download_row_set_text (row, *path ? path : "没有文件落盘");
}

static void
build_download_panel (GtkBox *box)
{
  const char *dir = download_directory ();

  demo.downloads = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  /* Which half of the policy is in force is a fact a scripted run has to be
   * able to check without reading the source, so it says it once here. */
  g_printerr ("demo: downloads: dir=%s ask=%d\n", dir ? dir : "none",
              g_getenv ("CEFNEST_DOWNLOAD_ASK") != NULL);

  gtk_widget_set_margin_start (demo.downloads, 8);
  gtk_widget_set_margin_end (demo.downloads, 8);
  gtk_widget_set_margin_top (demo.downloads, 4);
  gtk_widget_set_margin_bottom (demo.downloads, 4);
  /* Nothing to say until a page asks for a file. */
  gtk_widget_set_visible (demo.downloads, FALSE);
  gtk_box_append (box, demo.downloads);
}

/* ------------------------------------------------------------------ *
 * Find                                                               *
 * ------------------------------------------------------------------ *
 *
 * The bar is the host's half of an in-page search: it says what to look for
 * and which way, and every fact about how many matches there are comes back
 * from Chromium on ::find-result. The pattern is remembered here rather than
 * in the helper, so a step is a repeat of the request the bar just made. */

static char *find_pattern;
static gboolean find_match_case;

static void
find_bar_search (gboolean forward, gboolean next)
{
  if (!demo.session || !find_pattern || !*find_pattern)
    {
      g_warning ("demo: find has no pattern to search for");
      return;
    }

  chromium_session_find (demo.session, find_pattern, forward, find_match_case, next);
  g_printerr ("demo: find pattern=%s forward=%d match_case=%d next=%d\n",
              find_pattern, forward, find_match_case, next);
}

static void
find_set_pattern (const char *text)
{
  g_free (find_pattern);
  find_pattern = g_strdup (text ? text : "");
}

static void
find_entry_activate (GtkEntry *entry, gpointer data)
{
  find_set_pattern (gtk_editable_get_text (GTK_EDITABLE (entry)));
  find_match_case = gtk_check_button_get_active (GTK_CHECK_BUTTON (demo.find_case));
  find_bar_search (TRUE, FALSE);
}

static void
find_next_clicked (GtkButton *button, gpointer data)
{
  find_bar_search (TRUE, TRUE);
}

static void
find_prev_clicked (GtkButton *button, gpointer data)
{
  find_bar_search (FALSE, TRUE);
}

static void
find_bar_close (void)
{
  if (demo.session)
    chromium_session_stop_finding (demo.session, TRUE);
  g_printerr ("demo: find stop clear=1\n");

  if (demo.find_bar)
    gtk_widget_set_visible (demo.find_bar, FALSE);
  if (demo.find_label)
    gtk_label_set_text (GTK_LABEL (demo.find_label), "");
}

static void
find_close_clicked (GtkButton *button, gpointer data)
{
  find_bar_close ();
}

static gboolean
find_key_pressed (GtkEventControllerKey *controller, guint keyval,
                  guint keycode, GdkModifierType state, gpointer data)
{
  /* The window owns the browser zoom shortcuts, not the page: the view forwards
   * every key it gets, so Ctrl+plus/minus/0 has to be taken in the capture
   * phase first. Ctrl+Shift+= arrives as GDK_KEY_plus; the keypad variants are
   * what a full-size keyboard sends when Num Lock is on. */
  if (demo.session && (state & GDK_CONTROL_MASK) &&
      !(state & (GDK_ALT_MASK | GDK_SUPER_MASK)))
    {
      if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal || keyval == GDK_KEY_KP_Add)
        {
          chromium_session_zoom (demo.session, "in");
          return TRUE;
        }
      if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract)
        {
          chromium_session_zoom (demo.session, "out");
          return TRUE;
        }
      if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0)
        {
          chromium_session_zoom (demo.session, "reset");
          return TRUE;
        }
    }

  /* Capture phase, on purpose: Esc inside the page is what the helper reads as
   * "close the browser" (docs/session-lifecycle.md), so a find bar that wants
   * the key has to take it before the view forwards it. */
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_f || keyval == GDK_KEY_F))
    {
      if (!demo.find_bar || !demo.session)
        return FALSE;
      gtk_widget_set_visible (demo.find_bar, TRUE);
      gtk_widget_grab_focus (demo.find_entry);
      return TRUE;
    }

  if (keyval == GDK_KEY_Escape && demo.find_bar &&
      gtk_widget_get_visible (demo.find_bar))
    {
      find_bar_close ();
      return TRUE;
    }

  return FALSE;
}

static gboolean
find_key_released (GtkEventControllerKey *controller, guint keyval,
                   guint keycode, GdkModifierType state, gpointer data)
{
  NestedCompositor *compositor;
  gboolean modifier_key;
  gboolean shortcut_key;

  /* find_key_pressed() takes Ctrl+plus/minus/0 and Ctrl+F in the capture phase,
   * so the view never sees those presses. If the same controller also keeps the
   * release, Chromium is left with Ctrl down — and then every later wheel event
   * is Ctrl+wheel, i.e. another zoom. Forward the releases straight to the
   * nested seat here:
   *
   * key-released is a void signal, so this cannot stop propagation by returning
   * TRUE; the view may also send the release. A duplicate release is harmless,
   * while a missing one is the stuck-key bug. Shortcut keys still go to the
   * view as well, because they may be ordinary keys when Ctrl is not down
   * (F, Esc, bare plus/minus/0). */
  modifier_key =
    keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
    keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R ||
    keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
    keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R;
  shortcut_key =
    keyval == GDK_KEY_plus || keyval == GDK_KEY_equal ||
    keyval == GDK_KEY_KP_Add || keyval == GDK_KEY_minus ||
    keyval == GDK_KEY_KP_Subtract || keyval == GDK_KEY_0 ||
    keyval == GDK_KEY_KP_0 || keyval == GDK_KEY_f || keyval == GDK_KEY_F ||
    keyval == GDK_KEY_Escape;

  if ((!modifier_key && !shortcut_key) || !demo.view)
    return FALSE;

  compositor = chromium_view_get_compositor (demo.view);
  if (!compositor)
    return FALSE;

  g_debug ("demo: shortcut release keyval=%u (%s) evdev=%u", keyval,
           gdk_keyval_name (keyval), keycode - 8);
  nested_compositor_notify_keyboard_key (compositor, keycode - 8, FALSE);
  return modifier_key;
}

static void
session_find_result (ChromiumSession *session, int identifier, int count,
                     int active_ordinal, gboolean is_final, gpointer data)
{
  g_autofree char *text = NULL;

  g_printerr ("demo: find result id=%d count=%d ordinal=%d final=%d\n",
              identifier, count, active_ordinal, is_final);

  if (!demo.find_label)
    return;

  if (!is_final)
    text = count ? g_strdup_printf ("已找到 %d 处…", count) : g_strdup ("查找中…");
  else if (!count)
    text = g_strdup ("无匹配");
  else if (!active_ordinal)
    /* Measured: a new search selects nothing, so the number to show here is the
     * total, not "第 0 / N 处" (docs/find-path.md §2.3). */
    text = g_strdup_printf ("已找到 %d 处", count);
  else
    text = g_strdup_printf ("第 %d / %d 处", active_ordinal, count);

  gtk_label_set_text (GTK_LABEL (demo.find_label), text);
}

/* ------------------------------------------------------------------ *
 * Context menu                                                       *
 * ------------------------------------------------------------------ *
 *
 * The demo only reports the model Chromium built and leaves the drawing to the
 * session: a host that put up its own menu here would hide whether the default
 * one reaches the screen at all.
 *
 * Returning FALSE is how a handler hands the drawing back; the value it returns
 * is what lands in the emission's return location, not an argument it is given. */

static gboolean
session_context_menu (ChromiumSession *session, int x, int y, GList *items,
                      gpointer data)
{
  GList *l;
  int n = 0;

  for (l = items; l; l = l->next, n++)
    {
      ChromiumMenuItem *item = l->data;

      /* One line per row, id included: a scripted run answers a menu by its
       * command id, so this is where it finds out which ids exist. */
      g_printerr ("demo: menu row %d id=%d type=%s enabled=%d checked=%d label=%s\n",
                  n, item->id, item->type, item->enabled, item->checked,
                  item->label);
    }

  g_printerr ("demo: context menu at %d,%d with %d rows\n", x, y, n);

  return FALSE;
}

static void
build_find_bar (GtkBox *box)
{
  GtkWidget *button;
  GtkWidget *entry;

  demo.find_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  demo.find_entry = entry = gtk_entry_new ();
  demo.find_case = gtk_check_button_new_with_label ("区分大小写");
  demo.find_label = gtk_label_new ("");

  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "在页面中查找 / Find in page");
  gtk_editable_set_width_chars (GTK_EDITABLE (entry), 24);
  g_signal_connect (entry, "activate", G_CALLBACK (find_entry_activate), NULL);

  button = gtk_button_new_with_label ("上一个");
  g_signal_connect (button, "clicked", G_CALLBACK (find_prev_clicked), NULL);
  gtk_box_append (GTK_BOX (demo.find_bar), button);

  button = gtk_button_new_with_label ("下一个");
  g_signal_connect (button, "clicked", G_CALLBACK (find_next_clicked), NULL);
  gtk_box_append (GTK_BOX (demo.find_bar), button);

  gtk_box_append (GTK_BOX (demo.find_bar), demo.find_case);
  gtk_box_append (GTK_BOX (demo.find_bar), entry);

  gtk_label_set_xalign (GTK_LABEL (demo.find_label), 0.0);
  gtk_widget_set_hexpand (demo.find_label, TRUE);
  gtk_box_append (GTK_BOX (demo.find_bar), demo.find_label);

  button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_set_tooltip_text (button, "结束查找 / Stop finding (Esc)");
  g_signal_connect (button, "clicked", G_CALLBACK (find_close_clicked), NULL);
  gtk_box_append (GTK_BOX (demo.find_bar), button);

  gtk_widget_set_margin_start (demo.find_bar, 8);
  gtk_widget_set_margin_end (demo.find_bar, 8);
  gtk_widget_set_margin_top (demo.find_bar, 4);
  gtk_widget_set_margin_bottom (demo.find_bar, 4);
  /* Nothing to type into until the host asks for a search. */
  gtk_widget_set_visible (demo.find_bar, FALSE);

  /* Above the page: a find bar that pushes the viewport down is the convention
   * every browser settled on. */
  gtk_box_prepend (box, demo.find_bar);
}

/* ------------------------------------------------------------------ *
 * Permissions                                                        *
 * ------------------------------------------------------------------ *
 *
 * ::permission-request is a question the page is waiting on, and the session
 * never leaves one unanswered: the host decides it if it takes the signal, and
 * denies it if it does not. This panel is the half that asks a human, and
 * CEFNEST_PERMISSION says how. "ask" (the default) leaves a row up until 允许 or
 * 拒绝 is pressed, or until a perm-accept / perm-deny / perm-dismiss step presses
 * it for the pointer; "deny" and "allow" answer on arrival and keep the row only
 * as the record of what was decided; "ignore" hands the question back, which is
 * what a host with no permission UI does — and what the session then answers with
 * a deny.
 *
 * Asking is both the honest default and the dangerous one: Chromium works
 * through one media request at a time, and the next prompt dismisses the prompt
 * before it. A row left up is therefore not a page waiting politely — it is the
 * page's later requests arriving as "already gone". The panel says so in the
 * same line the gate asserts on. */

typedef struct {
  int id;
  gboolean decided;
  GtkWidget *info;
  GtkWidget *allow;
  GtkWidget *deny;
} PermissionRow;

static const char *
permission_policy (void)
{
  const char *env = g_getenv ("CEFNEST_PERMISSION");

  if (env && !strcmp (env, "deny"))
    return "deny";
  if (env && !strcmp (env, "allow"))
    return "allow";
  /* Declining to take the question at all is the one policy that belongs to the
   * session, not to this panel: it is what a host with no UI does. */
  if (env && !strcmp (env, "ignore"))
    return "ignore";
  return "ask";
}

static const char *
permission_verdict_name (ChromiumPermissionResult result)
{
  switch (result)
    {
    case CHROMIUM_PERMISSION_ACCEPT:
      return "accept";
    case CHROMIUM_PERMISSION_DISMISS:
      return "dismiss";
    default:
      return "deny";
    }
}

static const char *
permission_verdict_text (ChromiumPermissionResult result)
{
  switch (result)
    {
    case CHROMIUM_PERMISSION_ACCEPT:
      return "已允许";
    case CHROMIUM_PERMISSION_DISMISS:
      return "已收起";
    default:
      return "已拒绝";
    }
}

static PermissionRow *
permission_row_for (int id)
{
  GtkWidget *child;

  for (child = demo.permissions ? gtk_widget_get_first_child (demo.permissions) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      PermissionRow *row = g_object_get_data (G_OBJECT (child), "cefnest-permission");

      if (row && row->id == id)
        return row;
    }
  return NULL;
}

static void
permission_answer (PermissionRow *row, ChromiumPermissionResult result)
{
  if (!row || row->decided)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->allow, FALSE);
  gtk_widget_set_visible (row->deny, FALSE);
  gtk_label_set_text (GTK_LABEL (row->info), permission_verdict_text (result));

  chromium_session_permission_respond (demo.session, row->id, result);
  g_printerr ("demo: permission %d %s\n", row->id, permission_verdict_name (result));
}

static void
permission_allow_clicked (GtkButton *button, gpointer data)
{
  permission_answer ((PermissionRow *) data, CHROMIUM_PERMISSION_ACCEPT);
}

static void
permission_deny_clicked (GtkButton *button, gpointer data)
{
  permission_answer ((PermissionRow *) data, CHROMIUM_PERMISSION_DENY);
}

/* True when the step named a row this side is holding, so a scripted run cannot
 * answer the same request twice by accident. */
static gboolean
permission_step (const char *line, const char *verb, ChromiumPermissionResult result)
{
  gsize n = strlen (verb);
  PermissionRow *row;
  int id = 0;

  if (strncmp (line + 5, verb, n) != 0 || line[5 + n] != ' ')
    return FALSE;

  if (1 != sscanf (line + 5 + n + 1, "%d", &id))
    {
      g_warning ("demo: perm-%s wants a permission id, got '%s'", verb, line);
      return TRUE;
    }

  row = permission_row_for (id);
  if (!row)
    g_warning ("demo: no permission row %d to answer", id);
  else if (row->decided)
    /* The row that has already answered — or that Chromium took back — is the one
     * a step must not send a second answer for. The session guards this too, but
     * a host with a panel is the one that knows the row is dead. */
    g_warning ("demo: permission %d was already answered", id);
  else
    permission_answer (row, result);

  return TRUE;
}

static gboolean
session_permission_request (ChromiumSession *session, int id, const char *kind,
                            const char *origin, GList *names, int raw, gpointer data)
{
  GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *info = gtk_label_new ("");
  GtkWidget *allow = gtk_button_new_with_label ("允许");
  GtkWidget *deny = gtk_button_new_with_label ("拒绝");
  g_autoptr (GString) joined = g_string_new ("");
  g_autofree char *text = NULL;
  PermissionRow *row = g_new0 (PermissionRow, 1);
  const char *policy = permission_policy ();
  GList *l;

  for (l = names; l; l = l->next)
    {
      if (joined->len)
        g_string_append_c (joined, '+');
      g_string_append (joined, (const char *) l->data);
    }

  /* The id is what a script step answers by, and the names what it asserts on,
   * so both are on this line even when the panel is not on screen. */
  g_printerr ("demo: permission %d kind=%s names=%s raw=%d origin=%s policy=%s\n",
              id, kind, joined->str, raw, origin, policy);

  if (!strcmp (policy, "ignore"))
    return FALSE;

  row->id = id;
  row->info = info;
  row->allow = allow;
  row->deny = deny;

  text = g_strdup_printf ("%s · %s · %s", kind, joined->str, origin);
  gtk_label_set_text (GTK_LABEL (info), text);
  gtk_label_set_xalign (GTK_LABEL (info), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (info), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_tooltip_text (allow, "允许这个来源的这次请求 / Grant this request");
  gtk_widget_set_tooltip_text (deny, "拒绝 / Deny it");
  g_signal_connect (allow, "clicked", G_CALLBACK (permission_allow_clicked), row);
  g_signal_connect (deny, "clicked", G_CALLBACK (permission_deny_clicked), row);

  gtk_box_append (GTK_BOX (row_box), info);
  gtk_box_append (GTK_BOX (row_box), allow);
  gtk_box_append (GTK_BOX (row_box), deny);
  g_object_set_data_full (G_OBJECT (row_box), "cefnest-permission", row, g_free);

  gtk_widget_set_visible (demo.permissions, TRUE);
  gtk_box_append (GTK_BOX (demo.permissions), row_box);

  if (strcmp (policy, "ask") != 0)
    permission_answer (row, strcmp (policy, "allow") == 0
                             ? CHROMIUM_PERMISSION_ACCEPT
                             : CHROMIUM_PERMISSION_DENY);

  /* Taking the signal is taking the question: from here the answer is this
   * panel's to give, and the session will not second-guess it. */
  return TRUE;
}

static void
session_permission_dismissed (ChromiumSession *session, int id, const char *result,
                              gpointer data)
{
  PermissionRow *row = permission_row_for (id);

  g_printerr ("demo: permission %d dismissed as %s\n", id, result);

  if (!row || row->decided)
    return;

  /* Nothing to answer any more — the button that would have answered it is the
   * one thing that must not stay on screen. */
  row->decided = TRUE;
  gtk_widget_set_visible (row->allow, FALSE);
  gtk_widget_set_visible (row->deny, FALSE);
  gtk_label_set_text (GTK_LABEL (row->info), "页面已经不再等这个答案");
}

static void
build_permission_panel (GtkBox *box)
{
  demo.permissions = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  /* Which policy is in force is a fact a scripted run has to be able to check
   * without reading the source, so it says it once here. */
  g_printerr ("demo: permissions: policy=%s\n", permission_policy ());

  gtk_widget_set_margin_start (demo.permissions, 8);
  gtk_widget_set_margin_end (demo.permissions, 8);
  gtk_widget_set_margin_top (demo.permissions, 4);
  gtk_widget_set_margin_bottom (demo.permissions, 4);
  /* Nothing to say until a page asks for something. */
  gtk_widget_set_visible (demo.permissions, FALSE);
  gtk_box_append (box, demo.permissions);
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  if (demo.back)
    gtk_widget_set_sensitive (demo.back, FALSE);
  if (demo.forward)
    gtk_widget_set_sensitive (demo.forward, FALSE);
  update_title ();

  /* A scripted run has no hand on the close button, so a helper that goes away
   * on its own has to be reported rather than waited for. The reason rides along
   * so the report says what ended the session, not just that it ended. */
  script_note_helper_gone (reason);
}

static void
address_activate (GtkEntry *entry, gpointer data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  g_autofree char *uri = NULL;

  if (!text || !*text || !demo.session)
    return;

  /* A bare hostname still needs a scheme before Chromium will take it. */
  uri = strchr (text, ':') ? g_strdup (text) : g_strconcat ("https://", text, NULL);
  chromium_session_load_uri (demo.session, uri);
}

static void
go_back_clicked (GtkButton *button, gpointer data)
{
  chromium_session_go_back (demo.session);
}

static void
go_forward_clicked (GtkButton *button, gpointer data)
{
  chromium_session_go_forward (demo.session);
}

static void
reload_clicked (GtkButton *button, gpointer data)
{
  chromium_session_reload (demo.session, FALSE);
}

static const char *
initial_url (void)
{
  const char *env = g_getenv ("CHROMIUM_URL");
  const char *arg = demo.client_argv ? demo.client_argv[0] : NULL;

  if (env && *env)
    return env;
  if (arg && strstr (arg, "://"))
    return arg;

  /* Nothing to show yet, so nothing to load: starting on about:blank would make
   * it entry 0 of the session history and leave Back one press away from a page
   * the user never asked for. The first real navigation then becomes entry 0. */
  return "";
}

static void
build_toolbar (GtkHeaderBar *header)
{
  GtkWidget *button;
  GtkWidget *entry;
  GError *error = NULL;

  demo.back = button = gtk_button_new_from_icon_name ("go-previous-symbolic");
  gtk_widget_set_tooltip_text (button, "后退 / Back");
  gtk_widget_set_sensitive (button, FALSE);
  g_signal_connect (button, "clicked", G_CALLBACK (go_back_clicked), NULL);
  gtk_header_bar_pack_start (header, button);

  demo.forward = button = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_tooltip_text (button, "前进 / Forward");
  gtk_widget_set_sensitive (button, FALSE);
  g_signal_connect (button, "clicked", G_CALLBACK (go_forward_clicked), NULL);
  gtk_header_bar_pack_start (header, button);

  button = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_tooltip_text (button, "重新加载 / Reload");
  g_signal_connect (button, "clicked", G_CALLBACK (reload_clicked), NULL);
  gtk_header_bar_pack_start (header, button);

  demo.address = entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "地址 / Address");
  gtk_editable_set_width_chars (GTK_EDITABLE (entry), 28);
  gtk_widget_set_size_request (entry, 320, -1);
  g_signal_connect (entry, "activate", G_CALLBACK (address_activate), NULL);
  gtk_header_bar_pack_start (header, entry);

  demo.session = chromium_session_new (NULL);
  chromium_session_set_view (demo.session, demo.view);
  g_signal_connect (demo.session, "ready", G_CALLBACK (session_ready), NULL);
  g_signal_connect (demo.session, "changed", G_CALLBACK (session_changed), NULL);
  g_signal_connect (demo.session, "zoom-changed",
                    G_CALLBACK (session_zoom_changed), NULL);
  g_signal_connect (demo.session, "copy-text",
                    G_CALLBACK (session_copy_text), NULL);
  g_signal_connect (demo.session, "open-url",
                    G_CALLBACK (session_open_url), NULL);
  g_signal_connect (demo.session, "load-end", G_CALLBACK (session_load_end), NULL);
  g_signal_connect (demo.session, "load-error",
                    G_CALLBACK (session_load_error), NULL);
  g_signal_connect (demo.session, "renderer-exited",
                    G_CALLBACK (session_renderer_exited), NULL);
  g_signal_connect (demo.session, "script-result",
                    G_CALLBACK (session_script_result), NULL);
  g_signal_connect (demo.session, "page-message",
                    G_CALLBACK (session_page_message), NULL);
  g_signal_connect (demo.session, "popup-requested",
                    G_CALLBACK (session_popup_requested), NULL);
  g_signal_connect (demo.session, "download-begin",
                    G_CALLBACK (session_download_begin), NULL);
  g_signal_connect (demo.session, "download-progress",
                    G_CALLBACK (session_download_progress), NULL);
  g_signal_connect (demo.session, "download-end",
                    G_CALLBACK (session_download_end), NULL);
  g_signal_connect (demo.session, "find-result",
                    G_CALLBACK (session_find_result), NULL);
  g_signal_connect (demo.session, "context-menu",
                    G_CALLBACK (session_context_menu), NULL);
  g_signal_connect (demo.session, "permission-request",
                    G_CALLBACK (session_permission_request), NULL);
  g_signal_connect (demo.session, "permission-dismissed",
                    G_CALLBACK (session_permission_dismissed), NULL);
  g_signal_connect (demo.session, "closed", G_CALLBACK (session_closed), NULL);

  if (!chromium_session_start (demo.session, initial_url (), &error))
    {
      g_warning ("demo: %s", error->message);
      g_clear_error (&error);
      g_clear_object (&demo.session);
      return;
    }

  gtk_editable_set_text (GTK_EDITABLE (entry), chromium_session_get_uri (demo.session));
}


/* Grabs the view into a PNG: a cheap way to check that the nested surface
 * really ends up in the widget's render node. */
static void
capture_view (const char *path)
{
  GtkWidget *widget = GTK_WIDGET (demo.view);
  GtkWidget *parent = gtk_widget_get_parent (widget);
  GdkSurface *toplevel = gtk_native_get_surface (GTK_NATIVE (demo.window));
  double scale = toplevel ? gdk_surface_get_scale (toplevel) : 1.0;
  graphene_rect_t bounds;
  GskRenderNode *node;
  GdkTexture *texture;
  GtkSnapshot *snapshot;
  int width, height;

  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  /* The renderer draws a node one unit per pixel of the viewport it is given,
   * so grabbing at device resolution needs an explicit zoom: that is the only
   * way to see whether a client's buffer lands 1:1 on the display. */
  graphene_rect_init (&bounds, 0, 0, width * scale, height * scale);

  {
    NestedSurface *client_surface = nested_compositor_get_surface (chromium_view_get_compositor (CHROMIUM_VIEW (widget)));
    GdkTexture *client = client_surface ? nested_surface_get_texture (client_surface) : NULL;
    int sw = toplevel ? gdk_surface_get_width (toplevel) : 0;
    int sh = toplevel ? gdk_surface_get_height (toplevel) : 0;
    int local_w = 0, local_h = 0;
    graphene_rect_t src, dst;

    if (client_surface)
      {
        nested_surface_get_size (client_surface, &local_w, &local_h);
        nested_surface_get_paint_rect (client_surface, &src, &dst);
      }

    /* The scale the toplevel reports is the one the client is told about
     * through preferred_scale, so a client that honours it makes a buffer of
     * logical x scale. */
    g_printerr ("dump: widget %dx%d css, toplevel %dx%d scale=%.3f, "
                "client buffer %dx%d, surface %dx%d local, "
                "paint src %.1f,%.1f %.1fx%.1f -> dst %.0fx%.0f\n",
                width, height, sw, sh, scale,
                client ? gdk_texture_get_width (client) : 0,
                client ? gdk_texture_get_height (client) : 0,
                local_w, local_h,
                src.origin.x, src.origin.y, src.size.width, src.size.height,
                dst.size.width, dst.size.height);

    /* Grabbing the client texture on its own shows what GDK reads back from
     * the buffer the client made — for a dmabuf that is the GPU import. */
    if (client)
      {
        char *client_path = g_strconcat (path, ".client.png", NULL);

        if (!gdk_texture_save_to_png (client, client_path))
          g_warning ("dump: cannot write %s", client_path);
        else
          g_printerr ("dump: client texture saved to %s\n", client_path);
        g_free (client_path);
      }
  }

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  /* Zooming inside the snapshot is what a real 1.25-scale window does on the
   * way up: the widget's CSS-pixel node gets scaled to device pixels. */
  snapshot = gtk_snapshot_new ();
  gtk_snapshot_save (snapshot);
  if (scale != 1.0)
    gtk_snapshot_scale (snapshot, scale, scale);
  gtk_widget_snapshot_child (parent, widget, snapshot);
  gtk_snapshot_restore (snapshot);
  node = gtk_snapshot_free_to_node (snapshot);

  texture = node ? gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (demo.window)),
                                               node, &bounds)
                 : NULL;
  G_GNUC_END_IGNORE_DEPRECATIONS

  if (texture)
    {
      if (!gdk_texture_save_to_png (texture, path))
        g_warning ("dump: cannot write %s", path);
      g_object_unref (texture);
    }
  else
    g_warning ("dump: renderer gave no texture");

  if (node)
    gsk_render_node_unref (node);
}

/* CHROMIUM_DUMP=/path.png grabs the view once and exits. */
static gboolean
dump_view (gpointer data)
{
  capture_view ((const char *) data);
  g_application_quit (G_APPLICATION (demo.app));
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ *
 * A drag that leaves the page                                          *
 *                                                                      *
 * The compositor is the only thing that can read the payload out of a   *
 * page's wl_data_source; deciding what to do with it is the host's      *
 * job, so the drag below is the demo's own. COPY and nothing else: the  *
 * page's drag has already been called off by the time the bytes are     *
 * here, and there is no source left to move.                            *
 * ------------------------------------------------------------------ */

/* Only one handover is ever in flight — a widget has one pointer — and a drag
 * whose compositor never answered it is not worth keeping a second one for. */
static GdkDrag *demo_out_drag;
static char *demo_out_temp_dir;
static char *demo_out_temp_path;

static void
demo_out_clear_temp (void)
{
  if (demo_out_temp_path)
    {
      g_unlink (demo_out_temp_path);
      g_clear_pointer (&demo_out_temp_path, g_free);
    }
  if (demo_out_temp_dir)
    {
      g_rmdir (demo_out_temp_dir);
      g_clear_pointer (&demo_out_temp_dir, g_free);
    }
}

static void
demo_out_drag_end (GdkDrag *drag, gpointer data)
{
  if (demo_out_drag == drag)
    {
      /* dnd-finished means the destination has finished reading the file. Only
       * now is it safe to unlink the temp file; doing this on drop-performed
       * makes Nautilus open a path that no longer exists. */
      gdk_drag_drop_done (drag, TRUE);
      g_clear_object (&demo_out_drag);
      /* Keep the temp file alive after the drag as well: a file manager can
       * still open the URI a moment after dnd-finished. It is removed on the
       * next handover or when the window goes away. */
    }
}

/* Chromium's image drag offers its bytes as application/octet-stream with a
 * suggested name, not just text/html. Turn that into a real file so a file
 * manager has something it can copy. */
static char *
drag_suggested_name (const char *mime)
{
  const char *p = strstr (mime, "name=\"");
  char *name;

  if (p)
    {
      p += 6;
      const char *end = strchr (p, '\"');
      name = end ? g_strndup (p, end - p) : g_strdup (p);
    }
  else if (!strncmp (mime, "image/png", 9))
    name = g_strdup ("dragged-image.png");
  else if (!strncmp (mime, "image/jpeg", 10))
    name = g_strdup ("dragged-image.jpg");
  else if (!strncmp (mime, "image/gif", 9))
    name = g_strdup ("dragged-image.gif");
  else if (!strncmp (mime, "image/webp", 10))
    name = g_strdup ("dragged-image.webp");
  else
    name = g_strdup ("dragged-file");

  for (char *q = name; *q; q++)
    if (*q == '/' || *q == '\\' || g_ascii_iscntrl (*q))
      *q = '_';
  g_strstrip (name);

  if (!*name || !strcmp (name, ".") || !strcmp (name, ".."))
    {
      g_free (name);
      name = g_strdup ("dragged-file");
    }
  return name;
}

static GFile *
demo_out_write_temp (const char *mime, GBytes *bytes, char **path_out,
                     char **dir_out)
{
  GError *error = NULL;
  g_autofree char *dir = g_dir_make_tmp ("cefnest-drag-XXXXXX", &error);
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  gsize size = g_bytes_get_size (bytes);
  gconstpointer data = g_bytes_get_data (bytes, NULL);
  GFile *file;

  if (!dir)
    {
      g_print ("demo: drag out: no temp directory: %s\n",
               error ? error->message : "unknown");
      g_clear_error (&error);
      return NULL;
    }

  name = drag_suggested_name (mime);
  path = g_build_filename (dir, name, NULL);

  if (!g_file_set_contents (path, data, size, &error))
    {
      g_print ("demo: drag out: cannot write %s: %s\n", path,
               error ? error->message : "unknown");
      g_clear_error (&error);
      return NULL;
    }

  file = g_file_new_for_path (path);
  *path_out = g_steal_pointer (&path);
  *dir_out = g_steal_pointer (&dir);
  return file;
}

static GdkContentProvider *
provider_for_file (GFile *file)
{
  GdkFileList *list = gdk_file_list_new_from_array (&file, 1);
  GValue value = G_VALUE_INIT;
  GdkContentProvider *provider;

  g_value_init (&value, GDK_TYPE_FILE_LIST);
  g_value_take_boxed (&value, list);
  provider = gdk_content_provider_new_for_value (&value);
  g_value_unset (&value);
  return provider;
}

static void
demo_out_drag_cancel (GdkDrag *drag, GdkDragCancelReason reason, gpointer data)
{
  gdk_drag_drop_done (drag, FALSE);
  if (demo_out_drag == drag)
    g_clear_object (&demo_out_drag);
}

static void
demo_out_drag_drop (GdkDrag *drag, gpointer data)
{
  g_print ("demo: drag out: dropped as 0x%x\n", gdk_drag_get_selected_action (drag));

  /* drop-performed only says the destination accepted; it may still be
   * reading. dnd-finished is where drop_done() and temp cleanup belong. */
}

static void
demo_drag_out (NestedCompositor *compositor, const char *mime, GBytes *bytes,
               char **types, gpointer data)
{
  GdkDisplay *display = demo.window
    ? gtk_widget_get_display (GTK_WIDGET (demo.window))
    : gdk_display_get_default ();
  GdkSeat *seat = display ? gdk_display_get_default_seat (display) : NULL;
  GdkDevice *pointer = seat ? gdk_seat_get_pointer (seat) : NULL;
  /* ChromiumView is not a GtkNative; using it here returned a bogus surface and
   * gdk_drag_begin() never started a drag. The toplevel window is the native
   * whose surface owns the pointer grab. */
  GdkSurface *surface = demo.window
    ? gtk_native_get_surface (GTK_NATIVE (demo.window))
    : NULL;
  GdkContentProvider *provider;
  GdkDrag *drag;

  g_print ("demo: drag out: %s, %u bytes, types: %s\n", mime,
           (unsigned) g_bytes_get_size (bytes),
           types && *types ? g_strjoinv ("|", types) : "(none)");

  /* The payload is across the process boundary by now; whether this host wants
   * to put the window into a drag of its own is a separate question, and a run
   * that only wants to see what came out says so. */
  if (g_strcmp0 (g_getenv ("CEFNEST_DRAG_OUT"), "take") == 0)
    {
      g_print ("demo: drag out: taken, not carried\n");
      return;
    }

  /* A finished drag's file stays around for the target to finish opening; the
   * next handover is where the previous one is retired. */
  if (!demo_out_drag)
    demo_out_clear_temp ();

  if (!pointer || !surface)
    {
      g_print ("demo: drag out: no pointer and window to carry it with\n");
      return;
    }

  {
    gboolean file_payload = g_str_has_prefix (mime, "image/") ||
                            g_str_has_prefix (mime, "application/octet-stream");
    GdkContentProvider *file_provider = NULL;
    GFile *file = NULL;
    char *temp_path = NULL;
    char *temp_dir = NULL;

    if (file_payload)
      {
        file = demo_out_write_temp (mime, bytes, &temp_path, &temp_dir);
        if (file)
          {
            file_provider = provider_for_file (file);
            g_object_unref (file);
          }
      }

    if (file_provider)
      {
        g_autofree char *base = g_strdup (mime);
        char *semi = strchr (base, ';');

        if (semi)
          *semi = '\0';
        /* A file list is what a file manager accepts. The image bytes provider
         * would be a second representation, but GdkContentProvider's union
         * path is not needed for the file-manager case and was asserting on
         * this non-serializable value; keep the file list alone. */
        provider = file_provider;
        demo_out_temp_path = temp_path;
        demo_out_temp_dir = temp_dir;
      }
    else
      {
        provider = gdk_content_provider_new_for_bytes (mime, bytes);
        g_free (temp_path);
        g_free (temp_dir);
      }
  }

  drag = gdk_drag_begin (surface, pointer, provider, GDK_ACTION_COPY, 0, 0);
  g_object_unref (provider);

  if (!drag)
    {
      g_print ("demo: drag out: the compositor this window lives on refused it\n");
      demo_out_clear_temp ();
      return;
    }

  g_print ("demo: drag out: carrying it in a GTK drag\n");

  g_clear_object (&demo_out_drag);
  demo_out_drag = drag;
  /* GTK 4.18 spells these drop-performed / dnd-finished / cancel; the old
   * drop / failed / ended names do not exist on GdkWaylandDrag. */
  g_signal_connect (drag, "drop-performed",
                    G_CALLBACK (demo_out_drag_drop), NULL);
  g_signal_connect (drag, "dnd-finished",
                    G_CALLBACK (demo_out_drag_end), NULL);
  g_signal_connect (drag, "cancel",
                    G_CALLBACK (demo_out_drag_cancel), NULL);
}

/* The other direction's report: a host that carried a drag in has to answer its
 * own outer GdkDrop with what the page did with it, and this is the only moment
 * it is told. A page with no drop handler takes the bytes and says nothing, and
 * that arrives here as not taken. */
static void
demo_drag_in_end (NestedCompositor *compositor, gboolean taken, const char *mime,
                  gpointer data)
{
  g_print ("demo: drop-in verdict: %s, %s\n", taken ? "taken" : "given back",
           mime ? mime : "(no type)");
}

/* CHROMIUM_SESSION_SCRIPT drives the control plane without anyone typing. One
 * step per line: "load <uri>", "back", "forward", "reload", "stop",
 * "js <code>", "wait <ms>", "shot <path>", "dump <path>", "hover <x> <y>",
 * "click <x> <y> [button]", "drag <x0> <y0> <x1> <y1> [button]",
 * "drag-hold <x0> <y0> <x1> <y1> [button]", "release [button]", "surfaces",
 * "wheel <dx> <dy>", "mute", "unmute", "download-accept <path>",
 * "download-reject", "find <pattern>", "find-case <pattern>", "find-next",
 * "find-prev", "find-stop", "menu-pick <id>", "menu-dismiss",
 * "perm-accept <id>", "perm-deny <id>", "perm-dismiss <id>",
 * "close", "quit". This is how a headless run proves navigation, history and
 * repaint all work together, and "close" is
 * how it proves the way out of the window stops the helper with it. A "js" step
 * is tagged with its own line number, which is what comes back on
 * ::script-result, so a result can be matched to the step that asked for it.
 * The page steps need a helper session; "hover", "click", "drag", "wheel",
 * "shot", "dump", "surfaces", "drop-in", "drop-in-hold", "drop-in-move",
 * "drop-in-drop" and "close" work against any client, so the same script drives
 * the nested compositor without Chromium. A page step in a run that has no
 * session ends it with status 1: the run cannot do the one thing it was written
 * to do, and walking off the end of the script to exit 0 would report that as a
 * pass. */
typedef struct {
  GPtrArray *steps;
  guint index;
} SessionScript;

static SessionScript session_script;

static void script_run_next (void);

static gboolean
script_wait_done (gpointer data)
{
  script_run_next ();
  return G_SOURCE_REMOVE;
}

/* BTN_LEFT, spelled out so the kernel headers stay out of the demo. */
#define DEMO_BTN_LEFT 0x110

/* The release goes out on its own so a press can be dispatched, and whatever
 * popup it opened painted, before the click finishes. */
static gboolean
script_click_release (gpointer data)
{
  NestedCompositor *compositor = chromium_view_get_compositor (demo.view);

  if (compositor)
    nested_compositor_notify_pointer_button (compositor,
                                             GPOINTER_TO_UINT (data), FALSE);

  return G_SOURCE_REMOVE;
}

/* A page decides a text drag from how far the pointer travelled while the
 * button was down, so a jump from one end to the other is not a drag at all.
 * The intermediate positions are what make Chromium reach
 * wl_data_source.set_actions and wl_data_device.start_drag, and they have to
 * arrive one dispatch at a time. */
#define DEMO_DRAG_STEPS 12
#define DEMO_DRAG_INTERVAL 25

typedef struct {
  NestedCompositor *compositor;
  double x, y, step_x, step_y;
  int left;
  uint32_t button;
  unsigned int hold : 1;
} ScriptDrag;

static gboolean
script_drag_motion (gpointer data)
{
  ScriptDrag *drag = data;

  drag->x += drag->step_x;
  drag->y += drag->step_y;
  nested_compositor_notify_pointer_motion (drag->compositor, drag->x, drag->y);

  if (--drag->left > 0)
    return G_SOURCE_CONTINUE;

  /* A held drag stops with the button still down, so the script can look at
   * what the drag is carrying while it is in flight — a snapshot taken
   * mid-drag is the only way to see the drag icon painted — and finish it with
   * "release". */
  if (!drag->hold)
    nested_compositor_notify_pointer_button (drag->compositor, drag->button, FALSE);
  g_object_unref (drag->compositor);
  g_free (drag);

  return G_SOURCE_REMOVE;
}

/* What a text drag arriving from outside travels under: the two names stage 1
 * measured on a page's own drag, charset first, because a destination offered
 * both asks for that one. */
#define DEMO_DROP_IN_MIMES "text/plain;charset=utf-8,text/plain"
#define DEMO_DROP_IN_TEXT "from-host"

typedef struct {
  NestedCompositor *compositor;
  double x, y, step_x, step_y;
  int left;
  unsigned int let_go : 1;
} ScriptDropIn;

static gboolean
script_drop_in_motion (gpointer data)
{
  ScriptDropIn *job = data;

  job->x += job->step_x;
  job->y += job->step_y;

  /* The seat's own path: with a drag in flight it hands the position to the
   * surface underneath instead of to wl_pointer, which is what a host's drag
   * does too, since the outer compositor has the cursor. */
  nested_compositor_notify_pointer_motion (job->compositor, job->x, job->y);

  if (--job->left > 0)
    return G_SOURCE_CONTINUE;

  if (job->let_go)
    nested_compositor_drag_from_host_drop (job->compositor);

  g_object_unref (job->compositor);
  g_free (job);

  return G_SOURCE_REMOVE;
}

/* The host carrying a drag in, driven from the script instead of from a real
 * GdkDrop: same three entry points a drop target would call, with the payload
 * named on the line. The outer leg — a pointer that has to be really pressed in
 * another window — is what a script cannot give, so this is how the direction
 * gets measured at all (docs/drag-loop.md). */
static void
script_drop_in (double x0, double y0, double x1, double y1, const char *payload,
                const char *mimes, gboolean let_go)
{
  NestedCompositor *compositor = chromium_view_get_compositor (demo.view);
  char **types;
  GBytes *bytes;
  gboolean offered;

  if (!compositor)
    {
      g_warning ("demo: no nested compositor to drop onto");
      return;
    }

  types = g_strsplit (mimes, ",", -1);
  bytes = g_bytes_new (payload, strlen (payload));

  /* The cursor is already where the drag arrives, so the position goes in
   * first: entering a surface is what tells the page a drag exists, and the
   * compositor can only pick that surface if the pointer is over it. */
  nested_compositor_set_pointer_active (compositor, TRUE);
  nested_compositor_notify_pointer_motion (compositor, x0, y0);

  offered = nested_compositor_drag_from_host (compositor,
                                              (const char *const *) types,
                                              bytes, FALSE);
  g_printerr ("demo: drop-in %s at %.0f,%.0f: %s\n", types[0], x0, y0,
              offered ? "offered" : "refused");

  if (offered)
    {
      ScriptDropIn *job = g_new0 (ScriptDropIn, 1);

      job->compositor = g_object_ref (compositor);
      job->x = x0;
      job->y = y0;
      job->step_x = (x1 - x0) / DEMO_DRAG_STEPS;
      job->step_y = (y1 - y0) / DEMO_DRAG_STEPS;
      job->left = DEMO_DRAG_STEPS;
      job->let_go = let_go;
      g_timeout_add (DEMO_DRAG_INTERVAL, script_drop_in_motion, job);
    }

  g_bytes_unref (bytes);
  g_strfreev (types);
}

static void
script_finish (void)
{
  g_clear_pointer (&session_script.steps, g_ptr_array_unref);
  session_script.index = 0;
}

/* The helper is gone and this run still has steps to dispatch. Every callback
 * the remaining steps wait for — load-end, ::script-result, a download row, a
 * frame to dump — belongs to the process that just died, so the run cannot end
 * any way a harness would read as success: without this it keeps its window
 * open until whatever is watching it gives up, which turns a one-second failure
 * into the payload timeout. */
static gboolean
script_helper_gone (gpointer data)
{
  g_autofree char *reason = data;
  guint left;

  if (!session_script.steps)
    return G_SOURCE_REMOVE;

  /* Nothing left to dispatch: the last step was a wait or a dump, and the timer
   * or the quit it left behind still ends the run the way it always did. */
  if (session_script.index >= session_script.steps->len)
    return G_SOURCE_REMOVE;

  left = session_script.steps->len - session_script.index;
  g_printerr ("demo: the helper went away (%s) with %u of %u script step(s) still "
              "to run, so the run ends here\n",
              reason, left, session_script.steps->len);
  script_finish ();
  demo.exit_status = 1;
  g_application_quit (G_APPLICATION (demo.app));

  return G_SOURCE_REMOVE;
}

/* Run on an idle rather than straight from ::closed, because the steps that end
 * a run stop this session too — `close` closes the window, `dump` quits — and
 * by the time the idle is looked at they have finished their script. A quit
 * already in progress has no loop left to run it either, which is the right
 * answer: a run on its way out is not a run the helper abandoned.
 *
 * The reason travels as the idle's data because a signal argument is only
 * promised for the duration of the emission. */
static void
script_note_helper_gone (const char *reason)
{
  if (session_script.steps)
    g_idle_add (script_helper_gone, g_strdup (reason));
}

static void
script_run_next (void)
{
  NestedCompositor *compositor;

  while (session_script.steps && session_script.index < session_script.steps->len)
    {
      const char *line = g_ptr_array_index (session_script.steps, session_script.index++);

      /* Everything but the page commands works without a helper: a plain
       * Wayland client can be driven through the same script. */
      if (!demo.session && (!strncmp (line, "load ", 5) || !strncmp (line, "js ", 3) ||
                            !strcmp (line, "back") || !strcmp (line, "forward") ||
                            !strcmp (line, "reload") || !strcmp (line, "reload-hard") ||
                            !strcmp (line, "stop") || !strcmp (line, "mute") ||
                            !strcmp (line, "unmute") ||
                            !strncmp (line, "zoom-", 5) ||
                            !strncmp (line, "download-", 9) ||
                            !strncmp (line, "find", 4) ||
                            !strncmp (line, "menu-", 5) ||
                            !strncmp (line, "perm-", 5)))
        {
          /* Not a step that was skipped: without a session no later line of this
           * script can show a page either, so the run has already failed. Print
           * before script_finish() — `line` points into the array it frees.
           * Ending on an idle would be wrong here too: this is not a callback
           * some self-ending step is still unwinding. */
          g_printerr ("demo: '%s' needs a Chromium helper session, so the run ends "
                      "here\n", line);
          script_finish ();
          demo.exit_status = 1;
          g_application_quit (G_APPLICATION (demo.app));
          return;
        }

      if (!strncmp (line, "wait ", 5))
        {
          g_timeout_add (atoi (line + 5), script_wait_done, NULL);
          return;
        }
      else if (!strncmp (line, "load ", 5))
        {
          chromium_session_load_uri (demo.session, line + 5);
          continue;
        }
      else if (!strncmp (line, "js ", 3))
        {
          chromium_session_run_javascript (demo.session, line + 3, session_script.index);
          continue;
        }
      else if (!strncmp (line, "shot ", 5))
        {
          capture_view (line + 5);
          continue;
        }
      else if (!strncmp (line, "dump ", 5))
        {
          dump_view ((gpointer) (line + 5));
          return;
        }
      else if (!strcmp (line, "back"))
        {
          chromium_session_go_back (demo.session);
          continue;
        }
      else if (!strcmp (line, "forward"))
        {
          chromium_session_go_forward (demo.session);
          continue;
        }
      else if (!strcmp (line, "reload"))
        {
          chromium_session_reload (demo.session, FALSE);
          continue;
        }
      else if (!strcmp (line, "reload-hard"))
        {
          chromium_session_reload (demo.session, TRUE);
          continue;
        }
      else if (!strcmp (line, "stop"))
        {
          chromium_session_stop_loading (demo.session);
          continue;
        }
      else if (!strcmp (line, "zoom-in"))
        {
          chromium_session_zoom (demo.session, "in");
          continue;
        }
      else if (!strcmp (line, "zoom-out"))
        {
          chromium_session_zoom (demo.session, "out");
          continue;
        }
      else if (!strcmp (line, "zoom-reset"))
        {
          chromium_session_zoom (demo.session, "reset");
          continue;
        }
      else if (!strcmp (line, "mute"))
        {
          chromium_session_set_muted (demo.session, TRUE);
          continue;
        }
      else if (!strcmp (line, "unmute"))
        {
          chromium_session_set_muted (demo.session, FALSE);
          continue;
        }
      else if (!strncmp (line, "download-accept ", 16))
        {
          /* The panel picks the row, not the script: a scripted run cannot know
           * Chromium's download ids in advance, and standing in for the 保存
           * button is the whole point of the step. */
          DownloadRow *row = download_first_row (row_undecided);
          const char *path = line + 16;

          if (!*path)
            {
              g_warning ("demo: download-accept wants a path, got '%s'", line);
              continue;
            }
          if (!row)
            {
              g_warning ("demo: no download is waiting for a path");
              continue;
            }
          download_accept (row, path);
          continue;
        }
      else if (!strcmp (line, "download-reject"))
        {
          DownloadRow *row = download_first_row (row_open);

          if (!row)
            {
              g_warning ("demo: no download left to reject");
              continue;
            }
          download_reject (row);
          continue;
        }
      else if (!strncmp (line, "find ", 5))
        {
          /* The step owns the pattern the way the bar does: what it types
           * becomes what a following find-next steps through. */
          const char *pattern = line + 5;

          if (!*pattern)
            {
              g_warning ("demo: find wants a pattern, got '%s'", line);
              continue;
            }
          find_set_pattern (pattern);
          find_match_case = FALSE;
          find_bar_search (TRUE, FALSE);
          continue;
        }
      else if (!strncmp (line, "find-case ", 10))
        {
          /* The bar's 区分大小写 checkbox, written out: a scripted run has no
           * pointer to click it with, and the flag has to be reachable. */
          const char *pattern = line + 10;

          if (!*pattern)
            {
              g_warning ("demo: find-case wants a pattern, got '%s'", line);
              continue;
            }
          find_set_pattern (pattern);
          find_match_case = TRUE;
          find_bar_search (TRUE, FALSE);
          continue;
        }
      else if (!strcmp (line, "find-next"))
        {
          find_bar_search (TRUE, TRUE);
          continue;
        }
      else if (!strcmp (line, "find-prev"))
        {
          find_bar_search (FALSE, TRUE);
          continue;
        }
      else if (!strcmp (line, "find-stop"))
        {
          find_bar_close ();
          continue;
        }
      else if (!strncmp (line, "menu-pick ", 10))
        {
          /* The command id from the "menu row" lines, because a script has no
           * pointer to aim at a row and CEF's id is what carries the choice. */
          int id = 0;

          if (1 != sscanf (line + 10, "%d", &id))
            {
              g_warning ("demo: menu-pick wants a command id, got '%s'", line);
              continue;
            }
          chromium_session_context_menu_select (demo.session, id);
          g_printerr ("demo: menu pick id=%d\n", id);
          continue;
        }
      else if (!strcmp (line, "menu-dismiss"))
        {
          chromium_session_context_menu_dismiss (demo.session);
          g_printerr ("demo: menu dismiss\n");
          continue;
        }
      else if (!strncmp (line, "perm-", 5))
        {
          /* The id from the "demo: permission" lines, standing in for whichever
           * button the panel would have put under a finger. */
          static const struct {
            const char *verb;
            ChromiumPermissionResult result;
          } verdicts[] = {
            { "accept",  CHROMIUM_PERMISSION_ACCEPT },
            { "deny",    CHROMIUM_PERMISSION_DENY },
            { "dismiss", CHROMIUM_PERMISSION_DISMISS },
          };
          gsize i;

          for (i = 0; i < G_N_ELEMENTS (verdicts); i++)
            if (permission_step (line, verdicts[i].verb, verdicts[i].result))
              break;
          if (i == G_N_ELEMENTS (verdicts))
            g_warning ("demo: not a permission verdict: '%s'", line);
          continue;
        }
      else if (!strncmp (line, "hover ", 6))
        {
          int x = 0, y = 0;

          if (2 != sscanf (line + 6, "%d %d", &x, &y))
            {
              g_warning ("demo: hover wants two numbers, got '%s'", line);
              continue;
            }

          /* Pointer focus normally comes from the widget's own enter, and a
           * headless run has no hand on a mouse. This stands in for it, which
           * is how a page is made to ask for a cursor. */
          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to hover on");
              continue;
            }
          nested_compositor_set_pointer_active (compositor, TRUE);
          nested_compositor_notify_pointer_motion (compositor, x, y);
          continue;
        }
      else if (!strncmp (line, "click ", 6))
        {
          int x = 0, y = 0, button = DEMO_BTN_LEFT;

          if (sscanf (line + 6, "%d %d %d", &x, &y, &button) < 2)
            {
              g_warning ("demo: click wants '<x> <y> [button]', got '%s'", line);
              continue;
            }

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to click on");
              continue;
            }

          /* A right click is how a page opens a context menu, and the menu is
           * only reachable if the press lands on the surface under the pointer. */
          nested_compositor_set_pointer_active (compositor, TRUE);
          nested_compositor_notify_pointer_motion (compositor, x, y);
          nested_compositor_notify_pointer_button (compositor, (uint32_t) button, TRUE);
          g_timeout_add (80, script_click_release, GUINT_TO_POINTER ((uint32_t) button));
          continue;
        }
      else if (!strncmp (line, "drag ", 5) || !strncmp (line, "drag-hold ", 10))
        {
          gboolean hold = !strncmp (line, "drag-hold ", 10);
          const char *args = line + (hold ? 10 : 5);
          double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
          int button = DEMO_BTN_LEFT;
          ScriptDrag *drag;

          if (sscanf (args, "%lf %lf %lf %lf %d", &x0, &y0, &x1, &y1, &button) < 4)
            {
              g_warning ("demo: %s wants '<x0> <y0> <x1> <y1> [button]', got '%s'",
                         hold ? "drag-hold" : "drag", line);
              continue;
            }

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to drag on");
              continue;
            }

          /* The release goes out roughly 300 ms after the press unless the step
           * asked to hold, so a script that cares about what the drag did has
           * to wait before looking. */
          nested_compositor_set_pointer_active (compositor, TRUE);
          nested_compositor_notify_pointer_motion (compositor, x0, y0);
          nested_compositor_notify_pointer_button (compositor, (uint32_t) button, TRUE);

          drag = g_new0 (ScriptDrag, 1);
          drag->compositor = g_object_ref (compositor);
          drag->x = x0;
          drag->y = y0;
          drag->step_x = (x1 - x0) / DEMO_DRAG_STEPS;
          drag->step_y = (y1 - y0) / DEMO_DRAG_STEPS;
          drag->left = DEMO_DRAG_STEPS;
          drag->button = (uint32_t) button;
          drag->hold = hold;
          g_timeout_add (DEMO_DRAG_INTERVAL, script_drag_motion, drag);
          continue;
        }
      else if (!strncmp (line, "drop-in ", 8) || !strncmp (line, "drop-in-hold ", 13))
        {
          gboolean hold = !strncmp (line, "drop-in-hold ", 13);
          const char *args = line + (hold ? 13 : 8);
          double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
          char text[128] = "", mime_list[256] = "";

          if (sscanf (args, "%lf %lf %lf %lf %127s %255s", &x0, &y0, &x1, &y1,
                      text, mime_list) < 4)
            {
              g_warning ("demo: %s wants '<x0> <y0> <x1> <y1> [text] [mime,mime,...]', got '%s'",
                         hold ? "drop-in-hold" : "drop-in", line);
              continue;
            }

          script_drop_in (x0, y0, x1, y1,
                          text[0] ? text : DEMO_DROP_IN_TEXT,
                          mime_list[0] ? mime_list : DEMO_DROP_IN_MIMES,
                          !hold);
          continue;
        }
      else if (!strncmp (line, "drop-in-move ", 13))
        {
          double x = 0, y = 0;

          if (sscanf (line + 13, "%lf %lf", &x, &y) < 2)
            {
              g_warning ("demo: drop-in-move wants '<x> <y>', got '%s'", line);
              continue;
            }

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to move over");
              continue;
            }

          nested_compositor_notify_pointer_motion (compositor, x, y);
          continue;
        }
      else if (!strcmp (line, "drop-in-drop") || !strcmp (line, "drop-in-cancel"))
        {
          gboolean dropping = line[8] == 'd';

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to let go on");
              continue;
            }

          if (dropping)
            nested_compositor_drag_from_host_drop (compositor);
          else
            nested_compositor_drag_from_host_cancel (compositor);
          continue;
        }
      else if (!strcmp (line, "surfaces"))
        {
          GList *stack, *l;
          int n = 0;

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to look at");
              continue;
            }

          /* One line per surface that reaches the screen, in the order they are
           * drawn. A popup a page opened and the image a drag is carrying are
           * both invisible to the page, so this is how a headless run sees
           * them. */
          stack = nested_compositor_get_paint_stack (compositor);
          for (l = stack; l; l = l->next)
            {
              NestedSurface *surface = l->data;
              const char *title;
              int x = 0, y = 0, w = 0, h = 0;

              nested_surface_get_size (surface, &w, &h);
              nested_surface_get_position (surface, &x, &y);
              title = nested_surface_get_title (surface);
              g_printerr ("demo: surface %d at %d,%d size %dx%d title=%s\n",
                          ++n, x, y, w, h, title ? title : "none");
            }
          g_list_free_full (stack, g_object_unref);
          g_printerr ("demo: %d surface(s) paint\n", n);
          continue;
        }
      else if (!strncmp (line, "release", 7))
        {
          int button = DEMO_BTN_LEFT;

          if (line[7] && sscanf (line + 7, "%d", &button) < 1)
            {
              g_warning ("demo: release wants '[button]', got '%s'", line);
              continue;
            }

          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to let go on");
              continue;
            }

          nested_compositor_notify_pointer_button (compositor, (uint32_t) button, FALSE);
          continue;
        }
      else if (!strncmp (line, "wheel ", 6))
        {
          double dx = 0, dy = 0;

          if (sscanf (line + 6, "%lf %lf", &dx, &dy) < 2)
            {
              g_warning ("demo: wheel wants '<dx> <dy>' in detents, got '%s'", line);
              continue;
            }

          /* Detents go in because that is what a hand on a wheel gives, and this
           * is the same arithmetic the widget does: ten surface pixels and 120
           * in the high-resolution channel for one of them. Half a detent is the
           * case only the 120ths can carry. The stop is FALSE because a detent
           * is not the end of a gesture — GDK never marks one as a stop either,
           * and a stop would make Chromium settle the sequence and drop this
           * step's delta, whether it rides in this frame or not. */
          compositor = chromium_view_get_compositor (demo.view);
          if (!compositor)
            {
              g_warning ("demo: no nested compositor to scroll");
              continue;
            }

          nested_compositor_set_pointer_active (compositor, TRUE);
          nested_compositor_notify_pointer_axis (compositor, dx * 10.0, dy * 10.0,
                                                 (int32_t) lround (dx * 120.0),
                                                 (int32_t) lround (dy * 120.0),
                                                 WL_POINTER_AXIS_SOURCE_WHEEL, FALSE);
          continue;
        }
      else if (!strcmp (line, "close"))
        {
          /* The same route a hand on the close button takes: close-request
           * first, then the window and everything in it. */
          if (demo.window)
            gtk_window_close (demo.window);
          break;
        }
      else if (!strcmp (line, "quit"))
        {
          break;
        }

      g_warning ("demo: unknown script step '%s'", line);
    }

  script_finish ();
  g_application_quit (G_APPLICATION (demo.app));
}

static void
start_session_script (void)
{
  const char *path = g_getenv ("CHROMIUM_SESSION_SCRIPT");
  g_autofree char *text = NULL;
  g_autofree char **lines = NULL;
  GError *error = NULL;
  guint i;

  if (!path || !*path)
    return;

  if (!g_file_get_contents (path, &text, NULL, &error))
    {
      g_warning ("demo: cannot read the script: %s", error->message);
      g_clear_error (&error);
      return;
    }

  session_script.steps = g_ptr_array_new_with_free_func (g_free);
  lines = g_strsplit (text, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      char *line = g_strchomp (g_strdup (lines[i]));

      if (*line)
        g_ptr_array_add (session_script.steps, line);
      else
        g_free (line);
    }

  if (session_script.steps->len == 0)
    {
      script_finish ();
      return;
    }

  /* A script that drives a helper page waits for the helper itself: without
   * CHROMIUM_HELPER there is no session to report ready, so the plain client
   * path keeps a fixed guess. */
  if (!demo.session)
    g_timeout_add (800, script_wait_done, NULL);
}

/* The first command of a session script lands here rather than on a timer. A
 * cold Chromium profile takes seconds to come up, and a load sent before the
 * channel is open is simply lost — the script then waits for a page that was
 * never asked for. */
static void
session_ready (ChromiumSession *session, gpointer data)
{
  if (session_script.steps && session_script.index == 0)
    g_idle_add (script_wait_done, NULL);
}

static void
on_surface_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
  update_title ();

  /* A page that appeared is a page to type into: without this the view keeps
   * no focus and neither the nested seat nor an input method ever gets a key. */
  if (nested_compositor_get_surface (NESTED_COMPOSITOR (object)))
    gtk_widget_grab_focus (GTK_WIDGET (demo.view));
}

/* What the display clipboard holds right now. Printed rather than asserted so
 * a client's copy shows up in the same log as the client's own paste. */
static void
clipboard_read_done (GObject *clipboard, GAsyncResult *result, gpointer data)
{
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard),
                                                          result, NULL);

  g_printerr ("demo: clipboard is '%s'\n", text ? text : "(no text)");
}

static void
clipboard_changed (GObject *clipboard, GParamSpec *pspec, gpointer data)
{
  gdk_clipboard_read_text_async (GDK_CLIPBOARD (clipboard), NULL,
                                 clipboard_read_done, NULL);
}

static void
clipboard_seed (const char *why)
{
  const char *seed = g_getenv ("CHROMIUM_CLIPSET");
  GdkClipboard *clipboard;

  if (!seed || !*seed || !demo.window)
    return;

  clipboard = gdk_display_get_clipboard (gtk_widget_get_display (GTK_WIDGET (demo.window)));
  gdk_clipboard_set_text (clipboard, seed);
  g_printerr ("demo: seeded the clipboard with '%s' (%s, window active=%d)\n", seed, why,
              gtk_window_is_active (demo.window));
}

/* Wayland gives the selection to a client only while that client holds keyboard
 * focus, so a seed sent before the window is activated can be taken straight
 * back — and an empty clipboard looks exactly like a bridge that cannot paste.
 * Seed on sight and once more when the window gets it, whichever comes first. */
static void
window_active_changed (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  static gboolean reseeded = FALSE;

  if (reseeded || !gtk_window_is_active (window))
    return;

  reseeded = TRUE;
  clipboard_seed ("on activation");
}

static void
setup_clipboard_test (void)
{
  GdkClipboard *clipboard;
  const char *seed = g_getenv ("CHROMIUM_CLIPSET");

  if (!seed && !g_getenv ("CHROMIUM_CLIPWATCH"))
    return;

  clipboard = gdk_display_get_clipboard (gtk_widget_get_display (GTK_WIDGET (demo.window)));
  g_signal_connect (clipboard, "notify::content", G_CALLBACK (clipboard_changed), NULL);

  if (seed && *seed)
    {
      g_signal_connect (demo.window, "notify::active", G_CALLBACK (window_active_changed), NULL);
      clipboard_seed ("at startup");
    }
}

static void
report (void)
{
  if (demo.session)
    g_printerr ("nested compositor on WAYLAND_DISPLAY=%s, helper: %s\n",
                chromium_view_get_socket_name (demo.view),
                chromium_session_get_helper_path (demo.session));
  else
    g_printerr ("nested compositor on WAYLAND_DISPLAY=%s, client: %s\n",
                chromium_view_get_socket_name (demo.view),
                demo.client_argv ? demo.client_argv[0] : "(none)");
}

/* GtkApplication owns the window, the box owns the view and the header bar owns
 * the toolbar, so these are borrowed pointers and closing the window frees
 * them. Leaving them set would make the next ::changed or ::closed title a
 * window that no longer exists. */
static void
forget_widgets (void)
{
  demo.window = NULL;
  demo.view = NULL;
  demo.address = NULL;
  demo.back = NULL;
  demo.forward = NULL;
  demo.downloads = NULL;
  demo.find_bar = NULL;
  demo.find_entry = NULL;
  demo.find_case = NULL;
  demo.find_label = NULL;
  /* A pattern outliving its session would make the next find-next step a search
   * on a page that never asked for it. */
  g_clear_pointer (&find_pattern, g_free);
}

static gboolean
window_close_request (GtkWindow *window, gpointer data)
{
  /* This is the stop: it reaps the helper, unlinks its control socket and
   * takes the plain client down with it. It has to happen here rather than on
   * shutdown, because ::closed updates the toolbar and the toolbar is what
   * dies next — returning FALSE hands the window to GtkApplication, which
   * destroys it and everything inside it. */
  demo_out_clear_temp ();
  stop_everything ();
  forget_widgets ();
  return FALSE;
}

static void
app_shutdown (GApplication *application, gpointer data)
{
  /* Reaching the end without a close — a script's "quit", a CHROMIUM_DUMP
   * timer, a terminating signal — has to stop the helper too: the socket name
   * is a fresh uuid per run, so a run that never stopped left one dead file
   * behind in $XDG_RUNTIME_DIR and nothing came back for it. */
  demo_out_clear_temp ();
  stop_everything ();
  g_clear_object (&demo.session);
  g_clear_object (&demo.child);
  forget_widgets ();
}

static gboolean
signal_quit (gpointer data)
{
  g_application_quit (G_APPLICATION (data));
  return G_SOURCE_REMOVE;
}

/* The demo shows a page when the component can start one. Naming a helper in
 * the environment still decides the mode — that is how the fake helper is driven
 * through a script — but an installed component finds its helper by the rule in
 * chromium-session.c and needs no environment at all, which is the point of
 * installing it. With neither, the plain client takes over and the compositor is
 * still exercised, just without a browser in it. */
static gboolean
helper_is_available (void)
{
  ChromiumSession *session = chromium_session_new (NULL);
  gboolean found = g_file_test (chromium_session_get_helper_path (session),
                                G_FILE_TEST_IS_EXECUTABLE);

  g_object_unref (session);
  return found;
}

/* The client the demo drives when there is no Chromium to start. The two paths
 * are tried in the opposite order to the helper's, because the two have opposite
 * failure modes: a shipped component that quietly ran a development Chromium
 * would be running the wrong browser, while a demo that quietly ran an
 * installed shm-client would be running a stale copy of its own test driver
 * instead of the one built from this tree — which is what every gate script
 * assumes. With the tree gone there is nothing stale to prefer, so the installed
 * copy is what an unpackaged demo finds. */
static const char *
demo_find_client (void)
{
  if (*CLIENT_DEVELOPMENT &&
      g_file_test (CLIENT_DEVELOPMENT, G_FILE_TEST_IS_EXECUTABLE))
    return CLIENT_DEVELOPMENT;

  return CLIENT_PATH;
}

static void
activate (GApplication *application, gpointer data)
{
  const gboolean use_session = g_getenv ("CHROMIUM_HELPER") != NULL ||
                               helper_is_available ();
  GtkWidget *box;
  GtkWidget *header;
  GtkWidget *button;

  if (demo.window)
    {
      gtk_window_present (demo.window);
      return;
    }

  demo.window = GTK_WINDOW (gtk_application_window_new (GTK_APPLICATION (application)));
  g_signal_connect (demo.window, "close-request", G_CALLBACK (window_close_request), NULL);

  demo.view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);

  if (chromium_view_get_compositor (demo.view))
    {
      NestedCompositor *compositor = chromium_view_get_compositor (demo.view);

      g_signal_connect (compositor, "notify::surface",
                        G_CALLBACK (on_surface_notify), NULL);

      /* Connecting to ::drag-out is what tells the compositor that a host can
       * carry a page's drag out of the window. A run that wants the page to keep
       * its drag until the button lets go — which is what a host with no such
       * handler gets — says so with CEFNEST_DRAG_OUT=off. */
      if (g_strcmp0 (g_getenv ("CEFNEST_DRAG_OUT"), "off") != 0)
        g_signal_connect (compositor, "drag-out", G_CALLBACK (demo_drag_out), NULL);

      g_signal_connect (compositor, "drag-in-end",
                        G_CALLBACK (demo_drag_in_end), NULL);
    }

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (demo.view));
  gtk_widget_set_vexpand (GTK_WIDGET (demo.view), TRUE);
  gtk_window_set_child (GTK_WINDOW (demo.window), box);

  header = gtk_header_bar_new ();

  if (use_session)
    {
      /* The rows live in the window box rather than the header: a name and two
       * buttons need more room than a toolbar has. */
      build_find_bar (GTK_BOX (box));
      build_download_panel (GTK_BOX (box));
      build_permission_panel (GTK_BOX (box));
      build_toolbar (GTK_HEADER_BAR (header));

      /* Ctrl+F belongs to the window, not to the page: the widget forwards
       * every key it is given, so the bar has to take this one first. */
      {
        GtkEventController *keys = gtk_event_controller_key_new ();

        gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
        g_signal_connect (keys, "key-pressed", G_CALLBACK (find_key_pressed), NULL);
        g_signal_connect (keys, "key-released", G_CALLBACK (find_key_released), NULL);
        gtk_widget_add_controller (GTK_WIDGET (demo.window), keys);
      }
    }
  else
    {
      button = gtk_button_new_with_label ("启动客户端");
      g_signal_connect (button, "clicked", G_CALLBACK (spawn_client), NULL);
      gtk_header_bar_pack_start (GTK_HEADER_BAR (header), button);
    }

  button = gtk_button_new_with_label ("停止");
  g_signal_connect (button, "clicked", G_CALLBACK (stop_client), NULL);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), button);

  gtk_window_set_titlebar (GTK_WINDOW (demo.window), header);
  gtk_window_set_default_size (GTK_WINDOW (demo.window), 1000, 700);
  /* Deterministic window geometry, so an injected pointer can be clamped
   * against known corners and edges when checking the seat coordinates. */
  {
    const char *mode = g_getenv ("CHROMIUM_WINDOW");

    if (mode && !strcmp (mode, "fullscreen"))
      gtk_window_fullscreen (demo.window);
    else if (mode && !strcmp (mode, "maximize"))
      gtk_window_maximize (demo.window);
  }
  gtk_window_present (GTK_WINDOW (demo.window));

  update_title ();
  report ();
  setup_clipboard_test ();
  if (!use_session)
    spawn_client (NULL, NULL);
  start_session_script ();

  {
    const char *path = g_getenv ("CHROMIUM_DUMP");
    const char *delay = g_getenv ("CHROMIUM_DUMP_DELAY");
    unsigned int ms = delay && *delay ? (unsigned int) atoi (delay) : 2000;

    if (path && *path)
      g_timeout_add (ms, dump_view, (gpointer) path);
  }
}

int
main (int argc, char *argv[])
{
  GtkApplication *app;
  GPtrArray *args;
  int i;

  /* Everything after the program name is the inner client command line; keep
   * it away from GApplication, which would try to parse its own options. Copy
   * the strings: the array is released with g_strfreev(), which owns them. */
  args = g_ptr_array_new ();
  for (i = 1; i < argc; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  if (args->len == 0)
    g_ptr_array_add (args, g_strdup (demo_find_client ()));
  g_ptr_array_add (args, NULL);
  demo.client_argv = (char **) g_ptr_array_free (args, FALSE);

  app = demo.app = g_object_new (GTK_TYPE_APPLICATION,
                                 "application-id", "org.cefnest.Demo",
                                 "flags", G_APPLICATION_NON_UNIQUE,
                                 NULL);

  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (app_shutdown), NULL);

  /* Ctrl+C and `kill` reach us with the default disposition, which would drop
   * the process with its helper socket still bound — so a signal becomes a
   * quit, and the shutdown handler stops the helper on the way out. */
  g_unix_signal_add (SIGINT, signal_quit, app);
  g_unix_signal_add (SIGTERM, signal_quit, app);

  g_application_run (G_APPLICATION (app), 1, argv);

  g_object_unref (app);
  g_strfreev (demo.client_argv);
  /* Not a fixed 0: a scripted run cut short by its helper dying sets
   * demo.exit_status, and a harness only sees the difference if it is handed out. */
  return demo.exit_status;
}
