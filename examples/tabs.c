#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Multi-tab. One helper hosts several browsers; the shared tab window is
 * painted by this one view, and select_browser() is what switches pages.
 * CEF assigns the ids, so the tab row is keyed from ::browser-created — and
 * ::browser-selected can name a browser before its ::browser-created arrives,
 * so a tab is created on selection too.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-tabs
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *entry;
  GtkWidget *tabs;           /* the row of tab buttons */
  ChromiumSession *session;
  GHashTable *tab_buttons;   /* int id -> tab row widget */
  int active;
  gboolean gone;
} Tabs;

static Tabs tabs;

static void
mark_active (int id)
{
  GHashTableIter iter;
  gpointer key, value;

  /* remembered rather than only applied: on a first run the selection names a
   * tab whose button does not exist yet. */
  tabs.active = id;

  g_hash_table_iter_init (&iter, tabs.tab_buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_INT (key) == id)
        gtk_widget_add_css_class (value, "suggested-action");
      else
        gtk_widget_remove_css_class (value, "suggested-action");
    }
}

static void
select_tab (GtkButton *button, gpointer data)
{
  chromium_session_select_browser (tabs.session,
                                   GPOINTER_TO_INT (data));
}

static void
close_tab (GtkButton *button, gpointer data)
{
  int id = GPOINTER_TO_INT (data);
  GtkWidget *row = g_hash_table_lookup (tabs.tab_buttons,
                                        GINT_TO_POINTER (id));

  /* Measured: for a browser in the shared tab window nothing comes back — no
   * ::browser-closed, because CEF has no OnBeforeClose on this path. The row
   * goes away here, in the click handler, or it would stay forever. */
  chromium_session_close_browser (tabs.session, id);
  if (row)
    {
      g_hash_table_remove (tabs.tab_buttons, GINT_TO_POINTER (id));
      gtk_box_remove (GTK_BOX (tabs.tabs), row);
    }
}

static GtkWidget *
tab_row (int id, const char *url)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *button = gtk_button_new_with_label (url && *url ? url : "blank");
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");

  gtk_widget_set_tooltip_text (close, "Close tab (no event comes back for a tab)");
  g_signal_connect (button, "clicked", G_CALLBACK (select_tab),
                    GINT_TO_POINTER (id));
  g_signal_connect (close, "clicked", G_CALLBACK (close_tab),
                    GINT_TO_POINTER (id));
  gtk_box_append (GTK_BOX (row), button);
  gtk_box_append (GTK_BOX (row), close);
  return row;
}

static void
session_browser_created (ChromiumSession *session, int id, const char *url,
                         gboolean windowed, gboolean devtools, gpointer data)
{
  GtkWidget *row;

  if (windowed)
    return;   /* this demo keeps everything in the shared tab window */

  if (g_hash_table_contains (tabs.tab_buttons, GINT_TO_POINTER (id)))
    return;   /* the selection created it early */

  row = tab_row (id, url);
  g_hash_table_insert (tabs.tab_buttons, GINT_TO_POINTER (id), row);
  gtk_box_append (GTK_BOX (tabs.tabs), row);
  if (id == tabs.active)
    mark_active (id);
}

static void
session_browser_selected (ChromiumSession *session, int id, gpointer data)
{
  /* Possibly for a browser whose created event is still on the wire: the
   * highlight waits in tabs.active until the button shows up. */
  mark_active (id);
}

static void
session_browser_closed (ChromiumSession *session, int id, gpointer data)
{
  /* Only a windowed browser reports this. Nothing in this demo is windowed —
   * the row was already removed by close_tab(). */
}

static void
on_new_tab (GtkButton *button, gpointer data)
{
  const char *typed = gtk_editable_get_text (GTK_EDITABLE (tabs.entry));

  /* FALSE = tab mode: another browser in the same helper, painted by the same
   * view. TRUE would be a standalone CEF window — examples/windows.c. */
  chromium_session_new_browser (tabs.session,
                                typed && *typed ? typed : "about:blank",
                                FALSE);
}

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (text && *text)
    chromium_session_load_uri (tabs.session, text);
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (tabs.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  tabs.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (tabs.window), "CEFNest tabs");
  gtk_window_set_default_size (GTK_WINDOW (tabs.window), 1000, 700);

  {
    GtkWidget *new_tab = gtk_button_new_with_label ("+");
    gtk_widget_set_tooltip_text (new_tab, "New tab");
    g_signal_connect (new_tab, "clicked", G_CALLBACK (on_new_tab), NULL);
    gtk_box_append (GTK_BOX (toolbar), new_tab);
  }

  tabs.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (tabs.entry), "address");
  gtk_widget_set_hexpand (tabs.entry, TRUE);
  g_signal_connect (tabs.entry, "activate", G_CALLBACK (on_entry_activate),
                    NULL);
  gtk_box_append (GTK_BOX (toolbar), tabs.entry);
  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  tabs.tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (tabs.tabs, 6);
  gtk_box_append (GTK_BOX (root), tabs.tabs);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (tabs.window), root);

  tabs.session = chromium_session_new (NULL);
  chromium_session_set_view (tabs.session, view);

  g_signal_connect (tabs.session, "browser-created",
                    G_CALLBACK (session_browser_created), NULL);
  g_signal_connect (tabs.session, "browser-selected",
                    G_CALLBACK (session_browser_selected), NULL);
  g_signal_connect (tabs.session, "browser-closed",
                    G_CALLBACK (session_browser_closed), NULL);
  g_signal_connect (tabs.window, "close-request", G_CALLBACK (on_close), NULL);

  gtk_window_present (GTK_WINDOW (tabs.window));

  tabs.tab_buttons = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (!chromium_session_start (tabs.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
      tabs.gone = TRUE;
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (tabs.window), "session",
                          g_object_ref (tabs.session), g_object_unref);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (tabs.session)
    chromium_session_stop (tabs.session);
  g_clear_pointer (&tabs.tab_buttons, g_hash_table_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.tabs",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
