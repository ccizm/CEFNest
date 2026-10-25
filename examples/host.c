#include <gtk/gtk.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* examples/minimal.c is the smallest thing that paints a page. This is the next
 * step, and the part an embedding app has to get right: four of the session's
 * signals are questions, and the page is standing on the answer.
 *
 *   ::download-begin      nothing lands on disk until the host names a path
 *   ::permission-request  the promise stays pending, and the next media ask never
 *                         arrives, until this one is answered
 *   ::script-result       run_javascript*() answers here, keyed by the id passed in
 *   ::context-menu        CEF holds one menu request at a time — and this file
 *                         deliberately does not connect it, so the menu that
 *                         works for free is visible next to the ones a host owns
 *
 * It also drives one helper that hosts several browsers: the tab row comes from
 * ::browser-created, clicking it is select_browser(), and CEF assigns the ids, so
 * a host keys its own map from the signal instead of guessing the order.
 *
 *   ./builddir/examples/cefnest-host https://cnb.cool/yanzhiming/CEFNest
 *
 * and with no keyboard at all, which is how this file was tested:
 *
 *   printf 'load %s/download?name=report.pdf&bytes=4096\nwait 800\nsave /tmp/dl\n' \
 *     'http://gate.test' > /tmp/steps
 *   CEFNEST_HOST_SCRIPT=/tmp/steps CEFNEST_HELPER=$PWD/test/fake-helper.py \
 *     ./test/ci-session.sh ./builddir/examples/cefnest-host http://gate.test/start
 *
 * One command per line: load, tab, select, close, js, save, drop, allow, deny,
 * wait, quit. Every step is echoed to the log pane and to stderr, so a headless
 * run asserts on what the host did. The script hook is the only part of this file
 * a real host would drop.
 *
 * What the runs on 2026-10-22 measured, in the nested session and nowhere else:
 * a saved download writes the file and ends with state=complete naming the path
 * the host chose; a refused one ends with state=cancelled and an empty path, and
 * leaves no file; a name the page tried to escape out of the directory with
 * ("../escape.pdf") is written as "escape.pdf" inside it, and a name that collides
 * with an existing file becomes "report (2).pdf"; an accepted media ask crosses
 * the wire as permission-respond result=accept. Against the real helper,
 * ::browser-created/::browser-selected drive the tab row and
 * js document.title comes back as script N = "DND-PAGE" — and close on a *tab*
 * does nothing: no ::browser-closed, the row stays. A standalone (windowed)
 * browser does close, and the selection falls back to the one that is left.
 * The tab half is a helper-side gap, not something a host can answer around.
 */
typedef struct {
  int id;
  char *name;
  char *text;
  GtkWidget *row;
  GtkWidget *label;
} Pending;

typedef struct {
  GtkWidget *window;
  GtkWidget *entry;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *reload;
  GtkWidget *status;
  GtkWidget *tabs;
  GtkWidget *downloads;
  GtkWidget *permissions;
  GtkWidget *log;
  ChromiumSession *session;
  GHashTable *tab_buttons;
  GList *pending_downloads;
  GList *pending_permissions;
  GQueue script;
  char *download_dir;
  int script_id;
  int active_browser;
  gboolean gone;
} Host;

static Host host;

static void script_run (void);

static void
note (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *line = NULL;
  GtkTextBuffer *buffer;
  GtkTextIter end;

  va_start (ap, fmt);
  line = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  /* The pane is what a person watching the window reads, stderr is what a
   * headless run greps. A host needs one of the two. */
  g_printerr ("host: %s\n", line);

  if (!host.log)
    return;

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (host.log));
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_insert (buffer, &end, line, -1);
  gtk_text_buffer_insert (buffer, &end, "\n", -1);
  gtk_text_view_scroll_mark_onscreen (
    GTK_TEXT_VIEW (host.log), gtk_text_buffer_get_mark (buffer, "insert"));
}

static void
set_status (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *line = NULL;

  va_start (ap, fmt);
  line = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  if (host.status)
    gtk_label_set_text (GTK_LABEL (host.status), line);
}

/* ::changed carries nothing on purpose: there is one place that knows what the
 * page looks like, and it is the getters, not the signal arguments. */
static void
refresh_toolbar (void)
{
  const char *uri = chromium_session_get_uri (host.session);
  const char *title = chromium_session_get_title (host.session);

  if (!host.window)
    return;

  gtk_widget_set_sensitive (host.back,
                            chromium_session_get_can_go_back (host.session));
  gtk_widget_set_sensitive (host.forward,
                            chromium_session_get_can_go_forward (host.session));
  gtk_widget_set_sensitive (host.reload, !host.gone);

  /* The entry is where a person types the next address; a page reporting its own
   * URL must not overwrite a half-typed one. */
  if (uri && !gtk_widget_is_focus (host.entry))
    gtk_editable_set_text (GTK_EDITABLE (host.entry), uri);

  if (chromium_session_get_loading (host.session))
    set_status ("loading — %s", uri ? uri : "(no url)");
  else if (title && *title)
    set_status ("%s — %s", title, uri ? uri : "(no url)");
  else
    set_status ("%s", uri ? uri : "idle");
}

static void
session_changed (ChromiumSession *session, gpointer data)
{
  refresh_toolbar ();
}

static void
session_load_end (ChromiumSession *session, const char *uri, int http_code,
                  gpointer data)
{
  /* HTTP code 0 also means "this navigation never committed", and ::load-error
   * is the one that says why for the same navigation. */
  note ("load-end %s HTTP %d", uri ? uri : "(none)", http_code);
  refresh_toolbar ();
}

static void
session_load_error (ChromiumSession *session, int error_code, const char *uri,
                    const char *message, gpointer data)
{
  note ("load-error %d %s %s", error_code, uri ? uri : "(none)",
        message ? message : "");
  set_status ("did not load: %s", message ? message : "unknown reason");
}

static void
session_renderer_exited (ChromiumSession *session, int status, int error_code,
                         const char *message, gpointer data)
{
  /* Nothing else reports this. The surface keeps the last frame and later
   * commands go unanswered, so "the page stopped moving" is the symptom. */
  note ("renderer exited status=%d error=%d %s", status, error_code,
        message ? message : "");
  set_status ("the page's render process is gone (status %d)", status);
}

static void
session_script_result (ChromiumSession *session, int id, gboolean ok,
                       const char *value, const char *message, gpointer data)
{
  /* The answer to run_javascript*(), keyed by the id the host passed in. @value
   * is JSON text — "null" for a script that returned nothing. */
  if (ok)
    note ("script %d = %s", id, value ? value : "(nothing)");
  else
    note ("script %d failed: %s", id, message ? message : "no reason given");
}

/* ------------------------------------------------------------------ answers */

static Pending *
pending_add (GtkWidget *box, GList **list, int id, const char *name)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *label = gtk_label_new (name);
  Pending *pending = g_new0 (Pending, 1);

  gtk_widget_set_hexpand (label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (row), label);
  gtk_box_append (GTK_BOX (box), row);

  pending->id = id;
  pending->name = g_strdup (name);
  pending->text = g_strdup (name);
  pending->row = row;
  pending->label = label;
  *list = g_list_append (*list, pending);
  return pending;
}

static void
pending_set_text (Pending *pending, const char *fmt, ...)
{
  va_list ap;
  g_autofree char *line = NULL;

  va_start (ap, fmt);
  line = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  g_free (pending->text);
  pending->text = g_steal_pointer (&line);
  gtk_label_set_text (GTK_LABEL (pending->label), pending->text);
}

static void
pending_free (Pending *pending)
{
  g_free (pending->name);
  g_free (pending->text);
  g_free (pending);
}

static void
pending_remove (GtkWidget *box, GList **list, Pending *pending)
{
  GList *link = g_list_find (*list, pending);

  if (!link)
    return;
  *list = g_list_delete_link (*list, link);
  gtk_box_remove (GTK_BOX (box), pending->row);
  pending_free (pending);
}

/* The window owns every widget listed in Host, so once it is destroyed they are
 * all dangling — and the session can still answer one last time after that, if
 * only through ::closed. NULL is the flag the handlers below read. */
static void
on_window_destroy (GtkWindow *window, gpointer data)
{
  /* Only memory: the widgets it names are going away with the window, whatever
   * order GTK destroys them in. */
  g_list_free_full (host.pending_downloads, (GDestroyNotify) pending_free);
  g_list_free_full (host.pending_permissions, (GDestroyNotify) pending_free);
  host.pending_downloads = NULL;
  host.pending_permissions = NULL;
  g_hash_table_remove_all (host.tab_buttons);

  host.window = NULL;
  host.entry = NULL;
  host.back = NULL;
  host.forward = NULL;
  host.reload = NULL;
  host.status = NULL;
  host.tabs = NULL;
  host.downloads = NULL;
  host.permissions = NULL;
  host.log = NULL;
}

/* The host owns the path, so the host owns the sanitising: the name came from a
 * page. */
static char *
download_path (const char *dir, const char *suggested)
{
  g_autofree char *name = NULL;
  const char *dot;
  int n;

  name = g_path_get_basename (suggested && *suggested ? suggested : "download");
  if (!strcmp (name, ".") || !strcmp (name, ".."))
    {
      g_free (name);
      name = g_strdup ("download");
    }

  /* The extension is where a " (1)" goes, so the name still opens in the
   * application that reads the type. */
  dot = strrchr (name, '.');
  for (n = 0; n < 999; n++)
    {
      g_autofree char *candidate = n == 0 ? g_strdup (name) :
        (dot && dot > name ? g_strdup_printf ("%.*s (%d)%s",
                                              (int) (dot - name), name, n, dot) :
                             g_strdup_printf ("%s (%d)", name, n));
      g_autofree char *full = g_build_filename (dir, candidate, NULL);

      /* Tested as the whole path: the bare name would be looked for in the
       * host's own working directory, which is not where the file goes. */
      if (!g_file_test (full, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&full);
    }

  /* 999 candidates exhausted: the demo's names carry a timestamp rather than
   * handing back a name that is already there and letting the write replace
   * it. */
  {
    g_autofree char *candidate = g_strdup_printf ("%s-%" G_GINT64_FORMAT, name,
                                                  g_get_real_time ());
    return g_build_filename (dir, candidate, NULL);
  }
}

static void
download_accept (Pending *pending)
{
  const char *dir = host.download_dir ? host.download_dir : g_get_home_dir ();
  g_autofree char *path = NULL;

  /* The helper makes no parent directory: naming the path is the whole job of
   * this half. */
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      note ("download %d cannot write to %s", pending->id, dir);
      return;
    }

  path = download_path (dir, pending->name);
  note ("download %d -> %s", pending->id, path);
  chromium_session_accept_download (host.session, pending->id, path);
}

static void
on_download_accept (GtkButton *button, gpointer data)
{
  download_accept ((Pending *) data);
}

static void
on_download_reject (GtkButton *button, gpointer data)
{
  Pending *pending = data;

  note ("download %d refused", pending->id);
  /* One call covers both: it refuses a pending download and cancels an in-flight
   * one. The difference is only whether a file was ever opened. */
  chromium_session_reject_download (host.session, pending->id);
}

static void
session_download_begin (ChromiumSession *session, int id, const char *url,
                        const char *suggested_name, const char *mime_type,
                        gint64 total_bytes, gpointer data)
{
  Pending *pending;
  GtkWidget *accept;
  GtkWidget *reject;

  if (!host.window)
    {
      /* The window is gone, so there is nowhere to put the question. Leaving it
       * unanswered is what writes no file. */
      note ("download-begin %d after the window closed: refused by silence", id);
      return;
    }

  /* Nothing has been written yet and nothing will be until this row is answered.
   * @total_bytes is -1 when the server did not state a length. */
  pending = pending_add (host.downloads, &host.pending_downloads, id,
                         suggested_name && *suggested_name ? suggested_name :
                         "download");
  pending_set_text (pending, "%s (%s, %s)",
                    suggested_name && *suggested_name ? suggested_name : "download",
                    mime_type && *mime_type ? mime_type : "unknown type",
                    total_bytes < 0 ? "size unknown" : "a path is needed");

  accept = gtk_button_new_with_label ("Save");
  reject = gtk_button_new_with_label ("Discard");
  g_signal_connect (accept, "clicked", G_CALLBACK (on_download_accept), pending);
  g_signal_connect (reject, "clicked", G_CALLBACK (on_download_reject), pending);
  gtk_box_append (GTK_BOX (pending->row), accept);
  gtk_box_append (GTK_BOX (pending->row), reject);

  note ("download-begin %d %s from %s", id,
        suggested_name && *suggested_name ? suggested_name : "?",
        url ? url : "(no url)");
}

static void
session_download_progress (ChromiumSession *session, int id, gint64 received,
                           gint64 total, int percent, gint64 speed,
                           gpointer data)
{
  GList *l;

  for (l = host.pending_downloads; l; l = l->next)
    {
      Pending *pending = l->data;

      if (pending->id != id)
        continue;

      /* @percent is -1 for a server that never stated a length, so the row has
       * to say bytes moved instead of a fraction it cannot compute. */
      if (percent < 0)
        {
          pending_set_text (pending, "%s — %" G_GINT64_FORMAT " B, no length stated",
                            pending->name, received);
          note ("download-progress %d %" G_GINT64_FORMAT " B, length unknown",
                id, received);
        }
      else
        {
          pending_set_text (pending, "%s — %d %%, %" G_GINT64_FORMAT " B/s",
                            pending->name, percent, speed);
          note ("download-progress %d %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
                " B, %d %%, %" G_GINT64_FORMAT " B/s", id, received, total,
                percent, speed);
        }
    }
}

static void
session_download_end (ChromiumSession *session, int id, const char *state,
                      const char *path, gint64 received, gpointer data)
{
  GList *l;

  /* Both the accept and the refuse route finish here, and @path is what Chromium
   * actually wrote — not necessarily the name that was asked for, because the
   * helper uniquifies on the way to disk. This is the one place a host tracking
   * rows learns how it ended, so it is said before the row goes away. */
  note ("download-end %d state=%s path=%s bytes=%" G_GINT64_FORMAT, id,
        state ? state : "?", path && *path ? path : "(none)", received);

  for (l = host.pending_downloads; l; l = l->next)
    {
      Pending *pending = l->data;

      if (pending->id == id)
        {
          pending_remove (host.downloads, &host.pending_downloads, pending);
          break;
        }
    }
}

/* --------------------------------------------------------------- permissions */

static void
permission_answer (Pending *pending, ChromiumPermissionResult result)
{
  note ("permission %d answered %s", pending->id,
        result == CHROMIUM_PERMISSION_ACCEPT ? "accept" :
        result == CHROMIUM_PERMISSION_DENY ? "deny" : "dismiss");
  chromium_session_permission_respond (host.session, pending->id, result);
}

static void
on_permission_allow (GtkButton *button, gpointer data)
{
  permission_answer ((Pending *) data, CHROMIUM_PERMISSION_ACCEPT);
}

static void
on_permission_deny (GtkButton *button, gpointer data)
{
  permission_answer ((Pending *) data, CHROMIUM_PERMISSION_DENY);
}

static gboolean
session_permission_request (ChromiumSession *session, int id, const char *kind,
                            const char *origin, GList *names, int raw,
                            gpointer data)
{
  g_autoptr (GString) list = g_string_new (NULL);
  Pending *pending;
  GtkWidget *allow;
  GtkWidget *deny;
  gsize n = g_list_length (names);
  gsize i;

  if (!host.window)
    {
      /* FALSE is the answer a host with no UI gives: the session denies rather
       * than leaving the page's promise pending. */
      note ("permission-request %d after the window closed: denied", id);
      return FALSE;
    }

  /* The list belongs to the session for the duration of this emission; a host
   * that wants it later copies it. */
  for (i = 0; i < n; i++)
    {
      if (i)
        g_string_append (list, ", ");
      g_string_append (list, (const char *) g_list_nth_data (names, i));
    }

  /* Returning TRUE is the contract: this row is now the only way the question
   * can be answered, so exactly one of the two buttons has to be pressed. An
   * unanswered media ask also holds the media queue, so the page's *next* ask
   * never arrives. Returning FALSE — or connecting nothing — gets a deny from the
   * session. */
  pending = pending_add (host.permissions, &host.pending_permissions, id,
                         "asking");
  pending_set_text (pending, "%s wants %s: %s", origin ? origin : "a page",
                    kind && *kind ? kind : "permission",
                    list->len ? list->str : "(nothing named)");

  allow = gtk_button_new_with_label ("Allow");
  deny = gtk_button_new_with_label ("Deny");
  g_signal_connect (allow, "clicked", G_CALLBACK (on_permission_allow), pending);
  g_signal_connect (deny, "clicked", G_CALLBACK (on_permission_deny), pending);
  gtk_box_append (GTK_BOX (pending->row), allow);
  gtk_box_append (GTK_BOX (pending->row), deny);

  note ("permission-request %d kind=%s raw=%d [%s]", id, kind ? kind : "?", raw,
        list->len ? list->str : "");
  return TRUE;
}

static void
session_permission_dismissed (ChromiumSession *session, int id,
                              const char *result, gpointer data)
{
  GList *l;

  for (l = host.pending_permissions; l; l = l->next)
    {
      Pending *pending = l->data;

      if (pending->id == id)
        {
          pending_remove (host.permissions, &host.pending_permissions, pending);
          break;
        }
    }

  /* A prompt Chromium took back from under the host: answering it does nothing
   * now, and the row would stay on screen offering a dead button. Media asks have
   * no dismiss callback at all, which is why the two kinds are different rows in
   * the session and the same two here. */
  note ("permission-dismissed %d result=%s", id, result ? result : "?");
}

/* ---------------------------------------------------------------------- tabs */

static void
mark_active_tab (int id)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  /* remembered rather than only applied: on a first run ::browser-selected can
   * arrive before the ::browser-created it names, and then the highlight has to
   * be put on when the button does show up. */
  host.active_browser = id;

  g_hash_table_iter_init (&iter, host.tab_buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GtkWidget *button = value;

      if (GPOINTER_TO_INT (key) == id)
        gtk_widget_add_css_class (button, "suggested-action");
      else
        gtk_widget_remove_css_class (button, "suggested-action");
    }
}

static void
on_tab_clicked (GtkButton *button, gpointer data)
{
  chromium_session_select_browser (
    host.session, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
                                                      "browser-id")));
}

static void
on_tab_close (GtkButton *button, gpointer data)
{
  GtkWidget *row = gtk_widget_get_parent (GTK_WIDGET (button));

  /* Measured: for a browser in the shared tab window this closes nothing — no
   * ::browser-closed, and the row stays. A windowed browser closes and reports
   * itself. The gap is on the helper's side of the wire. */
  chromium_session_close_browser (
    host.session,
    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "browser-id")));
}

static void
session_browser_created (ChromiumSession *session, int id, const char *url,
                         gboolean windowed, gboolean devtools, gpointer data)
{
  GtkWidget *row;
  GtkWidget *button;
  GtkWidget *close;

  if (!host.window)
    {
      note ("browser-created %d after the window closed", id);
      return;
    }

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  button = gtk_button_new_with_label (url && *url ? url
                                                   : (windowed ? "window" : "blank"));
  close = gtk_button_new_from_icon_name ("window-close-symbolic");

  /* CEF assigns @id, so it is read off the signal rather than counted here: the
   * order the answers come in is not the order the requests went out. */
  g_object_set_data (G_OBJECT (button), "browser-id", GINT_TO_POINTER (id));
  g_object_set_data (G_OBJECT (row), "browser-id", GINT_TO_POINTER (id));
  g_hash_table_insert (host.tab_buttons, GINT_TO_POINTER (id), button);

  gtk_widget_set_tooltip_text (button, windowed ? "standalone browser" : "tab");
  g_signal_connect (button, "clicked", G_CALLBACK (on_tab_clicked), NULL);
  g_signal_connect (close, "clicked", G_CALLBACK (on_tab_close), NULL);
  gtk_box_append (GTK_BOX (row), button);
  gtk_box_append (GTK_BOX (row), close);
  gtk_box_append (GTK_BOX (host.tabs), row);

  if (id == host.active_browser)
    gtk_widget_add_css_class (button, "suggested-action");

  note ("browser-created %d windowed=%d %s", id, windowed, url ? url : "(none)");
}

static void
session_browser_selected (ChromiumSession *session, int id, gpointer data)
{
  mark_active_tab (id);
  note ("browser-selected %d", id);
}

static void
session_browser_closed (ChromiumSession *session, int id, gpointer data)
{
  GtkWidget *button = g_hash_table_lookup (host.tab_buttons,
                                           GINT_TO_POINTER (id));

  if (button)
    {
      GtkWidget *row = gtk_widget_get_parent (button);

      g_hash_table_remove (host.tab_buttons, GINT_TO_POINTER (id));
      if (row)
        gtk_box_remove (GTK_BOX (host.tabs), row);
    }
  note ("browser-closed %d", id);
}

static void
on_new_tab (GtkButton *button, gpointer data)
{
  const char *typed = gtk_editable_get_text (GTK_EDITABLE (host.entry));

  /* FALSE is the tab mode: another browser inside the same helper, painted by the
   * same view. TRUE would be a standalone CEF window, which needs its own GTK
   * window over the shared compositor — see chromium_view_new_shared() and the
   * demo. */
  chromium_session_new_browser (host.session,
                                typed && *typed ? typed : "about:blank", FALSE);
}

static void
session_open_url (ChromiumSession *session, const char *url, gpointer data)
{
  /* ::open-url is host policy: a menu row said "open this in a new window" and
   * Chromium is not the one that decides what a window means in this app. */
  note ("open-url %s -> a new tab", url ? url : "(none)");
  if (url && *url)
    chromium_session_new_browser (host.session, url, FALSE);
}

static void
session_copy_text (ChromiumSession *session, const char *text, gpointer data)
{
  GdkClipboard *clipboard;

  if (!text || !host.window)
    return;

  clipboard = gdk_display_get_clipboard (gtk_widget_get_display (host.window));
  gdk_clipboard_set_text (clipboard, text);
  note ("copy-text %s", text);
}

static void
session_ready (ChromiumSession *session, gpointer data)
{
  note ("helper ready, using %s", chromium_session_get_helper_path (session));
  refresh_toolbar ();

  /* From ::ready rather than from a timer: a cold profile takes seconds to report,
   * and a first load sent before the channel is open is simply lost. */
  if (!g_queue_is_empty (&host.script))
    script_run ();
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  host.gone = TRUE;

  while (host.pending_downloads)
    pending_remove (host.downloads, &host.pending_downloads,
                    host.pending_downloads->data);
  while (host.pending_permissions)
    pending_remove (host.permissions, &host.pending_permissions,
                    host.pending_permissions->data);

  /* It never comes back on this session: a new page needs start() again. */
  note ("the helper is gone: %s", reason ? reason : "(no reason)");
  set_status ("no browser (%s)", reason ? reason : "?");
}

/* -------------------------------------------------------------------- script */

static void
script_resume (gpointer data)
{
  script_run ();
}

static void
answer_all_downloads (gboolean accept)
{
  GList *links = host.pending_downloads;

  while (links)
    {
      Pending *pending = links->data;

      links = links->next;
      if (accept)
        download_accept (pending);
      else
        {
          note ("download %d refused", pending->id);
          chromium_session_reject_download (host.session, pending->id);
        }
    }
}

static void
answer_all_permissions (ChromiumPermissionResult result)
{
  GList *links = host.pending_permissions;

  while (links)
    {
      Pending *pending = links->data;

      links = links->next;
      permission_answer (pending, result);
    }
}

static void
script_run (void)
{
  while (!g_queue_is_empty (&host.script))
    {
      g_autofree char *line = g_queue_pop_head (&host.script);
      char verb[16];
      char rest[1024];
      int n = 0;

      verb[0] = '\0';
      rest[0] = '\0';
      sscanf (line, "%15s %1023[^\n]", verb, rest);
      if (!*verb)
        continue;

      note ("script: %s", line);

      if (!strcmp (verb, "wait"))
        {
          if (sscanf (rest, "%d", &n) != 1)
            n = 500;
          g_timeout_add_once (n < 0 ? 0 : (guint) n, script_resume, NULL);
          return;
        }
      else if (!strcmp (verb, "load"))
        chromium_session_load_uri (host.session, rest);
      else if (!strcmp (verb, "tab"))
        chromium_session_new_browser (host.session, *rest ? rest : "about:blank",
                                      FALSE);
      else if (!strcmp (verb, "select") && sscanf (rest, "%d", &n) == 1)
        chromium_session_select_browser (host.session, n);
      else if (!strcmp (verb, "close") && sscanf (rest, "%d", &n) == 1)
        chromium_session_close_browser (host.session, n);
      else if (!strcmp (verb, "js"))
        chromium_session_run_javascript_expression (host.session, rest,
                                                    ++host.script_id);
      else if (!strcmp (verb, "save"))
        {
          g_free (host.download_dir);
          host.download_dir = *rest ? g_strdup (rest) : NULL;
          answer_all_downloads (TRUE);
        }
      else if (!strcmp (verb, "drop"))
        answer_all_downloads (FALSE);
      else if (!strcmp (verb, "allow"))
        answer_all_permissions (CHROMIUM_PERMISSION_ACCEPT);
      else if (!strcmp (verb, "deny"))
        answer_all_permissions (CHROMIUM_PERMISSION_DENY);
      else if (!strcmp (verb, "quit"))
        {
          gtk_window_destroy (GTK_WINDOW (host.window));
          return;
        }
      else
        note ("script: \"%s\" is not load tab select close js save drop allow "
              "deny wait quit", verb);
    }
}

static void
load_script (const char *path)
{
  g_autofree char *text = NULL;
  char **lines;
  gsize i;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    {
      note ("cannot read the script %s", path);
      return;
    }

  lines = g_strsplit (text, "\n", -1);
  for (i = 0; lines[i]; i++)
    {
      if (lines[i][0])
        g_queue_push_tail (&host.script, g_strdup (lines[i]));
    }
  g_strfreev (lines);
}

/* ------------------------------------------------------------------- wiring */

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  chromium_session_load_uri (host.session,
                             gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_back (GtkButton *button, gpointer data)
{
  chromium_session_go_back (host.session);
}

static void
on_forward (GtkButton *button, gpointer data)
{
  chromium_session_go_forward (host.session);
}

static void
on_reload (GtkButton *button, gpointer data)
{
  chromium_session_reload (host.session, FALSE);
}

static void
on_stop (GtkButton *button, gpointer data)
{
  chromium_session_stop_loading (host.session);
}

static void
on_read_title (GtkButton *button, gpointer data)
{
  /* The CSP-safe form: the code is embedded as an expression and wrapped without
   * eval(), so a page whose script-src forbids unsafe-eval still answers. The
   * answer is ::script-result, with this id. */
  chromium_session_run_javascript_expression (host.session, "document.title",
                                              ++host.script_id);
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (host.session);
  return FALSE;
}

static GtkWidget *
add_button (GtkWidget *box, const char *label, GCallback callback)
{
  GtkWidget *button = gtk_button_new_with_label (label);

  gtk_box_append (GTK_BOX (box), button);
  g_signal_connect (button, "clicked", callback, NULL);
  return button;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *view;
  GtkWidget *scroller;
  GtkWidget *log;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");
  const char *script = g_getenv ("CEFNEST_HOST_SCRIPT");

  host.tab_buttons = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_queue_init (&host.script);
  if (script)
    load_script (script);

  host.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (host.window), "CEFNest host");
  gtk_window_set_default_size (GTK_WINDOW (host.window), 1100, 800);

  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  host.back = add_button (toolbar, "←", G_CALLBACK (on_back));
  host.forward = add_button (toolbar, "→", G_CALLBACK (on_forward));
  host.reload = add_button (toolbar, "reload", G_CALLBACK (on_reload));
  add_button (toolbar, "stop", G_CALLBACK (on_stop));

  host.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (host.entry), "address");
  gtk_widget_set_hexpand (host.entry, TRUE);
  g_signal_connect (host.entry, "activate", G_CALLBACK (on_entry_activate),
                    NULL);
  gtk_box_append (GTK_BOX (toolbar), host.entry);
  add_button (toolbar, "New tab", G_CALLBACK (on_new_tab));
  add_button (toolbar, "Read title", G_CALLBACK (on_read_title));
  gtk_box_append (GTK_BOX (root), toolbar);

  host.tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (host.tabs, 6);
  gtk_box_append (GTK_BOX (root), host.tabs);

  /* The two boxes a host answers into. They hold rows only while a question is
   * outstanding, so a page that asks nothing costs nothing. */
  host.downloads = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  host.permissions = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (host.downloads, 6);
  gtk_widget_set_margin_start (host.permissions, 6);
  gtk_box_append (GTK_BOX (root), host.permissions);
  gtk_box_append (GTK_BOX (root), host.downloads);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (view, TRUE);
  gtk_box_append (GTK_BOX (root), view);

  host.status = gtk_label_new ("starting");
  gtk_label_set_xalign (GTK_LABEL (host.status), 0.0);
  gtk_box_append (GTK_BOX (root), host.status);

  scroller = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scroller, -1, 130);
  log = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (log), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (log), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (log), GTK_WRAP_WORD_CHAR);
  host.log = log;
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), log);
  gtk_box_append (GTK_BOX (root), scroller);

  gtk_window_set_child (GTK_WINDOW (host.window), root);

  /* Not a style choice: the view has to be in the window before the session
   * starts, because the helper takes its WAYLAND_DISPLAY from the compositor this
   * view owns. */
  host.session = chromium_session_new (NULL);
  chromium_session_set_view (host.session, CHROMIUM_VIEW (view));

  g_signal_connect (host.session, "changed", G_CALLBACK (session_changed), NULL);
  g_signal_connect (host.session, "ready", G_CALLBACK (session_ready), NULL);
  g_signal_connect (host.session, "load-end", G_CALLBACK (session_load_end),
                    NULL);
  g_signal_connect (host.session, "load-error", G_CALLBACK (session_load_error),
                    NULL);
  g_signal_connect (host.session, "renderer-exited",
                    G_CALLBACK (session_renderer_exited), NULL);
  g_signal_connect (host.session, "script-result",
                    G_CALLBACK (session_script_result), NULL);
  g_signal_connect (host.session, "download-begin",
                    G_CALLBACK (session_download_begin), NULL);
  g_signal_connect (host.session, "download-progress",
                    G_CALLBACK (session_download_progress), NULL);
  g_signal_connect (host.session, "download-end",
                    G_CALLBACK (session_download_end), NULL);
  g_signal_connect (host.session, "permission-request",
                    G_CALLBACK (session_permission_request), NULL);
  g_signal_connect (host.session, "permission-dismissed",
                    G_CALLBACK (session_permission_dismissed), NULL);
  g_signal_connect (host.session, "browser-created",
                    G_CALLBACK (session_browser_created), NULL);
  g_signal_connect (host.session, "browser-selected",
                    G_CALLBACK (session_browser_selected), NULL);
  g_signal_connect (host.session, "browser-closed",
                    G_CALLBACK (session_browser_closed), NULL);
  g_signal_connect (host.session, "open-url", G_CALLBACK (session_open_url),
                    NULL);
  g_signal_connect (host.session, "copy-text", G_CALLBACK (session_copy_text),
                    NULL);
  g_signal_connect (host.session, "closed", G_CALLBACK (session_closed), NULL);

  /* ::context-menu is not connected on purpose. Returning FALSE, or connecting
   * nothing, leaves the request to the session, which pops its own menu over the
   * view — so a right click works in an app that never wrote a line of menu code.
   * A host that wants its own menu connects it, returns TRUE, and then owes
   * exactly one context_menu_select() or context_menu_dismiss(). */

  if (data && ((char **) data)[0])
    url = ((char **) data)[0];

  gtk_window_present (GTK_WINDOW (host.window));

  if (!chromium_session_start (host.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      note ("cannot start the helper: %s",
            error && error->message ? error->message : "?");
      g_clear_error (&error);
      set_status ("no helper");
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (host.window), "session",
                          g_object_ref (host.session), g_object_unref);
  g_signal_connect (host.window, "close-request", G_CALLBACK (on_close), NULL);
  g_signal_connect (host.window, "destroy", G_CALLBACK (on_window_destroy),
                    NULL);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (host.session)
    chromium_session_stop (host.session);
  g_clear_pointer (&host.download_dir, g_free);
  g_clear_pointer (&host.tab_buttons, g_hash_table_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.host",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  /* The URL goes over as data rather than through g_application_run()'s own
   * parser, which would read it as an unknown option. */
  g_signal_connect (app, "activate", G_CALLBACK (on_activate),
                    argc > 1 ? argv + 1 : NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), 0, NULL);
  g_object_unref (app);
  return status;
}
