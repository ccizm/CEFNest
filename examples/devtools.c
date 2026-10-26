#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"
#include "nested.h"

/* The inspector. The devtools-enabled switch turns on two entries: the context
 * menu's 检查 row and F12 / Ctrl+Shift+I. Either one asks CEF for a DevTools
 * browser, and the helper adopts it like any windowed browser — the host gets
 * ::browser-created with devtools=TRUE and opens a GTK window for it, exactly
 * as examples/windows.c does for page windows.
 *
 * Closing that window closes the inspector. Closing the page takes the
 * inspector with it, because CEF ties a DevTools browser to the browser it
 * inspects and closes both.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-devtools
 *   then right-click → 检查, or press F12.
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *reload;
  GtkWidget *entry;
  ChromiumView *view;
  NestedCompositor *compositor;
  NestedSurface *page_surface;  /* the first toplevel: the page's window */
  GHashTable *windows;          /* int id -> GtkWindow* (inspectors only) */
  GHashTable *views;            /* int id -> ChromiumView* */
  GHashTable *assigned;         /* NestedSurface* -> int id, 0 = the page */
  GList *pending;               /* inspector ids waiting for their surface */
  ChromiumSession *session;
  gboolean gone;
} DevTools;

static DevTools devtools;

static void
assign_surfaces (void)
{
  GList *tops, *l, *keys, *k;

  if (!devtools.compositor || !devtools.assigned)
    return;

  tops = nested_compositor_get_toplevels (devtools.compositor);

  /* A key has to be a surface the compositor still has: closing a window drops
   * its view, which was the last reference on the surface, and the helper's
   * next toplevel can come back at the same address. */
  keys = g_hash_table_get_keys (devtools.assigned);
  for (k = keys; k; k = k->next)
    {
      NestedSurface *dead = k->data;
      ChromiumView *view;

      if (g_list_find (tops, dead))
        continue;

      if (dead == devtools.page_surface)
        {
          view = devtools.view;
          g_clear_object (&devtools.page_surface);
        }
      else
        view = g_hash_table_lookup (devtools.views,
                                    g_hash_table_lookup (devtools.assigned,
                                                         dead));
      if (view)
        chromium_view_set_target_surface (view, NULL);
      g_hash_table_remove (devtools.assigned, dead);
    }
  g_list_free (keys);

  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;

      if (g_hash_table_contains (devtools.assigned, surface))
        continue;

      if (!devtools.page_surface)
        {
          devtools.page_surface = g_object_ref (surface);
          chromium_view_set_target_surface (devtools.view, surface);
          g_hash_table_insert (devtools.assigned, surface, GINT_TO_POINTER (0));
          continue;
        }

      /* A window can be destroyed before its surface arrives; its id then
       * waits in the queue with no view and must not swallow a surface. */
      while (devtools.pending)
        {
          int id = GPOINTER_TO_INT (devtools.pending->data);
          ChromiumView *view = g_hash_table_lookup (devtools.views,
                                                    GINT_TO_POINTER (id));

          devtools.pending = g_list_delete_link (devtools.pending,
                                                 devtools.pending);
          if (view)
            {
              chromium_view_set_target_surface (view, surface);
              g_hash_table_insert (devtools.assigned, surface,
                                   GINT_TO_POINTER (id));
              break;
            }
        }
    }
  g_list_free_full (tops, g_object_unref);
}

static void
on_toplevels_changed (NestedCompositor *compositor, gpointer data)
{
  GHashTableIter iter;
  gpointer key, value;

  assign_surfaces ();

  /* A retargeted view shows nothing new until it is told to draw: a static
   * page never damages itself. */
  if (devtools.view)
    gtk_widget_queue_draw (GTK_WIDGET (devtools.view));
  g_hash_table_iter_init (&iter, devtools.views);
  while (g_hash_table_iter_next (&iter, &key, &value))
    gtk_widget_queue_draw (value);
}

/* ::changed carries nothing on purpose: the getters are the one place that
 * knows what the page looks like. */
static void
refresh (void)
{
  const char *uri;
  const char *title;

  if (!devtools.window || !devtools.session)
    return;

  uri = chromium_session_get_uri (devtools.session);
  title = chromium_session_get_title (devtools.session);

  gtk_widget_set_sensitive (devtools.back,
                            chromium_session_get_can_go_back (devtools.session));
  gtk_widget_set_sensitive (devtools.forward,
                            chromium_session_get_can_go_forward (devtools.session));
  gtk_widget_set_sensitive (devtools.reload,
                            !devtools.gone &&
                            chromium_session_get_running (devtools.session));

  /* A page reporting its own URL must not overwrite a half-typed one. */
  if (uri && *uri && !gtk_widget_has_focus (devtools.entry))
    gtk_editable_set_text (GTK_EDITABLE (devtools.entry), uri);

  if (!devtools.gone && title && *title)
    gtk_window_set_title (GTK_WINDOW (devtools.window), title);
}

static void
session_changed (ChromiumSession *session, gpointer data)
{
  refresh ();
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  devtools.gone = TRUE;
  if (devtools.window)
    gtk_window_set_title (GTK_WINDOW (devtools.window),
                          reason ? reason : "gone");
  refresh ();
}

static void
devtools_window_gone (GtkWidget *widget, gpointer data)
{
  int id = GPOINTER_TO_INT (data);

  g_hash_table_remove (devtools.windows, GINT_TO_POINTER (id));
  g_hash_table_remove (devtools.views, GINT_TO_POINTER (id));
  devtools.pending = g_list_remove (devtools.pending, GINT_TO_POINTER (id));
}

static gboolean
devtools_window_close (GtkWindow *window, gpointer data)
{
  int id = GPOINTER_TO_INT (data);

  /* The user closed the inspector: tell the helper, then tear the GTK side
   * down. ::browser-closed will arrive too and find the row already gone. */
  if (devtools.session)
    chromium_session_close_browser (devtools.session, id);
  gtk_window_destroy (window);
  return TRUE;
}

static void
open_devtools_window (int id, const char *url)
{
  ChromiumView *view;
  GtkWidget *window;
  const char *title = chromium_session_get_title (devtools.session);
  g_autofree char *label = NULL;

  if (!devtools.compositor || !devtools.window)
    return;

  window = gtk_application_window_new (g_object_get_data (
      G_OBJECT (devtools.window), "gtk-app"));
  view = chromium_view_new_shared (devtools.compositor, NULL);
  label = g_strdup_printf ("DevTools — %s",
                           title && *title ? title
                                           : (url && *url ? url : "inspector"));
  gtk_window_set_title (GTK_WINDOW (window), label);
  gtk_window_set_default_size (GTK_WINDOW (window), 900, 650);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));

  g_hash_table_insert (devtools.windows, GINT_TO_POINTER (id), window);
  g_hash_table_insert (devtools.views, GINT_TO_POINTER (id), view);
  devtools.pending = g_list_append (devtools.pending, GINT_TO_POINTER (id));
  g_signal_connect (window, "destroy", G_CALLBACK (devtools_window_gone),
                    GINT_TO_POINTER (id));
  g_signal_connect (window, "close-request",
                    G_CALLBACK (devtools_window_close), GINT_TO_POINTER (id));

  g_printerr ("devtools: inspector %d in its own window\n", id);
  assign_surfaces ();
}

static void
session_browser_created (ChromiumSession *session, int id, const char *url,
                         gboolean windowed, gboolean devtools_browser,
                         gboolean background, gpointer data)
{
  /* Only the inspector gets a window here: the page's browser is painted by
   * the window this file built, whose view owns the compositor. */
  if (devtools_browser)
    open_devtools_window (id, url);
}

static void
session_browser_closed (ChromiumSession *session, int id, gpointer data)
{
  GtkWidget *window = g_hash_table_lookup (devtools.windows,
                                           GINT_TO_POINTER (id));

  if (window)
    {
      g_hash_table_remove (devtools.windows, GINT_TO_POINTER (id));
      g_hash_table_remove (devtools.views, GINT_TO_POINTER (id));
      devtools.pending = g_list_remove (devtools.pending, GINT_TO_POINTER (id));
      gtk_window_destroy (GTK_WINDOW (window));
      g_printerr ("devtools: inspector %d closed\n", id);
    }
}

/* ------------------------------------------------------------------ toolbar */

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (text && *text)
    chromium_session_load_uri (devtools.session, 0, text);
}

static void
on_back (GtkButton *button, gpointer data)
{
  chromium_session_go_back (devtools.session, 0);
}

static void
on_forward (GtkButton *button, gpointer data)
{
  chromium_session_go_forward (devtools.session, 0);
}

static void
on_reload (GtkButton *button, gpointer data)
{
  chromium_session_reload (devtools.session, 0, FALSE);
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

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (devtools.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hint;
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  devtools.window = gtk_application_window_new (app);
  g_object_set_data (G_OBJECT (devtools.window), "gtk-app", app);
  gtk_window_set_title (GTK_WINDOW (devtools.window), "CEFNest devtools");
  gtk_window_set_default_size (GTK_WINDOW (devtools.window), 1000, 700);

  devtools.back = toolbar_button (toolbar, "go-previous-symbolic",
                                  G_CALLBACK (on_back), "Back");
  devtools.forward = toolbar_button (toolbar, "go-next-symbolic",
                                     G_CALLBACK (on_forward), "Forward");
  devtools.reload = toolbar_button (toolbar, "view-refresh-symbolic",
                                    G_CALLBACK (on_reload), "Reload");

  devtools.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (devtools.entry), "address");
  gtk_widget_set_hexpand (devtools.entry, TRUE);
  g_signal_connect (devtools.entry, "activate",
                    G_CALLBACK (on_entry_activate), NULL);
  gtk_box_append (GTK_BOX (toolbar), devtools.entry);

  hint = gtk_label_new ("右键 → 检查，或按 F12 / Ctrl+Shift+I");
  gtk_widget_set_tooltip_text (hint, "The DevTools switch is on for this window");
  gtk_box_append (GTK_BOX (toolbar), hint);

  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_widget_set_margin_bottom (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (devtools.window), root);

  /* The view has to be in the window before the session starts: the helper
   * takes its WAYLAND_DISPLAY from the compositor this view owns. */
  devtools.session = chromium_session_new (NULL);
  chromium_session_set_view (devtools.session, view);
  devtools.view = view;
  devtools.compositor = chromium_view_get_compositor (view);

  /* Before start(): the ready handshake carries the switch to the helper. */
  chromium_session_set_devtools_enabled (devtools.session, TRUE);

  devtools.windows = g_hash_table_new (g_direct_hash, g_direct_equal);
  devtools.views = g_hash_table_new (g_direct_hash, g_direct_equal);
  devtools.assigned = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_signal_connect (devtools.compositor, "toplevels-changed",
                    G_CALLBACK (on_toplevels_changed), NULL);
  g_signal_connect (devtools.session, "changed", G_CALLBACK (session_changed),
                    NULL);
  g_signal_connect (devtools.session, "closed", G_CALLBACK (session_closed),
                    NULL);
  g_signal_connect (devtools.session, "browser-created",
                    G_CALLBACK (session_browser_created), NULL);
  g_signal_connect (devtools.session, "browser-closed",
                    G_CALLBACK (session_browser_closed), NULL);
  g_signal_connect (devtools.window, "close-request", G_CALLBACK (on_close),
                    NULL);

  gtk_window_present (GTK_WINDOW (devtools.window));

  if (!chromium_session_start (devtools.session,
                               url && *url ? url
                                           : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
      devtools.gone = TRUE;
    }

  refresh ();
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (devtools.session)
    chromium_session_stop (devtools.session);
  g_clear_pointer (&devtools.windows, g_hash_table_unref);
  g_clear_pointer (&devtools.views, g_hash_table_unref);
  g_clear_pointer (&devtools.assigned, g_hash_table_unref);
  g_clear_list (&devtools.pending, NULL);
  g_clear_object (&devtools.page_surface);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.devtools",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
