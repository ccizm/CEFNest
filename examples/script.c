#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Scripting. A host runs JavaScript two ways — run_javascript() for
 * statement-shaped code, run_javascript_expression() for a bare expression on
 * pages whose CSP forbids unsafe-eval — and the answer is ::script-result,
 * keyed by the id the host passed in. The other direction is cefnestPostMessage
 * (json), which the renderer registers in every context; the host receives it
 * on ::page-message and answers through run_javascript() + a MessageEvent.
 *
 * The demo is a mini console: type an expression, read the JSON answer, and
 * watch a page-ping cross both directions of the message path.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-script
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *input;    /* the expression entry */
  GtkWidget *log;      /* the console output */
  ChromiumSession *session;
  int next_id;
} Script;

static Script script;

static void
log_line (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *line = NULL;
  GtkTextBuffer *buffer;
  GtkTextIter end;

  va_start (ap, fmt);
  line = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  if (!script.log)
    return;
  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (script.log));
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_insert (buffer, &end, line, -1);
  gtk_text_buffer_insert (buffer, &end, "\n", -1);
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (script.log),
                                      gtk_text_buffer_get_mark (buffer,
                                                                "insert"));
}

static void
session_script_result (ChromiumSession *session, int id, gboolean ok,
                       const char *value, const char *message, gpointer data)
{
  /* @value is JSON text — "null" for a script that returned nothing. @message
   * explains a failure, a result the page could not serialise, or a result too
   * large for the channel. A script that navigates the page away may never
   * answer at all. */
  if (ok)
    log_line ("[%d] = %s", id, value ? value : "(nothing)");
  else
    log_line ("[%d] failed: %s", id, message ? message : "no reason given");
}

static void
session_page_message (ChromiumSession *session, const char *json,
                      gpointer data)
{
  /* The schema inside is the host's; this channel carries the string whole. */
  log_line ("page → host: %s", json ? json : "");
}

static void
session_ready (ChromiumSession *session, gpointer data)
{
  log_line ("helper ready: %s", chromium_session_get_helper_path (session));
}

static void
on_run (GtkButton *button, gpointer data)
{
  const char *code = gtk_editable_get_text (GTK_EDITABLE (script.input));

  if (!code || !*code)
    return;
  /* The CSP-safe form: the code is embedded as an expression and wrapped
   * without eval(). */
  chromium_session_run_javascript_expression (script.session, code,
                                              ++script.next_id);
}

static void
on_ping (GtkButton *button, gpointer data)
{
  /* The page → host direction, driven from the host: run code in the page that
   * calls the bridge the renderer registered. The ::page-message above is what
   * comes back. */
  chromium_session_run_javascript (script.session,
      "cefnestPostMessage (JSON.stringify ({kind: 'host-ping',"
      " title: document.title}))",
      ++script.next_id);
}

static void
on_pong (GtkButton *button, gpointer data)
{
  /* Host → page: no dedicated command exists, so dispatch a MessageEvent. The
   * page hears it on window 'message'. */
  chromium_session_run_javascript (script.session,
      "window.dispatchEvent (new MessageEvent ('message',"
      " { data: { kind: 'host-pong' } }))",
      ++script.next_id);
  log_line ("host → page: message event dispatched");
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (script.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *run = gtk_button_new_with_label ("Run");
  GtkWidget *ping = gtk_button_new_with_label ("Ping page");
  GtkWidget *pong = gtk_button_new_with_label ("Send event");
  GtkWidget *scroller;
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  script.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (script.window), "CEFNest script");
  gtk_window_set_default_size (GTK_WINDOW (script.window), 1000, 700);

  script.input = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (script.input),
                                  "1 + 1  /  document.title  /  location.href");
  gtk_widget_set_hexpand (script.input, TRUE);
  g_signal_connect (script.input, "activate", G_CALLBACK (on_run), NULL);
  gtk_box_append (GTK_BOX (toolbar), script.input);
  g_signal_connect (run, "clicked", G_CALLBACK (on_run), NULL);
  gtk_box_append (GTK_BOX (toolbar), run);
  g_signal_connect (ping, "clicked", G_CALLBACK (on_ping), NULL);
  gtk_widget_set_tooltip_text (ping,
                               "run cefnestPostMessage() in the page");
  gtk_box_append (GTK_BOX (toolbar), ping);
  g_signal_connect (pong, "clicked", G_CALLBACK (on_pong), NULL);
  gtk_widget_set_tooltip_text (pong, "dispatch a MessageEvent into the page");
  gtk_box_append (GTK_BOX (toolbar), pong);
  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));

  scroller = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scroller, -1, 120);
  script.log = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (script.log), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (script.log), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), script.log);
  gtk_box_append (GTK_BOX (root), scroller);
  gtk_window_set_child (GTK_WINDOW (script.window), root);

  script.session = chromium_session_new (NULL);
  chromium_session_set_view (script.session, view);

  g_signal_connect (script.session, "ready", G_CALLBACK (session_ready), NULL);
  g_signal_connect (script.session, "script-result",
                    G_CALLBACK (session_script_result), NULL);
  g_signal_connect (script.session, "page-message",
                    G_CALLBACK (session_page_message), NULL);
  g_signal_connect (script.window, "close-request", G_CALLBACK (on_close),
                    NULL);

  gtk_window_present (GTK_WINDOW (script.window));

  if (!chromium_session_start (script.session,
                               url && *url ? url
                                           : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  g_object_set_data_full (G_OBJECT (script.window), "session",
                          g_object_ref (script.session), g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.script",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
