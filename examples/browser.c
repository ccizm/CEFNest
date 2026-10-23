#include <math.h>

#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* examples/minimal.c paints a page and stops. This is the single-page browser:
 * one window, one view, one session, plus the toolbar chrome a real embedding
 * app writes around it — navigation, an address entry, zoom and the mute
 * switch. Everything on screen follows the session's ::changed signal and the
 * getters; no per-event bookkeeping.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-browser
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *reload;
  GtkWidget *entry;
  GtkWidget *zoom;
  GtkWidget *mute;
  ChromiumSession *session;
  double zoom_level;   /* last level ::zoom-changed reported, 0.0 = 100 % */
  gboolean gone;
} Browser;

static Browser browser;

/* ::changed carries nothing on purpose: the getters are the one place that
 * knows what the page looks like, so every event just lands here. */
static void
refresh (void)
{
  const char *uri = chromium_session_get_uri (browser.session);
  const char *title = chromium_session_get_title (browser.session);
  const gboolean running = !browser.gone &&
    chromium_session_get_running (browser.session);

  if (!browser.window)
    return;

  gtk_widget_set_sensitive (browser.back,
                            chromium_session_get_can_go_back (browser.session));
  gtk_widget_set_sensitive (browser.forward,
                            chromium_session_get_can_go_forward (browser.session));
  gtk_widget_set_sensitive (browser.reload, running);

  /* The entry is where a person types the next address; a page reporting its
   * own URL must not overwrite a half-typed one. */
  if (uri && *uri && !gtk_widget_has_focus (browser.entry))
    gtk_editable_set_text (GTK_EDITABLE (browser.entry), uri);

  /* The level is a Blink zoom level, not a percentage: the factor is
   * 1.2^level. Measured on this tree — level 0.5 reads window.devicePixelRatio
   * 1.09544 and level 1.0 reads 1.20000, so a linear *100 would print 50 % and
   * 100 % for what the page actually renders at 110 % and 120 %. */
  {
    g_autofree char *text =
      g_strdup_printf ("%d %%", (int) (100.0 * pow (1.2, browser.zoom_level) + 0.5));
    gtk_button_set_label (GTK_BUTTON (browser.zoom), text);
  }

  /* The mute row shows what the helper read back, not what was clicked: the
   * icon, the tooltip and the pressed state all follow the getter, and the
   * setter's echo is what on_mute_toggled() screens out. */
  {
    const gboolean muted = chromium_session_get_muted (browser.session);

    gtk_button_set_icon_name (
      GTK_BUTTON (browser.mute),
      muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
    gtk_widget_set_tooltip_text (browser.mute,
                                 muted ? "Unmute the page" : "Mute the page");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (browser.mute), muted);
  }

  if (running && chromium_session_get_loading (browser.session))
    gtk_window_set_title (GTK_WINDOW (browser.window),
                          "loading — CEFNest browser");
  else if (title && *title)
    gtk_window_set_title (GTK_WINDOW (browser.window), title);
}

static void
session_changed (ChromiumSession *session, gpointer data)
{
  refresh ();
}

static void
session_zoom_changed (ChromiumSession *session, double level, gpointer data)
{
  browser.zoom_level = level;
  refresh ();
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  /* It never comes back on this session; the toolbar goes insensitive and the
   * title says why. */
  browser.gone = TRUE;
  if (browser.window)
    gtk_window_set_title (GTK_WINDOW (browser.window), reason ? reason : "gone");
  refresh ();
}

/* ------------------------------------------------------------------ toolbar */

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (text && *text)
    chromium_session_load_uri (browser.session, text);
}

static void
on_back (GtkButton *button, gpointer data)
{
  chromium_session_go_back (browser.session);
}

static void
on_forward (GtkButton *button, gpointer data)
{
  chromium_session_go_forward (browser.session);
}

static void
on_reload (GtkButton *button, gpointer data)
{
  chromium_session_reload (browser.session, FALSE);
}

static void
on_stop (GtkButton *button, gpointer data)
{
  chromium_session_stop_loading (browser.session);
}

static void
on_zoom_out (GtkButton *button, gpointer data)
{
  chromium_session_zoom (browser.session, "out");
}

static void
on_zoom_reset (GtkButton *button, gpointer data)
{
  /* Chromium persists zoom per origin, so "reset" writes this origin's stored
   * level back to 100 % — the next visit starts there. */
  chromium_session_zoom (browser.session, "reset");
}

static void
on_zoom_in (GtkButton *button, gpointer data)
{
  chromium_session_zoom (browser.session, "in");
}

static void
on_mute_toggled (GtkToggleButton *button, gpointer data)
{
  /* The page's sound leaves the helper through its own audio process and lands
   * on the host's speakers, so this is the only switch the app has. What the
   * helper read back moves ::changed a round trip later — and refresh() then
   * sets this very button, whose "toggled" lands here again. Same value twice
   * in a row is that echo, not a request. */
  const gboolean want = gtk_toggle_button_get_active (button);

  if (want == chromium_session_get_muted (browser.session))
    return;
  chromium_session_set_muted (browser.session, want);
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (browser.session);
  return FALSE;
}

static GtkWidget *
toolbar_button (GtkWidget *box, const char *icon, GCallback callback,
                const char *tip)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);

  gtk_widget_set_tooltip_text (button, tip);
  g_signal_connect (button, "clicked", callback, NULL);
  gtk_box_append (GTK_BOX (box), button);
  return button;
}

/* -------------------------------------------------------------------- build */

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  browser.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (browser.window), "CEFNest browser");
  gtk_window_set_default_size (GTK_WINDOW (browser.window), 1000, 700);

  browser.back = toolbar_button (toolbar, "go-previous-symbolic",
                                 G_CALLBACK (on_back), "Back");
  browser.forward = toolbar_button (toolbar, "go-next-symbolic",
                                    G_CALLBACK (on_forward), "Forward");
  browser.reload = toolbar_button (toolbar, "view-refresh-symbolic",
                                   G_CALLBACK (on_reload), "Reload");
  toolbar_button (toolbar, "process-stop-symbolic", G_CALLBACK (on_stop),
                  "Stop loading");

  browser.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (browser.entry), "address");
  gtk_widget_set_hexpand (browser.entry, TRUE);
  g_signal_connect (browser.entry, "activate", G_CALLBACK (on_entry_activate),
                    NULL);
  gtk_box_append (GTK_BOX (toolbar), browser.entry);

  toolbar_button (toolbar, "zoom-out-symbolic", G_CALLBACK (on_zoom_out),
                  "Zoom out");
  browser.zoom = gtk_button_new_with_label ("100 %");
  g_signal_connect (browser.zoom, "clicked", G_CALLBACK (on_zoom_reset), NULL);
  gtk_widget_set_tooltip_text (browser.zoom, "Reset to 100 %");
  gtk_box_append (GTK_BOX (toolbar), browser.zoom);
  toolbar_button (toolbar, "zoom-in-symbolic", G_CALLBACK (on_zoom_in),
                  "Zoom in");

  /* A fresh helper always starts unmuted, so that is what the row opens with;
   * refresh() owns it from the first ::changed on. */
  browser.mute = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (browser.mute),
                            "audio-volume-high-symbolic");
  gtk_widget_set_tooltip_text (browser.mute, "Mute the page");
  g_signal_connect (browser.mute, "toggled", G_CALLBACK (on_mute_toggled),
                    NULL);
  gtk_box_append (GTK_BOX (toolbar), browser.mute);

  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_widget_set_margin_bottom (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (browser.window), root);

  /* The view has to be in the window before the session starts: the helper
   * takes its WAYLAND_DISPLAY from the compositor this view owns. */
  browser.session = chromium_session_new (NULL);
  chromium_session_set_view (browser.session, view);

  g_signal_connect (browser.session, "changed", G_CALLBACK (session_changed),
                    NULL);
  g_signal_connect (browser.session, "zoom-changed",
                    G_CALLBACK (session_zoom_changed), NULL);
  g_signal_connect (browser.session, "closed", G_CALLBACK (session_closed),
                    NULL);
  g_signal_connect (browser.window, "close-request", G_CALLBACK (on_close),
                    NULL);

  gtk_window_present (GTK_WINDOW (browser.window));

  if (!chromium_session_start (browser.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
      browser.gone = TRUE;
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (browser.window), "session",
                          g_object_ref (browser.session), g_object_unref);
  refresh ();
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.browser",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
