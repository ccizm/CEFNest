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

  /* Set when the page comes from a Chromium helper process rather than from a
   * plain Wayland client: then the header bar is a browser toolbar and the
   * session owns the process. */
  ChromiumSession *session;
  GtkWidget *address;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *downloads;
  GtkWidget *find_bar;
  GtkWidget *find_entry;
  GtkWidget *find_case;
  GtkWidget *find_label;
} Demo;

static Demo demo;

#ifndef CLIENT_PATH
#define CLIENT_PATH "shm-client"
#endif

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

static void
session_closed (ChromiumSession *session, gpointer data)
{
  if (demo.back)
    gtk_widget_set_sensitive (demo.back, FALSE);
  if (demo.forward)
    gtk_widget_set_sensitive (demo.forward, FALSE);
  update_title ();
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
  g_signal_connect (demo.session, "changed", G_CALLBACK (session_changed), NULL);
  g_signal_connect (demo.session, "load-end", G_CALLBACK (session_load_end), NULL);
  g_signal_connect (demo.session, "load-error",
                    G_CALLBACK (session_load_error), NULL);
  g_signal_connect (demo.session, "renderer-exited",
                    G_CALLBACK (session_renderer_exited), NULL);
  g_signal_connect (demo.session, "script-result",
                    G_CALLBACK (session_script_result), NULL);
  g_signal_connect (demo.session, "download-begin",
                    G_CALLBACK (session_download_begin), NULL);
  g_signal_connect (demo.session, "download-progress",
                    G_CALLBACK (session_download_progress), NULL);
  g_signal_connect (demo.session, "download-end",
                    G_CALLBACK (session_download_end), NULL);
  g_signal_connect (demo.session, "find-result",
                    G_CALLBACK (session_find_result), NULL);
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

/* CHROMIUM_SESSION_SCRIPT drives the control plane without anyone typing. One
 * step per line: "load <uri>", "back", "forward", "reload", "stop",
 * "js <code>", "wait <ms>", "shot <path>", "dump <path>", "hover <x> <y>",
 * "click <x> <y> [button]", "drag <x0> <y0> <x1> <y1> [button]",
 * "drag-hold <x0> <y0> <x1> <y1> [button]", "release [button]", "surfaces",
 * "wheel <dx> <dy>", "mute", "unmute", "download-accept <path>",
 * "download-reject", "find <pattern>", "find-case <pattern>", "find-next",
 * "find-prev", "find-stop",
 * "close", "quit". This is how a headless run proves navigation, history and
 * repaint all work together, and "close" is
 * how it proves the way out of the window stops the helper with it. A "js" step
 * is tagged with its own line number, which is what comes back on
 * ::script-result, so a result can be matched to the step that asked for it.
 * The page steps need a helper session; "hover", "click", "drag", "wheel",
 * "shot", "dump", "surfaces" and "close" work against any client, so the same
 * script drives the nested compositor without Chromium. */
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

static void
script_finish (void)
{
  g_clear_pointer (&session_script.steps, g_ptr_array_unref);
  session_script.index = 0;
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
                            !strncmp (line, "download-", 9) ||
                            !strncmp (line, "find", 4)))
        {
          g_warning ("demo: '%s' needs a Chromium helper session", line);
          continue;
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

  /* Give the helper time to report ready before the first command lands. */
  g_timeout_add (800, script_wait_done, NULL);
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

static void
activate (GApplication *application, gpointer data)
{
  const gboolean use_session = g_getenv ("CHROMIUM_HELPER") != NULL;
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
    g_signal_connect (chromium_view_get_compositor (demo.view), "notify::surface",
                      G_CALLBACK (on_surface_notify), NULL);

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
      build_toolbar (GTK_HEADER_BAR (header));

      /* Ctrl+F belongs to the window, not to the page: the widget forwards
       * every key it is given, so the bar has to take this one first. */
      {
        GtkEventController *keys = gtk_event_controller_key_new ();

        gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
        g_signal_connect (keys, "key-pressed", G_CALLBACK (find_key_pressed), NULL);
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
    g_ptr_array_add (args, g_strdup (CLIENT_PATH));
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
  return 0;
}
