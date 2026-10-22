/* cefnest-useragent — show and switch the browser's User-Agent.
 *
 * The User-Agent is baked into the helper's Chromium at startup
 * (CefSettings.user_agent), so switching means restarting the session with a
 * different CHROMIUM_USER_AGENT in the host environment: type a UA (or pick a
 * preset), press Apply, and the helper goes down and comes back up reporting
 * the new identity. https://tool.lu/useragent/ shows what arrived — both the
 * HTTP header and what the page's JavaScript sees.
 *
 *   CEFNEST_UA="Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X)..." \
 *     ./builddir/examples/cefnest-useragent
 *
 * The page can also be pointed elsewhere with CEFNEST_URL.
 */

#include <gtk/gtk.h>

#include "chromium-view.h"
#include "chromium-session.h"

#define DEFAULT_PAGE "https://tool.lu/useragent/"

static const struct
{
  const char *label;
  const char *ua;
} presets[] = {
  { "Chromium 默认", "" },
  { "iPhone Safari",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 "
    "Safari/604.1" },
  { "iPad Safari",
    "Mozilla/5.0 (iPad; CPU OS 17_5 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 "
    "Safari/604.1" },
  { "Firefox 桌面",
    "Mozilla/5.0 (X11; Linux x86_64; rv:127.0) Gecko/20100101 Firefox/127.0" },
  { "微信内置",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 "
    "MicroMessenger/8.0.49" },
};

static struct
{
  GtkWidget *window;
  GtkWidget *view;
  GtkWidget *entry;         /* custom UA the user typed */
  GtkWidget *status;        /* what the helper is running with right now */
  ChromiumSession *session;
  char url[512];
  char *pending_ua;         /* UA to install when ::closed arrives */
  guint restarting;         /* the Apply flow is in progress */
  gboolean closing;         /* the window is going away: ::closed must not
                               start() the helper again */
} ua;

static void
apply_ua (const char *text)
{
  const char *want = (text && *text) ? text : "";

  if (ua.restarting)
    return;
  ua.restarting++;

  /* The helper reads CHROMIUM_USER_AGENT only at CefInitialize, so the value
   * goes into the host environment and a fresh helper picks it up. */
  g_setenv ("CHROMIUM_USER_AGENT", want, TRUE);
  g_free (ua.pending_ua);
  ua.pending_ua = g_strdup (want);

  gtk_label_set_text (GTK_LABEL (ua.status),
                      want[0] ? "重启 helper 中…" : "恢复默认 UA，重启中…");
  chromium_session_stop (ua.session);   /* ::closed does the start() below */
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  GError *error = NULL;
  const char *ua_now;

  if (ua.closing)
    return;   /* the window is gone; this stop is shutdown, not a switch */

  g_debug ("closed (%s); restarting with the new UA", reason);

  /* After ::closed the session object is reusable: start() again. */
  if (!chromium_session_start (session, ua.url, &error))
    {
      g_warning ("cannot restart the helper: %s", error->message);
      g_clear_error (&error);
      gtk_label_set_text (GTK_LABEL (ua.status), "重启失败");
      ua.restarting = 0;
      return;
    }

  ua_now = g_getenv ("CHROMIUM_USER_AGENT");
  gtk_label_set_text (GTK_LABEL (ua.status),
                      (ua_now && *ua_now) ? ua_now
                                          : "Chromium 默认（未自定义）");
}

static void
session_ready (ChromiumSession *session, gpointer data)
{
  ua.restarting = 0;
  gtk_label_set_text (GTK_LABEL (ua.status), "已连接，页面加载中…");
}

static void
on_apply (GtkButton *button, gpointer data)
{
  apply_ua (gtk_editable_get_text (GTK_EDITABLE (ua.entry)));
}

static void
on_preset (GtkButton *button, gpointer data)
{
  gsize index = GPOINTER_TO_SIZE (data);

  gtk_editable_set_text (GTK_EDITABLE (ua.entry), presets[index].ua);
  apply_ua (presets[index].ua);
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  /* gboolean, not void: a close-request handler's return value decides whether
   * the window closes, and a void handler leaves it to garbage. FALSE lets the
   * default handler finish the close; the flag keeps the restart flow from
   * bringing the helper back underneath a dying window. */
  ua.closing = TRUE;
  chromium_session_stop (ua.session);
  return FALSE;
}

static GtkWidget *
preset_button (gsize index)
{
  GtkWidget *button = gtk_button_new_with_label (presets[index].label);

  g_signal_connect (button, "clicked", G_CALLBACK (on_preset),
                    GSIZE_TO_POINTER (index));
  return button;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *window, *view, *root, *box, *entry, *apply, *status;
  const char *url, *env_ua;

  window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "CEFNest useragent");
  gtk_window_set_default_size (GTK_WINDOW (window), 1000, 700);

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (window), root);

  /* The controls: a free-form UA entry, an Apply that restarts the helper,
   * and one-button presets for the identities worth showing off. */
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_box_append (GTK_BOX (root), box);

  entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry),
                                  "自定义 User-Agent（留空 = 默认）");
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_box_append (GTK_BOX (box), entry);

  apply = gtk_button_new_with_label ("应用并重启");
  g_signal_connect (apply, "clicked", G_CALLBACK (on_apply), NULL);
  gtk_box_append (GTK_BOX (box), apply);

  for (gsize i = 0; i < G_N_ELEMENTS (presets); i++)
    gtk_box_append (GTK_BOX (box), preset_button (i));

  status = gtk_label_new ("Chromium 默认（未自定义）");
  gtk_label_set_ellipsize (GTK_LABEL (status), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (status), 0.0);
  gtk_widget_set_margin_start (status, 6);
  gtk_widget_set_margin_bottom (status, 4);
  gtk_widget_add_css_class (status, "dim-label");
  gtk_box_append (GTK_BOX (root), status);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (view, TRUE);
  gtk_box_append (GTK_BOX (root), view);

  ua.window = window;
  ua.view = view;
  ua.entry = entry;
  ua.status = status;

  ua.session = chromium_session_new (NULL);
  chromium_session_set_view (ua.session, CHROMIUM_VIEW (view));
  /* The session dies with the window: a pending ::closed after destroy finds
   * a finalized emitter instead of a stale restart path. */
  g_object_set_data_full (G_OBJECT (window), "session", ua.session,
                          g_object_unref);

  url = g_getenv ("CEFNEST_URL");
  g_strlcpy (ua.url, (url && *url) ? url : DEFAULT_PAGE,
             sizeof (ua.url));

  /* A UA asked for on the command line applies before the first start, so
   * even the very first page already reports it. */
  env_ua = g_getenv ("CEFNEST_UA");
  if (env_ua && *env_ua)
    {
      g_setenv ("CHROMIUM_USER_AGENT", env_ua, TRUE);
      gtk_editable_set_text (GTK_EDITABLE (entry), env_ua);
      gtk_label_set_text (GTK_LABEL (status), env_ua);
    }

  g_signal_connect (ua.session, "closed", G_CALLBACK (session_closed), NULL);
  g_signal_connect (ua.session, "ready", G_CALLBACK (session_ready), NULL);
  g_signal_connect (window, "close-request", G_CALLBACK (on_close), NULL);

  gtk_window_present (GTK_WINDOW (window));

  {
    GError *error = NULL;

    if (!chromium_session_start (ua.session, ua.url, &error))
      {
        g_warning ("cannot start the helper: %s", error->message);
        g_clear_error (&error);
      }
  }
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("io.github.cefnest.useragent",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
