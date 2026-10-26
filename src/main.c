#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include "chromium-session.h"
#include "chromium-view.h"

typedef struct {
  GtkApplication *app;
  GtkWindow *window;
  ChromiumView *view;
  GSubprocess *child;
  char **client_argv;
  /* --data-dir=<absolute path>: the demo's one persistent profile, handed to
   * the session before start(). NULL means the default: no disk at all. */
  char *data_directory;
  /* --devtools: turn the inspector switch on before start(), the same call a
   * host makes from its own preferences UI. */
  gboolean devtools;
  /* --plugin-dir=<absolute path>: scan that directory, enable everything that
   * parses, and grant what enable() asks for. The demo's policy, not the
   * component's: a real host shows the person a prompt. */
  char *plugin_directory;
  /* What the process hands back once the run is over. Only a scripted run that
   * was cut short gives it anything but 0; see script_helper_gone(). */
  int exit_status;

  /* Set when the page comes from a Chromium helper process rather than from a
   * plain Wayland client: then the header bar is a browser toolbar and the
   * session owns the process. */
  ChromiumSession *session;
  /* A scripted run that loses its helper reports the failure at ::closed but
   * does not quit there: the helper may still be inside CefShutdown(), drawing
   * into the nested compositor this process owns. ::helper-exited is the first
   * moment the display can go without killing that shutdown. */
  gboolean helper_exited;
  gboolean quit_when_helper_exits;
  /* The `restart` step asked for the helper to stop; the ::closed that follows
   * is the step working, not the run failing. Cleared when start() lands. */
  gboolean restart_pending;
  GtkWidget *header;
  GHashTable *plugin_buttons;  /* plugin id -> GtkButton* */
  GtkWidget *address;
  GtkWidget *back;
  GtkWidget *forward;
  GtkWidget *downloads;
  GtkWidget *permissions;
  GtkWidget *dialogs;
  GtkWidget *find_bar;
  GtkWidget *find_entry;
  GtkWidget *find_case;
  GtkWidget *find_label;

  /* Multi-browser chrome: one tab button per CEF browser created by this
   * helper. Standalone (windowed) browsers are listed too until a separate
   * host window layer exists. */
  GtkWidget *tabs;
  GHashTable *tab_buttons;      /* int id -> GtkButton* */
  GHashTable *browser_windows;  /* int id -> GtkWindow* (windowed only) */
  GHashTable *browser_views;    /* int id -> ChromiumView* (windowed only) */
  GHashTable *assigned_surfaces;/* NestedSurface* -> browser id */
  GList *pending_window_ids;    /* windowed browsers waiting for a surface */
  NestedSurface *tab_surface;   /* the tab CefWindow's toplevel */
  int active_browser;
  /* The most recent ::render-unresponsive id, for the hang-respond step. */
  int pending_hang_id;

  /* 搭法二, the popover form: a plugin popup hosted under its action button
   * instead of in a standalone window. One at a time; the browser behind a
   * closed popover goes away with it. */
  gboolean popup_in_popover;    /* the "popup-mode popover" script step set */
  GtkWidget *popup_popover;
  char *popup_pending_plugin;   /* plugin id whose popup is on its way */
  char *popup_pending_uri;
  int popup_browser;
  int popup_width, popup_height;
  NestedSurface *popup_surface;
} Demo;

static Demo demo;

static uint64_t demo_frame_count (void);

#ifndef CLIENT_PATH
#define CLIENT_PATH "shm-client"
#endif

#ifndef CLIENT_DEVELOPMENT
#define CLIENT_DEVELOPMENT ""
#endif

static void session_ready (ChromiumSession *session, gpointer data);
static void open_browser_window (int id, gboolean devtools, const char *title);
static void open_popup_popover (int id);
static void assign_browser_surfaces (void);
static void session_browser_created (ChromiumSession *session, int id,
                                     const char *url, gboolean windowed,
                                     gboolean devtools, gboolean background,
                                     gpointer data);
static void session_browser_selected (ChromiumSession *session, int id,
                                      gpointer data);
static void session_browser_closed (ChromiumSession *session, int id,
                                    gpointer data);
static void script_note_helper_gone (const char *reason);
static void demo_setup_plugins (void);
static gboolean demo_restart_start (gpointer data);
static gboolean demo_clear_data_try (gpointer data);
static void session_plugin_action (ChromiumPluginManager *manager,
                                   const char *id, gpointer data);
static void plugin_action_clicked (GtkButton *button, gpointer data);

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
tab_clicked (GtkButton *button, gpointer data)
{
  int id = GPOINTER_TO_INT (data);
  chromium_session_select_browser (demo.session, id);
}

static void
session_browser_created (ChromiumSession *session, int id, const char *url,
                         gboolean windowed, gboolean devtools,
                         gboolean background, gpointer data)
{
  GtkWidget *button;
  const char *shown = url && *url ? url : (windowed ? "window" : "blank");

  if (!demo.tabs)
    return;
  if (background)
    {
      /* A background page has no surface and no tab: it runs, and the log is
       * the only place it shows. */
      g_printerr ("demo: browser %d created [background] %s\n", id, shown);
      return;
    }
  if (devtools)
    {
      /* The inspector lives in its own window like any standalone browser, but
       * it gets no tab: its focus must not steal the page's address bar. */
      const char *title = chromium_session_get_title (session);

      g_printerr ("demo: browser %d created [window] devtools=1 %s\n", id,
                  url && *url ? url : "(no url)");
      open_browser_window (id, TRUE, title && *title ? title : NULL);
      return;
    }
  if (demo.popup_in_popover && demo.popup_pending_uri &&
      g_strcmp0 (url, demo.popup_pending_uri) == 0)
    {
      /* The 搭法二 answer to ::plugin-action: this browser is the popup a
       * popover asked for. It takes no tab button — a transient popup is not
       * a page in the tab strip. */
      g_printerr ("demo: browser %d created [popup] %s\n", id, shown);
      g_clear_pointer (&demo.popup_pending_uri, g_free);
      open_popup_popover (id);
      return;
    }
  button = gtk_button_new_with_label (shown);
  gtk_widget_set_tooltip_text (button,
                               windowed ? "standalone browser" : "tab");
  g_object_set_data (G_OBJECT (button), "browser-id", GINT_TO_POINTER (id));
  g_signal_connect (button, "clicked", G_CALLBACK (tab_clicked),
                    GINT_TO_POINTER (id));
  gtk_box_append (GTK_BOX (demo.tabs), button);
  if (!demo.tab_buttons)
    demo.tab_buttons = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_hash_table_insert (demo.tab_buttons, GINT_TO_POINTER (id), button);
  if (id == demo.active_browser)
    gtk_widget_add_css_class (button, "suggested-action");
  g_printerr ("demo: browser %d created %s%s\n", id, windowed ? "[window] " : "",
              shown);
  if (windowed)
    open_browser_window (id, FALSE, NULL);
}

static void
session_browser_selected (ChromiumSession *session, int id, gpointer data)
{
  GtkWidget *button;

  demo.active_browser = id;
  if (!demo.tab_buttons)
    return;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, demo.tab_buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_INT (key) == id)
        gtk_widget_add_css_class (GTK_WIDGET (value), "suggested-action");
      else
        gtk_widget_remove_css_class (GTK_WIDGET (value), "suggested-action");
    }
  button = g_hash_table_lookup (demo.tab_buttons, GINT_TO_POINTER (id));
  if (button)
    gtk_widget_grab_focus (button);
  g_printerr ("demo: browser %d selected\n", id);
}

static void
session_browser_closed (ChromiumSession *session, int id, gpointer data)
{
  GtkWidget *button;

  if (id == demo.popup_browser)
    {
      /* The popover's browser went away (the popover's own close asked for
         this, or the page closed itself): drop what the FIFO and the widget
         tree still hold. */
      if (demo.browser_views)
        g_hash_table_remove (demo.browser_views, GINT_TO_POINTER (id));
      demo.pending_window_ids = g_list_remove (demo.pending_window_ids,
                                               GINT_TO_POINTER (id));
      if (demo.popup_popover)
        gtk_popover_popdown (GTK_POPOVER (demo.popup_popover));
      g_clear_pointer (&demo.popup_popover, gtk_widget_unparent);
      demo.popup_browser = 0;
      demo.popup_surface = NULL;
    }
  if (demo.tab_buttons)
    {
      button = g_hash_table_lookup (demo.tab_buttons, GINT_TO_POINTER (id));
      if (button)
        {
          g_hash_table_remove (demo.tab_buttons, GINT_TO_POINTER (id));
          gtk_box_remove (GTK_BOX (demo.tabs), button);
        }
    }
  if (demo.browser_windows)
    {
      GtkWidget *window = g_hash_table_lookup (demo.browser_windows,
                                               GINT_TO_POINTER (id));
      if (window)
        gtk_window_destroy (GTK_WINDOW (window));
    }
  g_printerr ("demo: browser %d closed\n", id);
}

/* The pairing the session reports, as a script can see it. The surface-pair
 * gate grades these lines against the pixels script_surface_probe() reads:
 * the signal says which surface is that browser's, and the sample says the
 * picture on it is that browser's page. */
static void
session_browser_surface (ChromiumSession *session, int id,
                         NestedSurface *surface, gpointer data)
{
  int w = 0, h = 0;

  if (!surface)
    {
      if (demo.popup_surface && id == demo.popup_browser)
        demo.popup_surface = NULL;
      g_printerr ("demo: browser-surface id=%d none\n", id);
      return;
    }
  if (id == demo.popup_browser)
    {
      /* The popover's box is the manifest's clamped popup size, not the
         window default; tell the toplevel before its first frame lands. */
      demo.popup_surface = surface;
      nested_compositor_set_surface_size (chromium_view_get_compositor (demo.view),
                                          surface,
                                          demo.popup_width, demo.popup_height);
    }
  nested_surface_get_size (surface, &w, &h);
  g_printerr ("demo: browser-surface id=%d size=%dx%d\n", id, w, h);
}

static void
new_tab_clicked (GtkButton *button, gpointer data)
{
  const char *url = demo.session ? chromium_session_get_uri (demo.session) : NULL;

  if (demo.session)
    chromium_session_new_browser (demo.session, url && *url ? url : "about:blank",
                                  FALSE);
}

static void
new_window_clicked (GtkButton *button, gpointer data)
{
  const char *url = demo.session ? chromium_session_get_uri (demo.session) : NULL;

  if (demo.session)
    chromium_session_new_browser (demo.session, url && *url ? url : "about:blank",
                                  TRUE);
}

static void
assign_browser_surfaces (void)
{
  NestedCompositor *compositor;
  GList *tops, *l, *keys, *k;

  if (!demo.view)
    return;
  compositor = chromium_view_get_compositor (demo.view);
  if (!compositor)
    return;
  if (!demo.assigned_surfaces)
    demo.assigned_surfaces = g_hash_table_new (g_direct_hash, g_direct_equal);

  gboolean requeued = FALSE;
  tops = nested_compositor_get_toplevels (compositor);

  /* A key has to be a surface the compositor still has. Closing a window drops
   * the view, and the view was the last reference on the surface it showed, so
   * the helper's next toplevel can come back with that address — and a key left
   * pointing at it reads as "already assigned", which is a window that never
   * gets a surface to show. */
  keys = g_hash_table_get_keys (demo.assigned_surfaces);
  for (k = keys; k; k = k->next)
    {
      NestedSurface *dead = k->data;
      ChromiumView *view;

      if (g_list_find (tops, dead))
        continue;

      int dead_id = 0;
      if (dead == demo.tab_surface)
        view = CHROMIUM_VIEW (demo.view);
      else
        {
          dead_id = GPOINTER_TO_INT (g_hash_table_lookup (demo.assigned_surfaces,
                                                          dead));
          view = g_hash_table_lookup (demo.browser_views,
                                      GINT_TO_POINTER (dead_id));
        }
      if (view)
        chromium_view_set_target_surface (view, NULL);
      if (dead == demo.tab_surface)
        g_clear_object (&demo.tab_surface);
      else if (view)
        /* The helper can retire a window and map its replacement within one
         * tick — adopting the DevTools browser out of CEF's framed window
         * does exactly that. Re-queue the id, so the replacement surface
         * lands in the window that just lost its surface instead of leaving
         * it blank until the next toplevel happens to come. */
        {
          demo.pending_window_ids = g_list_append (demo.pending_window_ids,
                                                   GINT_TO_POINTER (dead_id));
          requeued = TRUE;
        }
      g_hash_table_remove (demo.assigned_surfaces, dead);
    }
  g_list_free (keys);

  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      ChromiumView *view;
      int id;

      if (g_hash_table_contains (demo.assigned_surfaces, surface))
        continue;

      if (!demo.tab_surface)
        {
          demo.tab_surface = g_object_ref (surface);
          chromium_view_set_target_surface (CHROMIUM_VIEW (demo.view), surface);
          g_hash_table_insert (demo.assigned_surfaces, surface,
                               GINT_TO_POINTER (demo.active_browser));
          continue;
        }

      if (!demo.pending_window_ids)
        continue;

      /* A window can be destroyed before its surface arrives. Its id is then
       * still in the queue but no view owns it; skip those ids instead of
       * binding the surface to one — doing that inserts a key nothing holds a
       * reference to, and the next window that reuses the address is told the
       * surface is already assigned. */
      view = NULL;
      while (demo.pending_window_ids)
        {
          id = GPOINTER_TO_INT (demo.pending_window_ids->data);
          demo.pending_window_ids = g_list_delete_link (demo.pending_window_ids,
                                                        demo.pending_window_ids);
          view = g_hash_table_lookup (demo.browser_views, GINT_TO_POINTER (id));
          if (view)
            break;
        }
      if (!view)
        continue;

      chromium_view_set_target_surface (view, surface);
      g_hash_table_insert (demo.assigned_surfaces, surface, GINT_TO_POINTER (id));
      g_printerr ("demo: browser %d got a surface\n", id);
    }
  g_list_free_full (tops, g_object_unref);
  /* A re-queued id can only be satisfied by surfaces already in the list —
   * none of which will trigger on_toplevels_changed again. */
  if (requeued)
    assign_browser_surfaces ();
}

static void
on_toplevels_changed (NestedCompositor *compositor, gpointer data)
{
  /* The compositor announces its last surfaces going away while it dies, and
   * that can land after teardown has already cleared demo.view. */
  if (!demo.view)
    return;

  assign_browser_surfaces ();
  gtk_widget_queue_draw (GTK_WIDGET (demo.view));
}

static void
browser_window_destroyed (GtkWidget *widget, gpointer data)
{
  int id = GPOINTER_TO_INT (data);

  if (!demo.browser_windows)
    return;
  g_hash_table_remove (demo.browser_windows, GINT_TO_POINTER (id));
  g_hash_table_remove (demo.browser_views, GINT_TO_POINTER (id));
  /* A window that never got its surface leaves its id waiting in the queue;
   * the next toplevel must not consume it. */
  demo.pending_window_ids = g_list_remove (demo.pending_window_ids,
                                           GINT_TO_POINTER (id));
}

static void
browser_window_active (GObject *object, GParamSpec *pspec, gpointer data)
{
  int id = GPOINTER_TO_INT (data);

  if (gtk_window_is_active (GTK_WINDOW (object)) && demo.session)
    chromium_session_select_browser (demo.session, id);
}

static gboolean
browser_window_close_request (GtkWindow *window, gpointer data)
{
  int id = GPOINTER_TO_INT (data);

  if (demo.session)
    chromium_session_close_browser (demo.session, id);
  /* The helper's browser-closed event destroys the window; if it never comes,
   * do not leave a dead window behind. Returning TRUE says this handler took
   * the close: close-request is a boolean signal, and a void handler leaves
   * GTK reading whatever the ABI left in the return register. */
  gtk_window_destroy (window);
  return TRUE;
}

static void
open_browser_window (int id, gboolean devtools, const char *title)
{
  NestedCompositor *compositor;
  ChromiumView *view;
  GtkWidget *window;

  if (!demo.view || !demo.browser_windows)
    return;
  compositor = chromium_view_get_compositor (demo.view);
  if (!compositor)
    return;

  window = gtk_application_window_new (demo.app);
  view = chromium_view_new_shared (compositor, NULL);
  gtk_window_set_default_size (GTK_WINDOW (window), 900, 650);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  if (devtools)
    {
      g_autofree char *label =
        g_strdup_printf ("DevTools — %s", title && *title ? title : "inspector");

      gtk_window_set_title (GTK_WINDOW (window), label);
    }
  else
    gtk_window_set_title (GTK_WINDOW (window), "CEFNest window");
  gtk_window_present (GTK_WINDOW (window));

  g_hash_table_insert (demo.browser_windows, GINT_TO_POINTER (id), window);
  g_hash_table_insert (demo.browser_views, GINT_TO_POINTER (id), view);
  demo.pending_window_ids = g_list_append (demo.pending_window_ids,
                                           GINT_TO_POINTER (id));
  g_signal_connect (window, "destroy", G_CALLBACK (browser_window_destroyed),
                    GINT_TO_POINTER (id));
  g_signal_connect (window, "close-request",
                    G_CALLBACK (browser_window_close_request),
                    GINT_TO_POINTER (id));
  g_signal_connect (window, "notify::is-active",
                    G_CALLBACK (browser_window_active), GINT_TO_POINTER (id));
  g_printerr ("demo: browser %d standalone window%s\n", id,
              devtools ? " (devtools)" : "");
  assign_browser_surfaces ();
}

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
session_load_started (ChromiumSession *session, int browser, const char *uri,
                      int transition, gboolean is_main_frame, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: load-started browser=%d %s transition=%d main=%d\n",
              browser, uri, transition, is_main_frame ? 1 : 0);
}

static void
session_load_end (ChromiumSession *session, int browser, const char *uri,
                  int http_code, gpointer data)
{
  (void) session; (void) data;
  /* browser at the end on purpose: the line's prefix is what the older
   * gates match, and appending keeps every one of them working. */
  g_printerr ("demo: loaded %s (HTTP %d) browser=%d\n", uri, http_code,
              browser);
}

static void
session_load_error (ChromiumSession *session, int browser, int error_code,
                    const char *uri, const char *message, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: load failed %d %s %s browser=%d\n", error_code, uri,
              message ? message : "", browser);
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
session_popup_requested (ChromiumSession *session, const char *url,
                         int disposition, gboolean gesture, int browser,
                         gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: popup-requested browser=%d %s disposition=%d gesture=%d\n",
              browser, url, disposition, gesture ? 1 : 0);
}

/* ---- the epiphany-gap batch: what a host's own UI reads ---- */

/* The auth question the demo took (CEFNEST_AUTH=ask) and the scheme request
   it is serving (CEFNEST_SCHEME_SERVE=1): ids the script steps answer. */
static int demo_auth_id = 0;
static int demo_scheme_id = 0;

static void
session_browsers_changed (ChromiumSession *session, gpointer data)
{
  ChromiumBrowserInfo *rows = NULL;
  gsize n = 0, i;

  (void) data;
  rows = chromium_session_browsers_list (session, &n);
  g_printerr ("demo: browsers-changed n=%zu\n", n);
  for (i = 0; rows && i < n; i++)
    g_printerr ("demo: browser-row id=%d url=%s title=%s active=%d "
                "windowed=%d muted=%d zoom=%.2f\n",
                rows[i].id, rows[i].url, rows[i].title,
                rows[i].active ? 1 : 0, rows[i].windowed ? 1 : 0,
                rows[i].muted ? 1 : 0, rows[i].zoom);
  /* The array is one allocation of struct copies: free the strings, then the
     array — browser_info_free() is for a heap row of its own. */
  for (i = 0; i < n; i++)
    {
      g_free (rows[i].url);
      g_free (rows[i].title);
    }
  g_free (rows);
}

static gboolean
session_auth_required (ChromiumSession *session, int id, int browser,
                       gboolean is_proxy, const char *scheme,
                       const char *realm, const char *origin, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: auth-required id=%d browser=%d proxy=%d scheme=%s "
              "realm=%s origin=%s\n",
              id, browser, is_proxy ? 1 : 0, scheme ? scheme : "",
              realm ? realm : "", origin ? origin : "");
  if (!g_getenv ("CEFNEST_AUTH"))
    return FALSE;   /* the session's default: cancel */
  demo_auth_id = id;
  return TRUE;
}

static void
session_auth_closed (ChromiumSession *session, int id, int browser,
                     gpointer data)
{
  (void) session; (void) data;
  if (demo_auth_id == id)
    demo_auth_id = 0;
  g_printerr ("demo: auth-closed id=%d browser=%d\n", id, browser);
}

static gboolean
session_client_cert_request (ChromiumSession *session, int id, int browser,
                             const char *origin, GList *certs, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: client-cert-request id=%d browser=%d origin=%s n=%u\n",
              id, browser, origin ? origin : "", g_list_length (certs));
  for (GList *l = certs; l; l = l->next)
    {
      ChromiumClientCert *cert = l->data;
      g_printerr ("demo: client-cert subject=%s issuer=%s fp=%s\n",
                  cert->subject ? cert->subject : "",
                  cert->issuer ? cert->issuer : "",
                  cert->fingerprint ? cert->fingerprint : "");
    }
  return FALSE;   /* "none": the only default that cannot leak a key */
}

static void
session_client_cert_closed (ChromiumSession *session, int id, int browser,
                            gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: client-cert-closed id=%d browser=%d\n", id, browser);
}

static void
session_tls_info (ChromiumSession *session, int id, int browser, gboolean ok,
                  const char *security_state, const char *subject,
                  const char *issuer, const char *fingerprint,
                  int valid_from, int valid_to, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: tls-info id=%d browser=%d ok=%d state=%s subject=%s "
              "issuer=%s fp=%s from=%d to=%d\n",
              id, browser, ok ? 1 : 0,
              security_state ? security_state : "",
              subject ? subject : "", issuer ? issuer : "",
              fingerprint ? fingerprint : "", valid_from, valid_to);
}

static gboolean
session_scheme_request (ChromiumSession *session, int id, int browser,
                        const char *url, const char *method,
                        const char *headers, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: scheme-request id=%d browser=%d %s %s headers=%s\n",
              id, browser, method ? method : "", url ? url : "",
              headers ? headers : "{}");
  if (!g_getenv ("CEFNEST_SCHEME_SERVE"))
    return FALSE;   /* the session's default: abort */
  demo_scheme_id = id;
  return TRUE;
}

static void
session_scheme_closed (ChromiumSession *session, int id, int browser,
                       gpointer data)
{
  (void) session; (void) data;
  if (demo_scheme_id == id)
    demo_scheme_id = 0;
  g_printerr ("demo: scheme-closed id=%d browser=%d\n", id, browser);
}

static void
session_screenshot_result (ChromiumSession *session, int id, int browser,
                           gboolean ok, GBytes *png, const char *message,
                           gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: screenshot id=%d browser=%d ok=%d bytes=%zu %s\n",
              id, browser, ok ? 1 : 0, png ? g_bytes_get_size (png) : 0,
              message ? message : "");
  /* The bytes go where CEFNEST_SCREENSHOT_OUT names, so a gate can read a
     PNG back from disk instead of trusting a log line. */
  const char *out = g_getenv ("CEFNEST_SCREENSHOT_OUT");
  if (ok && png && out && *out)
    {
      GError *error = NULL;
      gsize length = 0;
      gconstpointer body = g_bytes_get_data (png, &length);

      g_file_set_contents (out, body, (gssize) length, &error);
      if (error)
        {
          g_printerr ("demo: screenshot write failed: %s\n", error->message);
          g_error_free (error);
        }
      else
        g_printerr ("demo: screenshot written %s\n", out);
    }
}

static void
session_window_properties (ChromiumSession *session, int browser, int x, int y,
                           int w, int h, gboolean toolbar, gboolean menubar,
                           gboolean status, gboolean resizable,
                           gboolean fullscreen, gpointer data)
{
  (void) session; (void) data;
  g_printerr ("demo: window-properties browser=%d x=%d y=%d w=%d h=%d "
              "toolbar=%d menubar=%d status=%d resizable=%d fullscreen=%d\n",
              browser, x, y, w, h, toolbar ? 1 : 0, menubar ? 1 : 0,
              status ? 1 : 0, resizable ? 1 : 0, fullscreen ? 1 : 0);
}

/* A menu action that the helper executes is only half of a browser action:
 * copying is the host clipboard, and "new window" is a second widget with its
 * own helper. Each helper gets its own ephemeral profile unless its host names
 * a data directory, so two windows no longer fight over one Chromium profile
 * and this is just "start another copy of the demo". */
static void
open_url_in_new_window (const char *url)
{
  GError *error = NULL;
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", &error);
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

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
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
session_view_source (ChromiumSession *session, int browser, const char *url,
                     const char *source, gpointer data)
{
  (void) session; (void) browser; (void) data;
  const char *dump = g_getenv ("CEFNEST_SOURCE_DUMP");

  /* Debug helper: what actually arrived, next to what was sent. */
  if (dump && *dump && source)
    g_file_set_contents (dump, source, (gssize) strlen (source), NULL);

  /* The source gate grades this line: url, byte count, a content hash (the
   * same 32-bit g_str_hash the gate recomputes), and — through the source
   * window's own debug line — the truncated marker. */
  g_printerr ("demo: view-source url=%s bytes=%zu hash=%u\n",
              url ? url : "", source ? strlen (source) : (size_t) 0,
              source ? g_str_hash (source) : 0);
}

static void
session_view_source_error (ChromiumSession *session, int browser,
                           const char *message, gpointer data)
{
  (void) session; (void) data;

  g_printerr ("demo: view-source-error browser=%d %s\n", browser,
              message ? message : "");
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
  (void) data;

  if (!url || !*url)
    return;

  g_printerr ("demo: open-url %s\n", url);
  /* The single-helper path: a standalone browser in the same helper gets its
   * own GTK window through ::browser-created. The separate-process spawn below
   * is only for the pathological case of the signal without a live session. */
  if (session && demo.view && demo.browser_windows)
    {
      chromium_session_new_browser (session, url, TRUE);
      return;
    }
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
  char *url;
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
  g_free (row->url);
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

/* Scripted runs cannot know Chromium's download ids in advance — they keep
 * counting up across runs from one cache directory — but they do know what
 * they asked the page for. A substring of the suggested name or the URL is
 * what a gate names a download with when two are alive at once. NULL matches
 * every row the picker accepts. */
static DownloadRow *
download_find_row (const char *substring, gboolean (*pick) (const DownloadRow *row))
{
  GtkWidget *child;

  for (child = demo.downloads ? gtk_widget_get_first_child (demo.downloads) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      DownloadRow *row = g_object_get_data (G_OBJECT (child), "cefnest-download");

      if (!row || !pick (row))
        continue;
      if (!substring ||
          (row->name && strstr (row->name, substring)) ||
          (row->url && strstr (row->url, substring)))
        return row;
    }
  return NULL;
}

static DownloadRow *
download_first_row (gboolean (*pick) (const DownloadRow *row))
{
  return download_find_row (NULL, pick);
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

/* A leaf is 255 bytes on the filesystems this targets, and a page that suggests
 * its whole <title> as a file name is not asking for 4 kB: past the limit there
 * is no file the helper can make and the download only fails. The cut comes out
 * of the stem so the leaf stays openable as the type it claims to be, and a
 * "suffix" longer than this is not a suffix. */
#define DOWNLOAD_STEM_MAX 200
#define DOWNLOAD_SUFFIX_MAX 16

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

  if (strlen (out) > DOWNLOAD_STEM_MAX + DOWNLOAD_SUFFIX_MAX)
    {
      const char *dot = strrchr (out, '.');
      g_autofree char *suffix =
        (dot && dot > out && strlen (dot) <= DOWNLOAD_SUFFIX_MAX) ?
          g_strdup (dot) : g_strdup ("");
      const char *cut = g_utf8_offset_to_pointer (out,
                                                  g_utf8_strlen (out, DOWNLOAD_STEM_MAX));
      g_autofree char *stem = g_strndup (out, (gsize) (cut - out));

      /* Counted in characters, not bytes: a name out of a page is UTF-8, and a
       * cut in the middle of one character is a leaf that no decoder reads. */
      g_free (out);
      out = g_strconcat (stem, suffix, NULL);
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

  /* A thousand of one name is a page that will not be satisfied by any leaf, and
   * the two answers left are to replace a path the host already handed out — the
   * one thing this loop exists to prevent — or to build the suffix as its own
   * directory component, which is what this line used to do: it handed out
   * <dir>/report/.pdf, a file under a directory nobody made, and the download
   * died on the way in. A leaf with the clock in it is neither. */
  {
    g_autofree char *leaf = g_strdup_printf ("%s (%" G_GINT64_FORMAT ")%s",
                                             stem, g_get_real_time (), ext);

    return g_build_filename (dir, leaf, NULL);
  }
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
  row->url = g_strdup (url);
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

  chromium_session_find (demo.session, 0, find_pattern, forward,
                         find_match_case, next);
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
    chromium_session_stop_finding (demo.session, 0, TRUE);
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
          chromium_session_zoom (demo.session, 0, "in");
          return TRUE;
        }
      if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract)
        {
          chromium_session_zoom (demo.session, 0, "out");
          return TRUE;
        }
      if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0)
        {
          chromium_session_zoom (demo.session, 0, "reset");
          return TRUE;
        }
    }

  /* Capture phase, on purpose: Esc inside the page is what the helper reads as
   * "close the browser", so a find bar that wants
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
     * total, not "第 0 / N 处". */
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

/* ------------------------------------------------------------------ *
 * JavaScript and file dialogs                                        *
 * ------------------------------------------------------------------ *
 *
 * Both arrive parked: the page cannot move until the host answers, and the
 * session's default answers only apply to a host that does not take the
 * signal. This panel is the half that takes it — a row per dialog, 确定 /
 * 取消, and for a prompt the entry the page asked for. A file dialog gets a
 * real GTK chooser; a scripted run stands in for the pointer with
 * js-dialog-respond / file-dialog-respond.
 *
 * CEFNEST_DIALOG=default makes the demo decline the signal instead, so the
 * session answers with Chrome's defaults. That is how a gate checks what a
 * page sees when its host has no dialog UI at all. */

typedef struct {
  int id;
  gboolean decided;
  GtkWidget *info;
  GtkWidget *entry;      /* prompt only, NULL otherwise */
  GtkWidget *ok;
  GtkWidget *cancel;
} JsDialogRow;

typedef struct {
  int id;
  gboolean decided;
  GtkWidget *info;
  GtkWidget *choose;
  GtkWidget *cancel;
  char *mode;
  char *title;
  char *default_path;
  GPtrArray *accept;     /* char *, owned */
} FileDialogRow;

static const char *
dialog_policy (void)
{
  const char *env = g_getenv ("CEFNEST_DIALOG");

  return env && !strcmp (env, "default") ? "default" : "ask";
}

/* A page's message is one string it chose, and this transcript is line-based:
 * a newline inside it would forge a line the gate might believe. */
static char *
transcript_text (const char *text)
{
  char *copy = g_strdup (text ? text : "");
  char *p;

  for (p = copy; *p; p++)
    if ((guchar) *p < 0x20 || (guchar) *p == 0x7f)
      *p = ' ';
  return copy;
}

static JsDialogRow *
js_dialog_row_for (int id)
{
  GtkWidget *child;

  for (child = demo.dialogs ? gtk_widget_get_first_child (demo.dialogs) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      JsDialogRow *row = g_object_get_data (G_OBJECT (child), "cefnest-js-dialog");

      if (row && row->id == id)
        return row;
    }
  return NULL;
}

static FileDialogRow *
file_dialog_row_for (int id)
{
  GtkWidget *child;

  for (child = demo.dialogs ? gtk_widget_get_first_child (demo.dialogs) : NULL;
       child; child = gtk_widget_get_next_sibling (child))
    {
      FileDialogRow *row = g_object_get_data (G_OBJECT (child), "cefnest-file-dialog");

      if (row && row->id == id)
        return row;
    }
  return NULL;
}

static void
js_dialog_answer (JsDialogRow *row, gboolean ok, const char *value)
{
  g_autofree char *safe = transcript_text (value);

  if (!row || row->decided)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->ok, FALSE);
  gtk_widget_set_visible (row->cancel, FALSE);
  if (row->entry)
    gtk_widget_set_sensitive (row->entry, FALSE);
  gtk_label_set_text (GTK_LABEL (row->info), ok ? "已确认" : "已取消");

  chromium_session_js_dialog_respond (demo.session, row->id, ok,
                                      value ? value : "");
  g_printerr ("demo: js-dialog %d %s value=%s\n", row->id,
              ok ? "ok" : "cancel", safe);
}

static void
js_dialog_ok_clicked (GtkButton *button, gpointer data)
{
  JsDialogRow *row = data;
  const char *value = row->entry
    ? gtk_editable_get_text (GTK_EDITABLE (row->entry)) : "";

  js_dialog_answer (row, TRUE, value);
}

static void
js_dialog_cancel_clicked (GtkButton *button, gpointer data)
{
  js_dialog_answer ((JsDialogRow *) data, FALSE, "");
}

/* CDP answers and events: the demo prints them, which is the whole host
   side a scripted run needs — the assertions read these lines. */
static void
session_cdp_result (ChromiumSession *session, int id, gboolean ok,
                    const char *json, const char *message, gpointer data)
{
  g_printerr ("demo: cdp-result id=%d ok=%d message=%s json=%.512s%s\n",
              id, ok ? 1 : 0, message ? message : "", json ? json : "",
              (json && strlen (json) > 512) ? "…" : "");
}

static void
session_cdp_event (ChromiumSession *session, int browser, const char *method,
                   const char *params, const char *message, gpointer data)
{
  g_printerr ("demo: cdp-event browser=%d method=%s params=%s message=%s\n",
              browser, method ? method : "", params ? params : "",
              message ? message : "");
}

/* The intercept question: the demo prints what arrived and answers nothing.
 * CEFNEST_INTERCEPT=allow makes it answer allow immediately, which is what
 * the passive-adjacent scenarios want without a scripted response step. */
static void
session_request_intercept (ChromiumSession *session, int request_id,
                           int browser, const char *url, const char *method,
                           const char *resource_type, gboolean is_main_frame,
                           const char *headers, gpointer data)
{
  g_printerr ("demo: request-intercept id=%d browser=%d type=%s main=%d "
              "method=%s url=%s\n",
              request_id, browser, resource_type ? resource_type : "",
              is_main_frame ? 1 : 0, method ? method : "",
              url ? url : "");
}

static void
session_request_intercept_closed (ChromiumSession *session, int request_id,
                                  int browser, gpointer data)
{
  g_printerr ("demo: request-intercept-closed id=%d browser=%d\n",
              request_id, browser);
}

/* Certificate questions: the demo prints the facts a host would put in its
   own prompt. CEFNEST_CERTIFICATE=ask takes the question and leaves it to
   the certificate-respond step — how test/certificate-gate.sh drives both
   decisions; the default leaves it with the session, which denies. */
static const char *
certificate_policy (void)
{
  const char *env = g_getenv ("CEFNEST_CERTIFICATE");

  return env && !strcmp (env, "ask") ? "ask" : "default";
}

static gboolean
session_certificate_error (ChromiumSession *session, int id, int browser,
                           const char *error, const char *url,
                           const char *subject, const char *issuer,
                           const char *fingerprint, gpointer data)
{
  g_printerr ("demo: certificate-error id=%d browser=%d error=%s url=%s "
              "subject=%s issuer=%s fingerprint=%s policy=%s\n",
              id, browser, error ? error : "", url ? url : "",
              subject ? subject : "", issuer ? issuer : "",
              fingerprint ? fingerprint : "", certificate_policy ());
  return !strcmp (certificate_policy (), "ask");
}

static void
session_certificate_closed (ChromiumSession *session, int id, int browser,
                            gpointer data)
{
  g_printerr ("demo: certificate-error-closed id=%d browser=%d\n", id,
              browser);
}

static gboolean
session_js_dialog (ChromiumSession *session, int id, int browser,
                   const char *kind, const char *origin, const char *message,
                   const char *default_value, gpointer data)
{
  g_autofree char *safe_origin = transcript_text (origin);
  g_autofree char *safe_message = transcript_text (message);
  g_autofree char *safe_default = transcript_text (default_value);
  const char *policy = dialog_policy ();
  GtkWidget *row_box;
  GtkWidget *info;
  GtkWidget *ok;
  GtkWidget *cancel;
  GtkWidget *entry = NULL;
  GtkWidget *controls;
  JsDialogRow *row;
  g_autofree char *text = NULL;

  /* Both strings are what a script asserts on, so both are here even when the
   * panel is not on screen (CEFNEST_DIALOG=default). */
  g_printerr ("demo: js-dialog %d browser=%d kind=%s origin=%s message=%s "
              "default_value=%s policy=%s\n",
              id, browser, kind ? kind : "", safe_origin, safe_message,
              safe_default, policy);

  if (!strcmp (policy, "default"))
    return FALSE;

  row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  info = gtk_label_new ("");
  ok = gtk_button_new_with_label ("确定");
  cancel = gtk_button_new_with_label ("取消");

  text = g_strdup_printf ("%s · %s · %s", kind, safe_origin, safe_message);
  gtk_label_set_text (GTK_LABEL (info), text);
  gtk_label_set_xalign (GTK_LABEL (info), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (info), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_tooltip_text (ok, "应答这个对话框 / Answer it");
  gtk_widget_set_tooltip_text (cancel, "拒绝 / Dismiss it");

  gtk_box_append (GTK_BOX (row_box), info);
  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  if (!strcmp (kind, "prompt"))
    {
      entry = gtk_entry_new ();
      gtk_editable_set_text (GTK_EDITABLE (entry), default_value);
      gtk_editable_set_width_chars (GTK_EDITABLE (entry), 18);
      gtk_box_append (GTK_BOX (controls), entry);
    }
  gtk_box_append (GTK_BOX (controls), ok);
  gtk_box_append (GTK_BOX (controls), cancel);
  gtk_box_append (GTK_BOX (row_box), controls);
  /* An alert has one answer; the other kinds have both. The script can still
   * answer any of them, which is why both buttons exist in the object tree. */
  if (!strcmp (kind, "alert"))
    gtk_widget_set_visible (cancel, FALSE);
  if (!strcmp (kind, "beforeunload"))
    {
      gtk_button_set_label (GTK_BUTTON (ok), "离开");
      gtk_button_set_label (GTK_BUTTON (cancel), "留下");
    }

  row = g_new0 (JsDialogRow, 1);
  row->id = id;
  row->info = info;
  row->entry = entry;
  row->ok = ok;
  row->cancel = cancel;
  g_object_set_data_full (G_OBJECT (row_box), "cefnest-js-dialog", row, g_free);
  g_signal_connect (ok, "clicked", G_CALLBACK (js_dialog_ok_clicked), row);
  g_signal_connect (cancel, "clicked", G_CALLBACK (js_dialog_cancel_clicked), row);

  gtk_widget_set_visible (demo.dialogs, TRUE);
  gtk_box_append (GTK_BOX (demo.dialogs), row_box);

  /* Taking the signal is taking the question: the session will not answer a
   * dialog this handler said it owns. */
  return TRUE;
}

static void
session_js_dialog_closed (ChromiumSession *session, int id, int browser,
                          gpointer data)
{
  JsDialogRow *row = js_dialog_row_for (id);

  g_printerr ("demo: js-dialog %d closed\n", id);

  if (!row || row->decided)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->ok, FALSE);
  gtk_widget_set_visible (row->cancel, FALSE);
  if (row->entry)
    gtk_widget_set_sensitive (row->entry, FALSE);
  gtk_label_set_text (GTK_LABEL (row->info), "页面已经不再等这个对话框");
}

static void
file_dialog_answer (FileDialogRow *row, const char *const *paths)
{
  g_autoptr (GString) joined = g_string_new ("");
  gsize i;

  if (!row || row->decided)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->choose, FALSE);
  gtk_widget_set_visible (row->cancel, FALSE);

  for (i = 0; paths && paths[i]; i++)
    {
      if (joined->len)
        g_string_append (joined, ";");
      g_string_append (joined, paths[i]);
    }

  gtk_label_set_text (GTK_LABEL (row->info),
                      joined->len ? joined->str : "已取消");
  chromium_session_file_dialog_respond (demo.session, row->id, paths);
  g_printerr ("demo: file-dialog %d %s%s\n", row->id,
              joined->len ? "answer " : "cancel", joined->str);
}

static void
file_dialog_cancel_clicked (GtkButton *button, gpointer data)
{
  file_dialog_answer ((FileDialogRow *) data, NULL);
}

/* The chooser's own answer, which can arrive after the script already answered
 * (the row is then decided and this is only cleanup). */
static void
file_dialog_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *row_box = data;
  FileDialogRow *row = g_object_get_data (G_OBJECT (row_box), "cefnest-file-dialog");
  GError *error = NULL;
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
  gboolean multiple = row && !strcmp (row->mode, "open-multiple");
  gboolean folder = row && !strcmp (row->mode, "open-folder");
  gboolean save = row && !strcmp (row->mode, "save");

  if (multiple)
    {
      GListModel *files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source),
                                                                result, &error);
      guint n = files ? g_list_model_get_n_items (files) : 0;
      guint i;

      for (i = 0; i < n; i++)
        {
          GFile *file = g_list_model_get_item (files, i);
          char *path = g_file_get_path (file);

          if (path)
            g_ptr_array_add (paths, path);
          g_object_unref (file);
        }
      g_clear_object (&files);
    }
  else
    {
      GFile *file = folder
        ? gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), result, &error)
        : save
          ? gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error)
          : gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

      if (file)
        {
          char *path = g_file_get_path (file);

          if (path)
            g_ptr_array_add (paths, path);
          g_object_unref (file);
        }
    }

  if (error)
    {
      /* A dismissed chooser is a cancel, which is the same answer the empty
       * list carries. Nothing here is an error worth a warning. */
      g_error_free (error);
      g_ptr_array_set_size (paths, 0);
    }

  g_ptr_array_add (paths, NULL);
  if (row)
    file_dialog_answer (row, (const char *const *) paths->pdata);
  g_ptr_array_unref (paths);
  g_object_unref (row_box);
}

static void
file_dialog_choose_clicked (GtkButton *button, gpointer data)
{
  GtkWidget *row_box = data;
  FileDialogRow *row = g_object_get_data (G_OBJECT (row_box), "cefnest-file-dialog");
  GtkFileDialog *dialog;
  GtkWindow *parent = demo.window ? GTK_WINDOW (demo.window) : NULL;
  GListStore *filters = NULL;
  gsize i;

  if (!row || row->decided)
    return;

  dialog = gtk_file_dialog_new ();
  if (row->title && *row->title)
    gtk_file_dialog_set_title (dialog, row->title);
  if (row->default_path && *row->default_path)
    {
      GFile *initial = g_file_new_for_path (row->default_path);

      if (!strcmp (row->mode, "open-folder"))
        gtk_file_dialog_set_initial_folder (dialog, initial);
      else
        gtk_file_dialog_set_initial_file (dialog, initial);
      g_object_unref (initial);
    }
  if (row->accept && row->accept->len)
    {
      filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
      for (i = 0; i < row->accept->len; i++)
        {
          const char *text = g_ptr_array_index (row->accept, i);
          GtkFileFilter *filter = gtk_file_filter_new ();

          gtk_file_filter_set_name (filter, text);
          if (strchr (text, '/'))
            gtk_file_filter_add_mime_type (filter, text);
          else if (text[0] == '.')
            {
              g_autofree char *pattern = g_strconcat ("*", text, NULL);

              gtk_file_filter_add_pattern (filter, pattern);
            }
          else
            gtk_file_filter_add_pattern (filter, text);
          g_list_store_append (filters, filter);
          g_object_unref (filter);
        }
      gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
      g_object_unref (filters);
    }

  /* Held across the async answer: the row must stay alive until its chooser
   * has finished, even if the panel is rebuilt in the meantime. */
  g_object_ref (row_box);
  if (!strcmp (row->mode, "open-multiple"))
    gtk_file_dialog_open_multiple (dialog, parent, NULL, file_dialog_chosen, row_box);
  else if (!strcmp (row->mode, "open-folder"))
    gtk_file_dialog_select_folder (dialog, parent, NULL, file_dialog_chosen, row_box);
  else if (!strcmp (row->mode, "save"))
    gtk_file_dialog_save (dialog, parent, NULL, file_dialog_chosen, row_box);
  else
    gtk_file_dialog_open (dialog, parent, NULL, file_dialog_chosen, row_box);
  g_object_unref (dialog);
}

static void
file_dialog_row_free (gpointer data)
{
  FileDialogRow *row = data;

  g_free (row->mode);
  g_free (row->title);
  g_free (row->default_path);
  if (row->accept)
    g_ptr_array_unref (row->accept);
  g_free (row);
}

static gboolean
session_file_dialog (ChromiumSession *session, int id, int browser,
                     const char *mode, const char *title,
                     const char *default_path, GList *accept, gpointer data)
{
  g_autofree char *safe_title = transcript_text (title);
  g_autofree char *safe_path = transcript_text (default_path);
  g_autoptr (GString) joined = g_string_new ("");
  const char *policy = dialog_policy ();
  GtkWidget *row_box;
  GtkWidget *info;
  GtkWidget *choose;
  GtkWidget *cancel;
  GtkWidget *controls;
  FileDialogRow *row;
  GList *l;
  g_autofree char *text = NULL;

  for (l = accept; l; l = l->next)
    {
      if (joined->len)
        g_string_append_c (joined, '+');
      g_string_append (joined, (const char *) l->data);
    }

  g_printerr ("demo: file-dialog %d browser=%d mode=%s title=%s "
              "default_path=%s accept=%s policy=%s\n",
              id, browser, mode ? mode : "", safe_title, safe_path,
              joined->len ? joined->str : "none", policy);

  if (!strcmp (policy, "default"))
    return FALSE;

  row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  info = gtk_label_new ("");
  choose = gtk_button_new_with_label ("选择…");
  cancel = gtk_button_new_with_label ("取消");

  text = g_strdup_printf ("%s · %s · %s", mode, safe_title, joined->str);
  gtk_label_set_text (GTK_LABEL (info), text);
  gtk_label_set_xalign (GTK_LABEL (info), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (info), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_tooltip_text (choose, "打开文件选择器 / Open the chooser");
  gtk_widget_set_tooltip_text (cancel, "取消这次选择 / Cancel it");

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (controls), choose);
  gtk_box_append (GTK_BOX (controls), cancel);
  gtk_box_append (GTK_BOX (row_box), info);
  gtk_box_append (GTK_BOX (row_box), controls);

  row = g_new0 (FileDialogRow, 1);
  row->id = id;
  row->info = info;
  row->choose = choose;
  row->cancel = cancel;
  row->mode = g_strdup (mode ? mode : "open");
  row->title = g_strdup (title ? title : "");
  row->default_path = g_strdup (default_path ? default_path : "");
  row->accept = g_ptr_array_new_with_free_func (g_free);
  for (l = accept; l; l = l->next)
    g_ptr_array_add (row->accept, g_strdup ((const char *) l->data));

  g_object_set_data_full (G_OBJECT (row_box), "cefnest-file-dialog", row,
                          file_dialog_row_free);
  g_signal_connect (choose, "clicked", G_CALLBACK (file_dialog_choose_clicked),
                    row_box);
  g_signal_connect (cancel, "clicked", G_CALLBACK (file_dialog_cancel_clicked),
                    row);

  gtk_widget_set_visible (demo.dialogs, TRUE);
  gtk_box_append (GTK_BOX (demo.dialogs), row_box);
  return TRUE;
}

static void
session_file_dialog_closed (ChromiumSession *session, int id, int browser,
                            gpointer data)
{
  FileDialogRow *row = file_dialog_row_for (id);

  g_printerr ("demo: file-dialog %d closed\n", id);

  if (!row || row->decided)
    return;

  row->decided = TRUE;
  gtk_widget_set_visible (row->choose, FALSE);
  gtk_widget_set_visible (row->cancel, FALSE);
  gtk_label_set_text (GTK_LABEL (row->info), "页面已经不再等这个选择");
}

static void
build_dialog_panel (GtkBox *box)
{
  demo.dialogs = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  g_printerr ("demo: dialogs: policy=%s\n", dialog_policy ());

  gtk_widget_set_margin_start (demo.dialogs, 8);
  gtk_widget_set_margin_end (demo.dialogs, 8);
  gtk_widget_set_margin_top (demo.dialogs, 4);
  gtk_widget_set_margin_bottom (demo.dialogs, 4);
  /* Nothing to say until a page asks for something. */
  gtk_widget_set_visible (demo.dialogs, FALSE);
  gtk_box_append (box, demo.dialogs);
}

static void
session_loading_progress (ChromiumSession *session, double progress, gpointer data)
{
  g_printerr ("demo: loading-progress %.3f\n", progress);
}

static void
session_auto_resize_updated (ChromiumSession *session, int browser, int width,
                             int height, gpointer data)
{
  g_printerr ("demo: auto-resize %dx%d browser=%d\n", width, height, browser);
}

static void
session_fullscreen_changed (ChromiumSession *session, gboolean fullscreen,
                            gpointer data)
{
  g_printerr ("demo: fullscreen %d\n", fullscreen ? 1 : 0);
}

/* The browser-data answers. Each is one line on stderr with the helper's own
 * JSON payload, so a gate can read the fields without a second protocol. */
static void
session_cookies_result (ChromiumSession *session, int id, gboolean ok,
                        const char *json, gpointer data)
{
  g_printerr ("demo: cookies id=%d ok=%d json=%s\n", id, ok ? 1 : 0, json);
}

static void
session_history_result (ChromiumSession *session, int id, const char *json,
                        gpointer data)
{
  g_printerr ("demo: history id=%d json=%s\n", id, json);
}

/* Page text: the demo joins the chunks so the log line has the whole text
 * (control characters folded to spaces, the first 400 bytes shown). */
static struct {
  int id;
  GString *text;
  gboolean truncated;
} page_text_sink;

static void
session_page_text_begin (ChromiumSession *session, int id, int browser,
                         int bytes, gboolean truncated, gpointer data)
{
  if (page_text_sink.text)
    g_string_free (page_text_sink.text, TRUE);
  page_text_sink.id = id;
  page_text_sink.text = g_string_new (NULL);
  page_text_sink.truncated = truncated;
  g_printerr ("demo: page-text-begin id=%d browser=%d bytes=%d truncated=%d\n",
              id, browser, bytes, truncated ? 1 : 0);
}

static void
session_page_text_chunk (ChromiumSession *session, int id, const char *data,
                         gpointer data_unused)
{
  if (page_text_sink.text && id == page_text_sink.id)
    g_string_append (page_text_sink.text, data);
}

static void
session_page_text_end (ChromiumSession *session, int id, gboolean truncated,
                       gpointer data)
{
  g_autofree char *shown = NULL;
  const char *text = page_text_sink.text ? page_text_sink.text->str : "";

  shown = g_strndup (text, 400);
  for (char *p = shown; *p; p++)
    if ((guchar) *p < 0x20)
      *p = ' ';
  g_printerr ("demo: page-text id=%d bytes=%zu truncated=%d text=\"%s\"\n",
              id, page_text_sink.text ? page_text_sink.text->len : 0,
              truncated ? 1 : 0, shown);
  if (page_text_sink.text)
    g_string_free (page_text_sink.text, TRUE);
  page_text_sink.text = NULL;
}

/* PDF bytes: collected per request, written to $CEFNEST_PDF_OUT when the host
 * named a file, and summarized on stderr — head and tail are what tell a real
 * PDF from an error page. */
static struct {
  int id;
  GByteArray *bytes;
} pdf_sink;

static void
session_pdf_begin (ChromiumSession *session, int id, int bytes,
                   gboolean truncated, gpointer data)
{
  g_clear_pointer (&pdf_sink.bytes, g_byte_array_unref);
  pdf_sink.id = id;
  pdf_sink.bytes = g_byte_array_new ();
  g_printerr ("demo: pdf-begin id=%d bytes=%d truncated=%d\n", id, bytes,
              truncated ? 1 : 0);
}

static void
session_pdf_chunk (ChromiumSession *session, int id, GBytes *data,
                   gpointer data_unused)
{
  gsize length = 0;
  const void *bytes = g_bytes_get_data (data, &length);

  if (pdf_sink.bytes && id == pdf_sink.id)
    g_byte_array_append (pdf_sink.bytes, bytes, length);
}

static void
session_pdf_end (ChromiumSession *session, int id, int bytes, gboolean truncated,
                 const char *error, gpointer data)
{
  const char *path = g_getenv ("CEFNEST_PDF_OUT");
  const guint8 *bytes_data = pdf_sink.bytes ? pdf_sink.bytes->data : NULL;
  gsize length = pdf_sink.bytes ? pdf_sink.bytes->len : 0;
  gboolean magic = length >= 5 && !memcmp (bytes_data, "%PDF-", 5);
  gboolean eof = FALSE;

  if (length >= 8)
    for (gsize at = length > 1024 ? length - 1024 : 0; at + 5 <= length; at++)
      if (!memcmp (bytes_data + at, "%%EOF", 5))
        eof = TRUE;

  if (path && *path)
    g_file_set_contents (path, (const char *) bytes_data, (gssize) length, NULL);
  g_printerr ("demo: pdf id=%d bytes=%d truncated=%d error=\"%s\" magic=%d "
              "eof=%d\n",
              id, bytes, truncated ? 1 : 0, error ? error : "", magic ? 1 : 0,
              eof ? 1 : 0);
  g_clear_pointer (&pdf_sink.bytes, g_byte_array_unref);
}

static void
session_favicon_changed (ChromiumSession *session, int browser,
                         const char **urls, gpointer data)
{
  g_autofree char *joined = g_strjoinv (",", (char **) urls);

  g_printerr ("demo: favicon browser=%d urls=%s\n", browser, joined);
}

static void
session_console (ChromiumSession *session, int browser, const char *level,
                 const char *message, const char *source, int line,
                 gpointer data)
{
  g_printerr ("demo: console browser=%d level=%s source=%s:%d message=%s\n",
              browser, level, source, line, message);
}

static void
session_tooltip_changed (ChromiumSession *session, int browser,
                         const char *text, gpointer data)
{
  g_printerr ("demo: tooltip browser=%d text=%s\n", browser, text);
}

static void
session_status_message (ChromiumSession *session, int browser,
                        const char *text, gpointer data)
{
  g_printerr ("demo: status-message browser=%d text=%s\n", browser, text);
}

static void
session_focus_requested (ChromiumSession *session, int browser,
                         const char *source, gpointer data)
{
  g_printerr ("demo: focus-requested browser=%d source=%s\n", browser, source);
}

static void
session_media_access_changed (ChromiumSession *session, int browser,
                              gboolean video, gboolean audio, gpointer data)
{
  g_printerr ("demo: media-access browser=%d video=%d audio=%d\n",
              browser, video ? 1 : 0, audio ? 1 : 0);
}

static void
session_print_requested (ChromiumSession *session, int browser,
                         gpointer data)
{
  g_printerr ("demo: print-requested browser=%d\n", browser);
}

static void
session_render_unresponsive (ChromiumSession *session, int id, int browser,
                             gpointer data)
{
  /* The id is what the hang-respond step names. */
  demo.pending_hang_id = id;
  g_printerr ("demo: render-unresponsive id=%d browser=%d\n", id, browser);
}

static void
session_render_responsive (ChromiumSession *session, int id, int browser,
                           gpointer data)
{
  g_printerr ("demo: render-responsive id=%d browser=%d\n", id, browser);
}

static void
session_render_unresponsive_closed (ChromiumSession *session, int id,
                                    int browser, gpointer data)
{
  g_printerr ("demo: render-unresponsive-closed id=%d browser=%d\n",
              id, browser);
}

static void
session_frame_created (ChromiumSession *session, int browser,
                       const char *frame, gboolean is_main, const char *url,
                       gpointer data)
{
  g_printerr ("demo: frame-created browser=%d frame=%s main=%d url=%s\n",
              browser, frame, is_main ? 1 : 0, url);
}

static void
session_frame_destroyed (ChromiumSession *session, int browser,
                         const char *frame, gpointer data)
{
  g_printerr ("demo: frame-destroyed browser=%d frame=%s\n", browser, frame);
}

static void
session_frame_attached (ChromiumSession *session, int browser,
                        const char *frame, gboolean reattached,
                        gpointer data)
{
  g_printerr ("demo: frame-attached browser=%d frame=%s re=%d\n",
              browser, frame, reattached ? 1 : 0);
}

static void
session_frame_detached (ChromiumSession *session, int browser,
                        const char *frame, gpointer data)
{
  g_printerr ("demo: frame-detached browser=%d frame=%s\n", browser, frame);
}

static void
session_main_frame_changed (ChromiumSession *session, int browser,
                            const char *old_frame, const char *new_frame,
                            gpointer data)
{
  g_printerr ("demo: main-frame-changed browser=%d old=%s new=%s\n",
              browser, old_frame, new_frame);
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

  /* Every standalone and DevTools window shows a page nobody is serving any
   * more. gtk_window_destroy skips close-request, so no close-browser goes out
   * to a session that is not there, and the destroy handlers clear the tables
   * (removing the current key during a hash table walk is allowed). */
  if (demo.browser_windows && g_hash_table_size (demo.browser_windows) > 0)
    {
      /* Destroying a window removes it from this very table (the destroy
       * handler), so plain iteration trips GLib's version check — take the
       * windows out first and destroy the snapshot. */
      GList *windows = g_hash_table_get_values (demo.browser_windows);
      GList *l;

      g_hash_table_steal_all (demo.browser_windows);
      for (l = windows; l; l = l->next)
        gtk_window_destroy (GTK_WINDOW (l->data));
      g_list_free (windows);
    }
}

static void
session_helper_exited (ChromiumSession *session, gpointer data)
{
  demo.helper_exited = TRUE;
  g_printerr ("demo: helper process reaped\n");

  /* The failure was already reported when ::closed arrived; this is the point
   * where a quit can no longer pull the nested compositor out from under the
   * helper's CefShutdown(). */
  if (demo.quit_when_helper_exits)
    g_application_quit (G_APPLICATION (demo.app));
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
  chromium_session_load_uri (demo.session, 0, uri);
}

static void
go_back_clicked (GtkButton *button, gpointer data)
{
  chromium_session_go_back (demo.session, 0);
}

static void
go_forward_clicked (GtkButton *button, gpointer data)
{
  chromium_session_go_forward (demo.session, 0);
}

static void
reload_clicked (GtkButton *button, gpointer data)
{
  chromium_session_reload (demo.session, 0, FALSE);
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

  demo.header = GTK_WIDGET (header);
  demo.plugin_buttons = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);

  button = gtk_button_new_with_label ("+");
  gtk_widget_set_tooltip_text (button, "新标签 / New tab");
  g_signal_connect (button, "clicked", G_CALLBACK (new_tab_clicked), NULL);
  gtk_header_bar_pack_start (header, button);

  button = gtk_button_new_with_label ("▣");
  gtk_widget_set_tooltip_text (button, "新窗口 / New window");
  g_signal_connect (button, "clicked", G_CALLBACK (new_window_clicked), NULL);
  gtk_header_bar_pack_start (header, button);

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
  /* Before start(): the ready handshake carries the switch to the helper, the
   * same way the accessibility state does. */
  if (demo.devtools)
    chromium_session_set_devtools_enabled (demo.session, TRUE);
  g_signal_connect (demo.session, "ready", G_CALLBACK (session_ready), NULL);
  g_signal_connect (demo.session, "changed", G_CALLBACK (session_changed), NULL);
  g_signal_connect (demo.session, "zoom-changed",
                    G_CALLBACK (session_zoom_changed), NULL);
  g_signal_connect (demo.session, "copy-text",
                    G_CALLBACK (session_copy_text), NULL);
  g_signal_connect (demo.session, "open-url",
                    G_CALLBACK (session_open_url), NULL);
  g_signal_connect (demo.session, "view-source",
                    G_CALLBACK (session_view_source), NULL);
  g_signal_connect (demo.session, "view-source-error",
                    G_CALLBACK (session_view_source_error), NULL);
  g_signal_connect (demo.session, "load-started",
                    G_CALLBACK (session_load_started), NULL);
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
  g_signal_connect (demo.session, "browsers-changed",
                    G_CALLBACK (session_browsers_changed), NULL);
  g_signal_connect (demo.session, "auth-required",
                    G_CALLBACK (session_auth_required), NULL);
  g_signal_connect (demo.session, "auth-closed",
                    G_CALLBACK (session_auth_closed), NULL);
  g_signal_connect (demo.session, "client-cert-request",
                    G_CALLBACK (session_client_cert_request), NULL);
  g_signal_connect (demo.session, "client-cert-closed",
                    G_CALLBACK (session_client_cert_closed), NULL);
  g_signal_connect (demo.session, "tls-info",
                    G_CALLBACK (session_tls_info), NULL);
  g_signal_connect (demo.session, "scheme-request",
                    G_CALLBACK (session_scheme_request), NULL);
  g_signal_connect (demo.session, "scheme-closed",
                    G_CALLBACK (session_scheme_closed), NULL);
  g_signal_connect (demo.session, "screenshot-result",
                    G_CALLBACK (session_screenshot_result), NULL);
  g_signal_connect (demo.session, "window-properties",
                    G_CALLBACK (session_window_properties), NULL);
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
  g_signal_connect (demo.session, "js-dialog",
                    G_CALLBACK (session_js_dialog), NULL);
  g_signal_connect (demo.session, "js-dialog-closed",
                    G_CALLBACK (session_js_dialog_closed), NULL);
  g_signal_connect (demo.session, "certificate-error",
                    G_CALLBACK (session_certificate_error), NULL);
  g_signal_connect (demo.session, "certificate-error-closed",
                    G_CALLBACK (session_certificate_closed), NULL);
  g_signal_connect (demo.session, "request-intercept",
                    G_CALLBACK (session_request_intercept), NULL);
  g_signal_connect (demo.session, "request-intercept-closed",
                    G_CALLBACK (session_request_intercept_closed), NULL);
  g_signal_connect (demo.session, "cdp-result",
                    G_CALLBACK (session_cdp_result), NULL);
  g_signal_connect (demo.session, "cdp-event",
                    G_CALLBACK (session_cdp_event), NULL);
  g_signal_connect (demo.session, "file-dialog",
                    G_CALLBACK (session_file_dialog), NULL);
  g_signal_connect (demo.session, "file-dialog-closed",
                    G_CALLBACK (session_file_dialog_closed), NULL);
  g_signal_connect (demo.session, "loading-progress",
                    G_CALLBACK (session_loading_progress), NULL);
  g_signal_connect (demo.session, "cookies-result",
                    G_CALLBACK (session_cookies_result), NULL);
  g_signal_connect (demo.session, "history-result",
                    G_CALLBACK (session_history_result), NULL);
  g_signal_connect (demo.session, "page-text-begin",
                    G_CALLBACK (session_page_text_begin), NULL);
  g_signal_connect (demo.session, "page-text-chunk",
                    G_CALLBACK (session_page_text_chunk), NULL);
  g_signal_connect (demo.session, "page-text-end",
                    G_CALLBACK (session_page_text_end), NULL);
  g_signal_connect (demo.session, "pdf-begin",
                    G_CALLBACK (session_pdf_begin), NULL);
  g_signal_connect (demo.session, "pdf-chunk",
                    G_CALLBACK (session_pdf_chunk), NULL);
  g_signal_connect (demo.session, "pdf-end",
                    G_CALLBACK (session_pdf_end), NULL);
  g_signal_connect (demo.session, "favicon-changed",
                    G_CALLBACK (session_favicon_changed), NULL);
  g_signal_connect (demo.session, "console",
                    G_CALLBACK (session_console), NULL);
  g_signal_connect (demo.session, "tooltip-changed",
                    G_CALLBACK (session_tooltip_changed), NULL);
  g_signal_connect (demo.session, "status-message",
                    G_CALLBACK (session_status_message), NULL);
  g_signal_connect (demo.session, "focus-requested",
                    G_CALLBACK (session_focus_requested), NULL);
  g_signal_connect (demo.session, "media-access-changed",
                    G_CALLBACK (session_media_access_changed), NULL);
  g_signal_connect (demo.session, "print-requested",
                    G_CALLBACK (session_print_requested), NULL);
  g_signal_connect (demo.session, "render-unresponsive",
                    G_CALLBACK (session_render_unresponsive), NULL);
  g_signal_connect (demo.session, "render-responsive",
                    G_CALLBACK (session_render_responsive), NULL);
  g_signal_connect (demo.session, "render-unresponsive-closed",
                    G_CALLBACK (session_render_unresponsive_closed), NULL);
  g_signal_connect (demo.session, "frame-created",
                    G_CALLBACK (session_frame_created), NULL);
  g_signal_connect (demo.session, "frame-destroyed",
                    G_CALLBACK (session_frame_destroyed), NULL);
  g_signal_connect (demo.session, "frame-attached",
                    G_CALLBACK (session_frame_attached), NULL);
  g_signal_connect (demo.session, "frame-detached",
                    G_CALLBACK (session_frame_detached), NULL);
  g_signal_connect (demo.session, "main-frame-changed",
                    G_CALLBACK (session_main_frame_changed), NULL);
  g_signal_connect (demo.session, "auto-resize-updated",
                    G_CALLBACK (session_auto_resize_updated), NULL);
  g_signal_connect (demo.session, "fullscreen-changed",
                    G_CALLBACK (session_fullscreen_changed), NULL);
  g_signal_connect (demo.session, "closed", G_CALLBACK (session_closed), NULL);
  if (demo.plugin_buttons == NULL)
    demo.plugin_buttons = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);
  g_signal_connect (demo.session, "helper-exited",
                    G_CALLBACK (session_helper_exited), NULL);
  g_signal_connect (demo.session, "browser-created",
                    G_CALLBACK (session_browser_created), NULL);
  g_signal_connect (demo.session, "browser-selected",
                    G_CALLBACK (session_browser_selected), NULL);
  g_signal_connect (demo.session, "browser-closed",
                    G_CALLBACK (session_browser_closed), NULL);
  g_signal_connect (demo.session, "browser-surface",
                    G_CALLBACK (session_browser_surface), NULL);

  /* Test hook: the source gate turns the component's own window off to grade
   * the ::view-source-only path; a real host would set the property for its
   * own reasons (its own window management, usually). */
  if (g_strcmp0 (g_getenv ("CEFNEST_SOURCE_WINDOW"), "0") == 0)
    g_object_set (demo.session, "auto-source-window", FALSE, NULL);

  if (demo.data_directory)
    chromium_session_set_data_directory (demo.session, demo.data_directory);

  /* The start-time properties, read from the environment the same way the
   * download and permission knobs are: a gate drives the demo without a host
   * program of its own, and a real host would set the properties directly. */
  if (g_getenv ("CEFNEST_PROXY"))
    chromium_session_set_proxy (demo.session, g_getenv ("CEFNEST_PROXY"));
  if (g_getenv ("CEFNEST_PROXY_BYPASS"))
    chromium_session_set_proxy_bypass (demo.session, g_getenv ("CEFNEST_PROXY_BYPASS"));
  if (g_getenv ("CEFNEST_ACCEPT_LANG"))
    chromium_session_set_accept_language (demo.session,
                                          g_getenv ("CEFNEST_ACCEPT_LANG"));
  if (g_getenv ("CEFNEST_AUTOPLAY_POLICY"))
    chromium_session_set_autoplay_policy (demo.session,
                                          g_getenv ("CEFNEST_AUTOPLAY_POLICY"));
  if (g_getenv ("CEFNEST_AUTO_RESIZE"))
    {
      const char *bounds = g_getenv ("CEFNEST_AUTO_RESIZE_BOUNDS");
      int min_w = 0, min_h = 0, max_w = 0, max_h = 0;

      if (bounds &&
          4 == sscanf (bounds, "%d %d %d %d", &min_w, &min_h, &max_w, &max_h))
        chromium_session_set_auto_resize_bounds (demo.session, min_w, min_h,
                                                 max_w, max_h);
      chromium_session_set_auto_resize (demo.session, TRUE);
    }
  /* CEFNEST_SCHEMES: "name=flag,flag;name2" — the start-time half of the
     host-served schemes; the data factories ride the script. */
  if (g_getenv ("CEFNEST_SCHEMES"))
    {
      g_auto (GStrv) entries = g_strsplit (g_getenv ("CEFNEST_SCHEMES"), ";", -1);

      for (guint i = 0; entries[i]; i++)
        {
          g_auto (GStrv) halves = g_strsplit (entries[i], "=", 2);
          guint flags = 0;
          struct { const char *name; guint bit; } bits[] = {
            { "local", CHROMIUM_SCHEME_LOCAL },
            { "display_isolated", CHROMIUM_SCHEME_DISPLAY_ISOLATED },
            { "secure", CHROMIUM_SCHEME_SECURE },
            { "cors", CHROMIUM_SCHEME_CORS },
            { "fetch", CHROMIUM_SCHEME_FETCH },
          };

          if (!halves[0] || !*halves[0])
            continue;
          if (halves[1])
            {
              g_auto (GStrv) words = g_strsplit (halves[1], ",", -1);
              for (guint w = 0; words[w]; w++)
                for (gsize b = 0; b < G_N_ELEMENTS (bits); b++)
                  if (g_strcmp0 (words[w], bits[b].name) == 0)
                    flags |= bits[b].bit;
            }
          chromium_session_scheme_register (demo.session, halves[0], flags);
          g_printerr ("demo: scheme registered %s flags=%u\n", halves[0],
                      flags);
        }
    }

  /* Scan and enable before start(): the .so files load against the model
     alone, and the helper that starts below is told the enabled set at
     ::ready — the same rule the devtools switch follows. There is something
     to scan when --plugin-dir named a directory, or when a data directory
     anchors the install root the user may have installed into. */
  if (demo.plugin_directory || demo.data_directory)
    demo_setup_plugins ();

  if (!chromium_session_start (demo.session, initial_url (), &error))
    {
      /* A run that was asked for a persistent profile must not fall back to a
       * session-less window: say why and exit, so a script sees the failure.
       * Every other start failure keeps the old behaviour — a warning and a
       * demo with no page. */
      if (demo.data_directory)
        {
          g_printerr ("demo: %s\n", error->message);
          g_clear_error (&error);
          g_clear_object (&demo.session);
          demo.exit_status = 1;
          g_application_quit (G_APPLICATION (demo.app));
          return;
        }

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
    /* Printed whether or not there is a client surface, so they start at zero:
     * a dump line whose numbers are whatever was on the stack is worse than one
     * that says the compositor had nothing to show. */
    graphene_rect_t src, dst;

    graphene_rect_init (&src, 0, 0, 0, 0);
    graphene_rect_init (&dst, 0, 0, 0, 0);

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
   * next handover is where the previous one is retired. Two drags cannot be in
   * flight at once: replacing demo_out_drag while the first is still alive
   * would also strand the first one's temp file, because its dnd-finished
   * handler no longer matches the global. Say no to the second one. */
  if (demo_out_drag)
    {
      g_print ("demo: drag out: a previous drag is still in flight, not carrying "
               "this one\n");
      return;
    }

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
 * "js <code>", "js-expr <expression>", "a11y-on", "a11y-off", "frames",
 * "devtools-on", "devtools-off", "new-tab <url>", "new-window <url>",
 * "select-browser <id>",
 * "close-browser <id>", "wait <ms>", "hide", "show", "view-source",
 * "shot <path>", "dump <path>", "hover <x> <y>",
 * "click <x> <y> [button]", "drag <x0> <y0> <x1> <y1> [button]",
 * "drag-hold <x0> <y0> <x1> <y1> [button]", "release [button]", "surfaces",
 * "wheel <dx> <dy>", "mute", "unmute", "zoom-set <level>",
 * "auto-resize <on|off> [min-w min-h max-w max-h]", "download-accept <path>",
 * "surface-size <browser-id>", "surface-sample <browser-id> [fx fy]",
 * "cookies-get <url>", "cookies-set <url> <name> <value>",
 * "cookies-delete <url> [name]", "history", "page-text", "print-pdf [max-bytes]",
 * "download-reject", "find <pattern>", "find-case <pattern>", "find-next",
 * "find-prev", "find-stop", "menu-pick <id>", "menu-dismiss",
 * "perm-accept <id>", "perm-deny <id>", "perm-dismiss <id>",
 * "js-dialog-respond <id> <ok|cancel> [value]",
 * "certificate-respond <id> <allow|deny>", "certificate-authorize <fp>",
 * "certificate-clear", "hang-respond <wait|terminate>",
 * "intercept-add <id> <pattern>[;<pattern>…]", "intercept-remove <id>",
 * "intercept-mode <passive|blocking>",
 * "request-respond <id> <allow|cancel|redirect> [url]",
 * "cdp-enable [browser]", "cdp-disable [browser]",
 * "cdp-execute <id> <method> [json-params]",
 * "load-browser <id> <url>", "back-browser <id>", "forward-browser <id>",
 * "reload-browser <id>", "zoom-browser <id> <action>", "new-window-geom <x> <y> <w> <h> [url]",
 * "browsers", "history-go <index>", "helper-version",
 * "auth-respond <user> <pass>", "auth-cancel", "client-cert-respond <id> <index>",
 * "tls-info", "screenshot", "scheme-register <name> [flags]",
 * "scheme-response <status> <content-type>", "scheme-body <b64>", "scheme-end",
 * "script-add <id> <run_at> <pattern> <code-b64>",
 * "script-add-for <id> <browser> <run_at> <pattern> <code-b64>", "script-remove <id>",
 * "ua-set <value>", "ua-clear", "clear-cookies", "clear-cache", "clear-auth-cache",
 * "clear-origin <origin>",
 * "popup-mode <window|popover>", "plugin-action <id>",
 * "file-dialog-respond <id> [path[;path…]]",
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
 * gets measured at all. */
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

/* The paired surface of one browser, as a script can see it: the size the
 * nested compositor configured, and one pixel of the client buffer. The pair
 * comes from ::browser-surface, which is what the surface-pair gate grades —
 * a pixel of this surface answering for that browser id is the whole claim. */
static void
script_surface_probe (const char *args, gboolean sample)
{
  int id = 0;
  double fx = 0.5, fy = 0.5;
  NestedSurface *surface;
  int size = sample ? sscanf (args, "%d %lf %lf", &id, &fx, &fy)
                    : sscanf (args, "%d", &id);

  if (size < 1)
    {
      g_warning ("demo: surface-%s wants a browser id", sample ? "sample" : "size");
      return;
    }
  surface = demo.session ? chromium_session_get_browser_surface (demo.session, id)
                         : NULL;
  if (!surface)
    {
      g_printerr ("demo: surface-%s browser=%d none\n",
                  sample ? "sample" : "size", id);
      return;
    }

  if (!sample)
    {
      int w = 0, h = 0;

      nested_surface_get_size (surface, &w, &h);
      g_printerr ("demo: surface-size browser=%d size=%dx%d\n", id, w, h);
      return;
    }

  {
    GdkTexture *texture = nested_surface_get_texture (surface);

    if (!texture)
      {
        g_printerr ("demo: surface-sample browser=%d rgba=none\n", id);
        return;
      }
    int w = gdk_texture_get_width (texture);
    int h = gdk_texture_get_height (texture);
    g_autofree guchar *data = g_malloc ((gsize) w * 4 * h);
    int x, y;
    guchar *p;
    guchar r, g, b, a;

    gdk_texture_download (texture, data, (gsize) w * 4);
    x = CLAMP ((int) (w * fx), 0, w - 1);
    y = CLAMP ((int) (h * fy), 0, h - 1);
    p = data + ((gsize) y * w + x) * 4;

    /* gdk_texture_download() always provides GDK_MEMORY_DEFAULT — cairo
     * ARGB32, i.e. B,G,R,A in memory on little-endian — whatever the
     * texture's own format was (gdkmemorytexture.h says so). The line says
     * "rgba", so the gate gets the colour the page asked for. */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    b = p[0]; g = p[1]; r = p[2]; a = p[3];
#else
    a = p[0]; r = p[1]; g = p[2]; b = p[3];
#endif
    g_printerr ("demo: surface-sample browser=%d size=%dx%d rgba=%u,%u,%u,%u\n",
                id, w, h, r, g, b, a);
  }
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

  /* A restart step stops the helper on purpose; the ::closed it produces is
     expected, and the steps after it are for the new helper. */
  if (demo.restart_pending)
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

  /* Report now, quit later. On SIGTERM/SIGHUP the helper announces ::closed
   * from inside its shutdown and then stays alive through CefShutdown(), still
   * drawing into this process's nested compositor. Quitting here destroys that
   * compositor and the helper's GDK ends the process with "Lost connection to
   * Wayland compositor" before the tail of main() — the same bug with a better
   * log. ::helper-exited is the first moment there is no process left to need
   * the display. */
  if (!demo.helper_exited)
    {
      demo.quit_when_helper_exits = TRUE;
      return G_SOURCE_REMOVE;
    }

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
                            !strncmp (line, "js-expr ", 8) ||
                            !strcmp (line, "back") || !strcmp (line, "forward") ||
                            !strcmp (line, "reload") || !strcmp (line, "reload-hard") ||
                            !strcmp (line, "stop") || !strcmp (line, "mute") ||
                            !strcmp (line, "unmute") ||
                            !strncmp (line, "zoom-", 5) ||
                            !strncmp (line, "auto-resize", 11) ||
                            !strncmp (line, "surface-", 8) ||
                            !strncmp (line, "cookies-", 8) ||
                            !strcmp (line, "history") ||
                            !strcmp (line, "page-text") ||
                            !strncmp (line, "print-pdf", 9) ||
                            !strncmp (line, "download-", 9) ||
                            !strncmp (line, "find", 4) ||
                            !strncmp (line, "menu-", 5) ||
                            !strncmp (line, "perm-", 5) ||
                            !strncmp (line, "js-dialog-", 10) ||
                            !strncmp (line, "file-dialog-", 12) ||
                            !strncmp (line, "a11y-", 5) ||
                            !strncmp (line, "devtools-", 9) ||
                            !strncmp (line, "plugin-", 7) ||
                            !strcmp (line, "restart") ||
                            !strcmp (line, "clear-data") ||
                            !strncmp (line, "new-", 4) ||
                            !strncmp (line, "select-browser", 14) ||
                            !strncmp (line, "close-browser", 13) ||
                            !strncmp (line, "load-browser", 12) ||
                            !strncmp (line, "back-browser", 12) ||
                            !strncmp (line, "forward-browser", 15) ||
                            !strncmp (line, "reload-browser", 14) ||
                            !strncmp (line, "zoom-browser", 12) ||
                            !strncmp (line, "scheme-", 7) ||
                            !strncmp (line, "script-", 7) ||
                            !strncmp (line, "auth-", 5) ||
                            !strncmp (line, "client-cert-", 12) ||
                            !strcmp (line, "tls-info") ||
                            !strcmp (line, "screenshot") ||
                            !strcmp (line, "browsers") ||
                            !strncmp (line, "history-go", 10) ||
                            !strcmp (line, "helper-version") ||
                            !strncmp (line, "ua-set", 6) ||
                            !strncmp (line, "clear-", 6)))
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
          chromium_session_load_uri (demo.session, 0, line + 5);
          continue;
        }
      else if (!strcmp (line, "frames"))
        {
          g_printerr ("demo: frames %" G_GUINT64_FORMAT "\n", demo_frame_count ());
          continue;
        }
      else if (!strcmp (line, "a11y-on"))
        {
          chromium_session_set_accessibility (demo.session, TRUE);
          continue;
        }
      else if (!strcmp (line, "a11y-off"))
        {
          chromium_session_set_accessibility (demo.session, FALSE);
          continue;
        }
      else if (!strcmp (line, "devtools-on"))
        {
          chromium_session_set_devtools_enabled (demo.session, TRUE);
          continue;
        }
      else if (!strcmp (line, "devtools-off"))
        {
          chromium_session_set_devtools_enabled (demo.session, FALSE);
          continue;
        }
      else if (!strncmp (line, "plugin-enable ", 14) ||
               !strncmp (line, "plugin-disable ", 15))
        {
          ChromiumPluginManager *manager = demo.session ?
            chromium_session_get_plugin_manager (demo.session) : NULL;
          const char *id = line + (line[7] == 'e' ? 14 : 15);
          GError *error = NULL;

          if (!manager)
            g_printerr ("demo: %s without a session manager\n", line);
          else if (line[7] == 'e')
            {
              if (!chromium_plugin_manager_enable (manager, id, &error))
                g_printerr ("demo: plugin-enable %s failed: %s\n", id,
                            error ? error->message : "unknown");
              g_clear_error (&error);
            }
          else
            {
              chromium_plugin_manager_disable (manager, id);
              g_printerr ("demo: plugin %s disabled\n", id);
            }
          continue;
        }
      else if (!strncmp (line, "popup-mode ", 11))
        {
          /* Which host form a ::plugin-action takes: the standalone window
           * (搭法一, the default) or a popover under the plugin's button
           * (搭法二). A script can drive both; a real host picks one. */
          const char *mode = line + 11;

          demo.popup_in_popover = g_strcmp0 (mode, "popover") == 0;
          g_printerr ("demo: popup mode %s\n",
                      demo.popup_in_popover ? "popover" : "window");
          continue;
        }
      else if (!strncmp (line, "plugin-action ", 14))
        {
          /* The button click without the button: the same manager entry the
           * action button lands on, for scripts that cannot point at a
           * header bar. */
          ChromiumPluginManager *manager = demo.session ?
            chromium_session_get_plugin_manager (demo.session) : NULL;

          if (!manager)
            g_printerr ("demo: plugin-action without a session manager\n");
          else
            chromium_plugin_manager_activate_action (manager, line + 14);
          continue;
        }
      else if (!strcmp (line, "restart"))
        {
          g_printerr ("demo: restarting the helper\n");
          demo.restart_pending = TRUE;
          chromium_session_stop (demo.session);
          g_timeout_add (300, demo_restart_start, NULL);
          return;
        }
      else if (!strcmp (line, "clear-data"))
        {
          g_printerr ("demo: stopping the helper for clear-data\n");
          demo.restart_pending = TRUE;
          chromium_session_stop (demo.session);
          g_timeout_add (200, demo_clear_data_try, NULL);
          return;
        }
      else if (!strcmp (line, "hide"))
        {
          /* Take the window off the screen, the way a user hiding it would:
           * test/frame-stop-gate.sh asserts what the page's rAF does about it. */
          if (demo.window)
            gtk_widget_set_visible (GTK_WIDGET (demo.window), FALSE);
          continue;
        }
      else if (!strcmp (line, "show"))
        {
          if (demo.window)
            {
              gtk_widget_set_visible (GTK_WIDGET (demo.window), TRUE);
              gtk_window_present (demo.window);
            }
          continue;
        }
      else if (!strncmp (line, "new-tab ", 8) ||
               !strncmp (line, "new-window ", 11))
        {
          gboolean windowed = !strncmp (line, "new-window ", 11);
          const char *url = line + (windowed ? 11 : 8);
          chromium_session_new_browser (demo.session, url, windowed);
          continue;
        }
      else if (!strncmp (line, "select-browser ", 15))
        {
          chromium_session_select_browser (demo.session, atoi (line + 15));
          continue;
        }
      else if (!strncmp (line, "close-browser ", 14))
        {
          chromium_session_close_browser (demo.session, atoi (line + 14));
          continue;
        }
      else if (!strncmp (line, "load-browser ", 13) ||
               !strncmp (line, "back-browser ", 13) ||
               !strncmp (line, "forward-browser ", 16) ||
               !strncmp (line, "reload-browser ", 15) ||
               !strncmp (line, "zoom-browser ", 13))
        {
          /* The A.1 shape: navigation aimed at a background tab. */
          g_auto (GStrv) parts = g_strsplit (line, " ", 3);
          int id = parts[1] ? atoi (parts[1]) : 0;

          if (g_str_has_prefix (line, "load-browser "))
            chromium_session_load_uri (demo.session, id,
                                       parts[2] ? parts[2] : "about:blank");
          else if (g_str_has_prefix (line, "back-browser "))
            chromium_session_go_back (demo.session, id);
          else if (g_str_has_prefix (line, "forward-browser "))
            chromium_session_go_forward (demo.session, id);
          else if (g_str_has_prefix (line, "reload-browser "))
            chromium_session_reload (demo.session, id, FALSE);
          else if (parts[2])
            chromium_session_zoom (demo.session, id, parts[2]);
          continue;
        }
      else if (!strncmp (line, "new-window-geom ", 16))
        {
          /* new-window-geom <x> <y> <w> <h> [url] — the window.open features
             a host's own policy would translate into this call. */
          g_auto (GStrv) parts = g_strsplit (line + 16, " ", 5);
          int x = parts[0] ? atoi (parts[0]) : 0;
          int y = parts[1] ? atoi (parts[1]) : 0;
          int w = parts[2] ? atoi (parts[2]) : 0;
          int h = parts[3] ? atoi (parts[3]) : 0;

          chromium_session_new_browser_with_geometry
            (demo.session, parts[4] ? parts[4] : "about:blank", TRUE,
             x, y, w, h);
          continue;
        }
      else if (!strcmp (line, "browsers"))
        {
          /* The cache as it stands right now; ::browsers-changed logs the
             pushes as they land. */
          session_browsers_changed (demo.session, NULL);
          continue;
        }
      else if (!strncmp (line, "history-go ", 11))
        {
          chromium_session_history_go (demo.session, 0, atoi (line + 11));
          continue;
        }
      else if (!strcmp (line, "helper-version"))
        {
          g_printerr ("demo: helper-version %d\n",
                      chromium_session_get_helper_version (demo.session));
          continue;
        }
      else if (!strncmp (line, "auth-respond ", 13))
        {
          g_auto (GStrv) parts = g_strsplit (line + 13, " ", 2);

          if (demo_auth_id == 0)
            g_warning ("demo: no auth request is being held");
          else
            chromium_session_auth_respond (demo.session, demo_auth_id,
                                           parts[0] ? parts[0] : "",
                                           parts[1] ? parts[1] : "");
          continue;
        }
      else if (!strcmp (line, "auth-cancel"))
        {
          if (demo_auth_id == 0)
            g_warning ("demo: no auth request is being held");
          else
            chromium_session_auth_cancel (demo.session, demo_auth_id);
          continue;
        }
      else if (!strncmp (line, "client-cert-respond ", 20))
        {
          g_auto (GStrv) parts = g_strsplit (line + 20, " ", 2);

          chromium_session_client_cert_respond (demo.session,
                                                parts[0] ? atoi (parts[0]) : 0,
                                                parts[1] ? atoi (parts[1]) : -1);
          continue;
        }
      else if (!strcmp (line, "tls-info"))
        {
          chromium_session_get_tls_info (demo.session,
                                         (int) session_script.index, 0);
          continue;
        }
      else if (!strncmp (line, "scheme-register ", 16))
        {
          /* scheme-register <name> [flags] — flags are the flag words
             comma-joined (local,display_isolated,secure,cors,fetch). */
          g_auto (GStrv) parts = g_strsplit (line + 16, " ", 2);
          guint flags = 0;
          struct { const char *name; guint bit; } bits[] = {
            { "local", CHROMIUM_SCHEME_LOCAL },
            { "display_isolated", CHROMIUM_SCHEME_DISPLAY_ISOLATED },
            { "secure", CHROMIUM_SCHEME_SECURE },
            { "cors", CHROMIUM_SCHEME_CORS },
            { "fetch", CHROMIUM_SCHEME_FETCH },
          };

          if (parts[1])
            {
              g_auto (GStrv) words = g_strsplit (parts[1], ",", -1);
              for (guint w = 0; words[w]; w++)
                for (gsize b = 0; b < G_N_ELEMENTS (bits); b++)
                  if (g_strcmp0 (words[w], bits[b].name) == 0)
                    flags |= bits[b].bit;
            }
          chromium_session_scheme_register (demo.session,
                                            parts[0] ? parts[0] : "", flags);
          g_printerr ("demo: scheme-register %s flags=%u\n",
                      parts[0] ? parts[0] : "", flags);
          continue;
        }
      else if (!strncmp (line, "scheme-response ", 16))
        {
          /* scheme-response <status> <content-type> */
          g_auto (GStrv) parts = g_strsplit (line + 16, " ", 2);

          if (demo_scheme_id == 0)
            g_warning ("demo: no scheme request is being served");
          else
            chromium_session_scheme_response
              (demo.session, demo_scheme_id,
               parts[0] ? atoi (parts[0]) : 200, "OK",
               parts[1] && *parts[1] ? parts[1] : "text/html", NULL);
          continue;
        }
      else if (!strncmp (line, "scheme-body ", 12))
        {
          /* The body rides base64, the same way script-add's code does: a
             page is exactly the thing with spaces in it. */
          gsize length = 0;
          g_autofree guchar *body = g_base64_decode (line + 12, &length);
          GBytes *bytes = g_bytes_new_take (g_steal_pointer (&body), length);

          if (demo_scheme_id == 0)
            g_warning ("demo: no scheme request is being served");
          else
            chromium_session_scheme_write (demo.session, demo_scheme_id,
                                           bytes);
          g_bytes_unref (bytes);
          continue;
        }
      else if (!strcmp (line, "scheme-end"))
        {
          if (demo_scheme_id != 0)
            {
              chromium_session_scheme_end (demo.session, demo_scheme_id);
              demo_scheme_id = 0;
            }
          else
            g_warning ("demo: no scheme request is being served");
          continue;
        }
      else if (!strncmp (line, "script-add-for ", 15) ||
               !strncmp (line, "script-add ", 11))
        {
          /* script-add <id> <run_at> <pattern> <code-b64>, or the per-browser
             form script-add-for <id> <browser> <run_at> <pattern> <code-b64>
             — the code rides base64 because a script is exactly the thing
             with spaces in it. */
          gboolean scoped = line[10] == '-';
          g_auto (GStrv) parts = g_strsplit (line + (scoped ? 15 : 11), " ", 5);
          gsize length = 0;
          g_autofree guchar *code = NULL;
          int browser = 0;
          const char *run_at;
          const char *pattern;
          const char *encoded;

          if (g_strv_length (parts) < (scoped ? 5u : 4u))
            {
              g_warning ("demo: script-add wants <id> <run_at> <pattern> "
                         "<code-b64>");
              continue;
            }
          if (scoped)
            {
              browser = atoi (parts[1]);
              run_at = parts[2];
              pattern = parts[3];
              encoded = parts[4];
            }
          else
            {
              run_at = parts[1];
              pattern = parts[2];
              encoded = parts[3];
            }
          code = g_base64_decode (encoded, &length);
          {
            g_autofree char *code_text = g_strndup ((gchar *) code, length);
            const char *patterns[] = { pattern, NULL };

            chromium_session_script_add (demo.session,
                                         (guint) atoi (parts[0]), browser,
                                         patterns, code_text, run_at, FALSE);
          }
          g_printerr ("demo: script-add id=%s browser=%d run_at=%s bytes=%zu\n",
                      parts[0], browser, run_at, length);
          continue;
        }
      else if (!strncmp (line, "script-remove ", 14))
        {
          chromium_session_script_remove (demo.session,
                                          (guint) atoi (line + 14));
          continue;
        }
      else if (!strncmp (line, "ua-set ", 7))
        {
          chromium_session_set_user_agent (demo.session, line + 7);
          g_printerr ("demo: ua-set %s\n", line + 7);
          continue;
        }
      else if (!strncmp (line, "ua-clear", 8))
        {
          chromium_session_set_user_agent (demo.session, "");
          g_printerr ("demo: ua-set (cleared)\n");
          continue;
        }
      else if (!strcmp (line, "screenshot"))
        {
          chromium_session_capture_screenshot (demo.session,
                                               (int) session_script.index, 0);
          continue;
        }
      else if (!strcmp (line, "clear-cookies") || !strcmp (line, "clear-cache") ||
               !strcmp (line, "clear-auth-cache"))
        {
          guint types = !strcmp (line, "clear-cookies")
                          ? CHROMIUM_CLEAR_COOKIES
                          : !strcmp (line, "clear-cache")
                              ? CHROMIUM_CLEAR_CACHE
                              : CHROMIUM_CLEAR_HTTP_AUTH_CACHE;

          chromium_session_clear_data_types (demo.session, types, NULL);
          g_printerr ("demo: clear-data-types %s\n", line);
          continue;
        }
      else if (!strncmp (line, "clear-origin ", 13))
        {
          const char *origins[] = { line + 13, NULL };

          chromium_session_clear_data_types
            (demo.session, CHROMIUM_CLEAR_ORIGIN_STORAGE, origins);
          g_printerr ("demo: clear-data-types origin %s\n", line + 13);
          continue;
        }
      else if (!strncmp (line, "js-expr ", 8))
        {
          chromium_session_run_javascript_expression (demo.session, line + 8,
                                                      session_script.index);
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
      else if (!strncmp (line, "cookies-get ", 12))
        {
          chromium_session_cookies_get (demo.session,
                                        (int) session_script.index, line + 12);
          continue;
        }
      else if (!strncmp (line, "cookies-set ", 12))
        {
          /* cookies-set <url> <name> <value> — one cookie is all the demo's
           * script needs; the component API takes the array form. */
          g_auto (GStrv) parts = g_strsplit (line + 12, " ", 3);
          g_autoptr (JsonBuilder) builder = json_builder_new ();
          g_autoptr (JsonNode) root = NULL;
          g_autoptr (JsonGenerator) generator = json_generator_new ();
          g_autofree char *json = NULL;
          gsize length = 0;

          if (g_strv_length (parts) < 3)
            {
              g_warning ("demo: cookies-set wants <url> <name> <value>");
              continue;
            }
          json_builder_begin_array (builder);
          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "name");
          json_builder_add_string_value (builder, parts[1]);
          json_builder_set_member_name (builder, "value");
          json_builder_add_string_value (builder, parts[2]);
          json_builder_end_object (builder);
          json_builder_end_array (builder);
          root = json_builder_get_root (builder);
          json_generator_set_root (generator, root);
          json = json_generator_to_data (generator, &length);
          chromium_session_cookies_set (demo.session, (int) session_script.index,
                                        parts[0], json);
          continue;
        }
      else if (!strncmp (line, "cookies-delete ", 15))
        {
          /* cookies-delete <url> [name] */
          g_auto (GStrv) parts = g_strsplit (line + 15, " ", 2);

          if (g_strv_length (parts) < 1 || !*parts[0])
            {
              g_warning ("demo: cookies-delete wants <url> [name]");
              continue;
            }
          chromium_session_cookies_delete (
            demo.session, (int) session_script.index, parts[0],
            g_strv_length (parts) > 1 ? parts[1] : NULL);
          continue;
        }
      else if (!strcmp (line, "history"))
        {
          chromium_session_history (demo.session, 0, (int) session_script.index);
          continue;
        }
      else if (!strcmp (line, "page-text"))
        {
          chromium_session_page_text (demo.session, 0, (int) session_script.index);
          continue;
        }
      else if (!strncmp (line, "print-pdf", 9))
        {
          /* print-pdf [max-bytes] — a cap is how a run asks for a stream cut
           * short on purpose. */
          int max_bytes = line[9] ? atoi (line + 10) : 0;

          if (max_bytes > 0)
            {
              ChromiumPdfSettings settings = { 0 };

              settings.max_bytes = max_bytes;
              chromium_session_print_pdf_with_settings (
                demo.session, 0, (int) session_script.index, &settings);
            }
          else
            chromium_session_print_pdf (demo.session, 0,
                                        (int) session_script.index);
          continue;
        }
      else if (!strcmp (line, "back"))
        {
          chromium_session_go_back (demo.session, 0);
          continue;
        }      else if (!strcmp (line, "forward"))
        {
          chromium_session_go_forward (demo.session, 0);
          continue;
        }
      else if (!strcmp (line, "reload"))
        {
          chromium_session_reload (demo.session, 0, FALSE);
          continue;
        }
      else if (!strcmp (line, "reload-hard"))
        {
          chromium_session_reload (demo.session, 0, TRUE);
          continue;
        }
      else if (!strcmp (line, "stop"))
        {
          chromium_session_stop_loading (demo.session, 0);
          continue;
        }
      else if (!strcmp (line, "zoom-in"))
        {
          chromium_session_zoom (demo.session, 0, "in");
          continue;
        }
      else if (!strcmp (line, "zoom-out"))
        {
          chromium_session_zoom (demo.session, 0, "out");
          continue;
        }
      else if (!strcmp (line, "zoom-reset"))
        {
          chromium_session_zoom (demo.session, 0, "reset");
          continue;
        }
      else if (!strncmp (line, "zoom-set ", 9))
        {
          double level = 0;
          char extra = 0;

          if (1 != sscanf (line + 9, "%lf %c", &level, &extra))
            {
              g_warning ("demo: zoom-set wants one level, got '%s'", line);
              continue;
            }
          chromium_session_set_zoom_level (demo.session, 0, level);
          continue;
        }
      else if (!strncmp (line, "auto-resize ", 12))
        {
          const char *args = line + 12;
          char verdict[8] = "";
          int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
          int used = 0;
          gboolean enabled;

          if (sscanf (args, "%7s%n", verdict, &used) < 1)
            {
              g_warning ("demo: auto-resize wants on|off, got '%s'", line);
              continue;
            }
          if (!strcmp (verdict, "on"))
            enabled = TRUE;
          else if (!strcmp (verdict, "off"))
            enabled = FALSE;
          else
            {
              g_warning ("demo: auto-resize wants on|off, got '%s'", verdict);
              continue;
            }
          if (args[used] &&
              4 == sscanf (args + used, "%d %d %d %d", &min_w, &min_h, &max_w,
                           &max_h))
            chromium_session_set_auto_resize_bounds (demo.session, min_w, min_h,
                                                     max_w, max_h);
          chromium_session_set_auto_resize (demo.session, enabled);
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
      else if (!strncmp (line, "download-accept-name ", 21))
        {
          /* Two downloads alive at once: the first-row steps cannot say which
           * one they mean, and a gate knows the file it asked for, not
           * Chromium's id. First word is a substring of the suggested name or
           * the URL, the rest is the path. */
          const char *rest = line + 21;
          const char *space = strchr (rest, ' ');
          const char *path;
          DownloadRow *row;

          if (!space || space == rest || !space[1])
            {
              g_warning ("demo: download-accept-name wants '<substring> <path>',"
                         " got '%s'", rest);
              continue;
            }
          path = space + 1;
          g_autofree char *substring = g_strndup (rest, space - rest);
          row = download_find_row (substring, row_undecided);
          if (!row)
            {
              g_warning ("demo: no undecided download matches '%s'", substring);
              continue;
            }
          download_accept (row, path);
          continue;
        }
      else if (!strncmp (line, "download-reject-name ", 21))
        {
          const char *substring = line + 21;
          DownloadRow *row;

          if (!*substring)
            {
              g_warning ("demo: download-reject-name wants a substring");
              continue;
            }
          row = download_find_row (substring, row_open);
          if (!row)
            {
              g_warning ("demo: no open download matches '%s'", substring);
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
      else if (!strcmp (line, "view-source"))
        {
          /* The same request the context menu's 查看网页源代码 makes; the
           * answer is the ::view-source line below, which is what the source
           * gate grades. */
          chromium_session_view_source (demo.session, 0);
          g_printerr ("demo: view-source requested\n");
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
      else if (!strncmp (line, "js-dialog-respond ", 18))
        {
          /* The id from the "demo: js-dialog" line, standing in for the button
           * a finger would press. The rest of the line is the prompt's answer
           * verbatim — spaces and all — because a page may ask for anything. */
          int id = 0;
          char verdict[8] = "";
          int used = 0;
          JsDialogRow *row;
          const char *value;

          if (sscanf (line + 18, "%d %7s%n", &id, verdict, &used) < 2)
            {
              g_warning ("demo: js-dialog-respond wants '<id> ok|cancel [value]',"
                         " got '%s'", line);
              continue;
            }
          value = line + 18 + used;
          while (*value == ' ')
            value++;
          if (strcmp (verdict, "ok") && strcmp (verdict, "cancel"))
            {
              g_warning ("demo: js-dialog-respond wants ok or cancel, got '%s'",
                         verdict);
              continue;
            }
          row = js_dialog_row_for (id);
          if (!row || row->decided)
            {
              g_warning ("demo: no js dialog %d is waiting for an answer", id);
              continue;
            }
          js_dialog_answer (row, !strcmp (verdict, "ok"), value);
          continue;
        }
      else if (!strncmp (line, "certificate-respond ", 20))
        {
          /* The id from the "demo: certificate-error" line; the verdict is
             the whole decision the host would have made. */
          int id = 0;
          char verdict[8] = "";

          if (sscanf (line + 20, "%d %7s", &id, verdict) != 2 ||
              (strcmp (verdict, "allow") && strcmp (verdict, "deny")))
            {
              g_warning ("demo: certificate-respond wants '<id> allow|deny',"
                         " got '%s'", line);
              continue;
            }
          chromium_session_certificate_respond (demo.session, id,
                                                !strcmp (verdict, "allow"));
          g_printerr ("demo: certificate %d %s\n", id, verdict);
          continue;
        }
      else if (!strncmp (line, "certificate-authorize ", 22))
        {
          chromium_session_certificate_authorize (demo.session, line + 22);
          g_printerr ("demo: certificate authorize %.16s...\n", line + 22);
          continue;
        }
      else if (!strcmp (line, "certificate-clear"))
        {
          chromium_session_certificate_clear_authorized (demo.session);
          g_printerr ("demo: certificate clear\n");
          continue;
        }
      else if (!strncmp (line, "hang-respond ", 13))
        {
          /* hang-respond <wait|terminate> — answers the ::render-unresponsive
           * the demo last saw. The id bookkeeping is the demo's convenience;
           * the component API is id-addressed like every parked pair. */
          const char *action = line + 13;

          if (strcmp (action, "wait") && strcmp (action, "terminate"))
            {
              g_warning ("demo: hang-respond wants 'wait|terminate', got '%s'",
                         line);
              continue;
            }
          if (!demo.pending_hang_id)
            {
              g_warning ("demo: hang-respond with no pending hang");
              continue;
            }
          chromium_session_render_unresponsive_respond (
              demo.session, demo.pending_hang_id,
              !strcmp (action, "terminate") ? CHROMIUM_HANG_TERMINATE
                                            : CHROMIUM_HANG_WAIT);
          g_printerr ("demo: hang-respond id=%d %s\n", demo.pending_hang_id,
                      action);
          demo.pending_hang_id = 0;
          continue;
        }
      else if (!strncmp (line, "intercept-add ", 14))
        {
          /* intercept-add <id> <pattern>[;<pattern>…] — the host's own id,
           * the patterns verbatim from the script line. */
          int id = 0;
          const char *patterns;
          char *copy;
          const char *patterns_argv[64] = {0};
          int n = 0;
          char *p;

          if (sscanf (line + 14, "%d", &id) != 1 || line[14] == '\0')
            {
              g_warning ("demo: intercept-add wants '<id> <patterns>', got"
                         " '%s'", line);
              continue;
            }
          patterns = line + 14;
          while (*patterns && *patterns != ' ')
            patterns++;   /* the id the sscanf already read */
          while (*patterns == ' ')
            patterns++;
          copy = g_strdup (patterns);
          patterns_argv[n++] = copy;
          for (p = copy; *p && n < 63; p++)
            if (*p == ';')
              {
                *p = '\0';
                patterns_argv[n++] = p + 1;
              }
          chromium_session_intercept_add (demo.session, (guint) id,
                                          patterns_argv, NULL);
          g_free (copy);
          g_printerr ("demo: intercept-add %d (%d patterns)\n", id, n);
          continue;
        }
      else if (!strncmp (line, "intercept-remove ", 17))
        {
          chromium_session_intercept_remove (demo.session,
                                             (guint) atoi (line + 17));
          g_printerr ("demo: intercept-remove %s\n", line + 17);
          continue;
        }
      else if (!strncmp (line, "intercept-mode ", 15))
        {
          gboolean passive = !strncmp (line + 15, "passive", 7);

          chromium_session_intercept_set_passive (demo.session, passive);
          g_printerr ("demo: intercept-mode %s\n",
                      passive ? "passive" : "blocking");
          continue;
        }
      else if (!strncmp (line, "request-respond ", 16))
        {
          /* request-respond <id> allow|cancel|redirect [url] */
          int id = 0;
          char action[16] = "";
          int used = 0;
          const char *url;

          if (sscanf (line + 16, "%d %15s%n", &id, action, &used) < 2)
            {
              g_warning ("demo: request-respond wants '<id> allow|cancel|"
                         "redirect [url]', got '%s'", line);
              continue;
            }
          url = line + 16 + used;
          while (*url == ' ')
            url++;
          if (!strcmp (action, "allow"))
            chromium_session_request_respond (demo.session, id,
                                              CHROMIUM_REQUEST_ALLOW, NULL);
          else if (!strcmp (action, "cancel"))
            chromium_session_request_respond (demo.session, id,
                                              CHROMIUM_REQUEST_CANCEL, NULL);
          else if (!strcmp (action, "redirect"))
            chromium_session_request_respond (demo.session, id,
                                              CHROMIUM_REQUEST_REDIRECT,
                                              *url ? url : NULL);
          else
            {
              g_warning ("demo: request-respond wants allow, cancel or"
                         " redirect, got '%s'", action);
              continue;
            }
          g_printerr ("demo: request %d %s\n", id, action);
          continue;
        }
      else if (!strncmp (line, "cdp-enable", 10) &&
               (line[10] == '\0' || line[10] == ' '))
        {
          int id = 0;

          sscanf (line + 10, "%*s %d", &id);
          chromium_session_cdp_enable (demo.session, id);
          g_printerr ("demo: cdp-enable browser=%d\n", id);
          continue;
        }
      else if (!strncmp (line, "cdp-disable", 11) &&
               (line[11] == '\0' || line[11] == ' '))
        {
          int id = 0;

          sscanf (line + 11, "%*s %d", &id);
          chromium_session_cdp_disable (demo.session, id);
          g_printerr ("demo: cdp-disable browser=%d\n", id);
          continue;
        }
      else if (!strncmp (line, "cdp-execute ", 12))
        {
          /* cdp-execute <id> <method> [json-params] — the rest of the line
             is the params verbatim, because a script can carry JSON. */
          int id = 0;
          char method[128] = "";
          int used = 0;
          const char *params;

          if (sscanf (line + 12, "%d %127s%n", &id, method, &used) < 2)
            {
              g_warning ("demo: cdp-execute wants '<id> <method> [json]',"
                         " got '%s'", line);
              continue;
            }
          params = line + 12 + used;
          while (*params == ' ')
            params++;
          chromium_session_cdp_execute (demo.session, id, 0, method,
                                        *params ? params : "");
          g_printerr ("demo: cdp-execute id=%d method=%s\n", id, method);
          continue;
        }
      else if (!strncmp (line, "file-dialog-respond ", 20))
        {
          /* One path is the common case; a semicolon separates the several an
           * open-multiple asks for. No paths at all is the cancel. */
          int id = 0;
          int used = 0;
          const char *rest = line + 20;
          const char *paths_at;
          FileDialogRow *row;

          if (1 != sscanf (rest, "%d%n", &id, &used))
            {
              g_warning ("demo: file-dialog-respond wants '<id> [path[;path…]]',"
                         " got '%s'", line);
              continue;
            }
          paths_at = rest + used;
          while (*paths_at == ' ')
            paths_at++;
          row = file_dialog_row_for (id);
          if (!row || row->decided)
            {
              g_warning ("demo: no file dialog %d is waiting for an answer", id);
              continue;
            }
          if (!*paths_at)
            file_dialog_answer (row, NULL);
          else
            {
              g_auto (GStrv) paths = g_strsplit (paths_at, ";", -1);
              gsize i;

              /* An empty element would be a path of "", which no chooser can
               * return; the separator is the whole grammar. */
              for (i = 0; paths[i]; i++)
                if (!*paths[i])
                  break;
              if (paths[i])
                {
                  g_warning ("demo: empty path in '%s'", line);
                  continue;
                }
              file_dialog_answer (row, (const char *const *) paths);
            }
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
      else if (!strncmp (line, "surface-sample ", 15))
        {
          script_surface_probe (line + 15, TRUE);
          continue;
        }
      else if (!strncmp (line, "surface-size ", 13))
        {
          script_surface_probe (line + 13, FALSE);
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

/* ------------------------------------------------------------------ *
 * --plugin-dir: the demo's own plugin policy                         *
 *                                                                    *
 * The demo is also the plugin gate's driver, so its policy is the    *
 * simplest one that exercises the machinery: scan, enable everything *
 * that parsed, grant what enable() asks for. A real host shows the   *
 * person a prompt at ::grant-request instead — the component         *
 * deliberately has no policy of its own.                             *
 * ------------------------------------------------------------------ */

static void
demo_grant_request (ChromiumPluginManager *manager, const char *id,
                    const char *const *permissions,
                    ChromiumPluginPermissionRequest *request, gpointer data)
{
  GString *names = g_string_new (NULL);

  for (guint i = 0; permissions && permissions[i]; i++)
    {
      if (names->len)
        g_string_append (names, ", ");
      g_string_append (names, permissions[i]);
    }
  g_printerr ("demo: granting [%s] to plugin %s\n",
              names->len ? names->str : "(none)", id);
  g_string_free (names, TRUE);

  /* The demo's policy is the synchronous one: it grants everything the
     manifest declared, and accept() both fills the table and settles the
     enable that asked. A host with a dialog would keep the request and answer
     it later. */
  chromium_plugin_permission_request_accept (request);
}

static void
demo_plugin_error (ChromiumPluginManager *manager, const char *id,
                   const char *message, gpointer data)
{
  g_printerr ("demo: plugin error %s: %s\n", id, message);
}

static void
demo_setup_plugins (void)
{
  ChromiumPluginManager *manager;
  GListModel *model;
  GError *error = NULL;
  guint n;

  manager = chromium_session_get_plugin_manager (demo.session);
  g_signal_connect (manager, "plugin-error",
                    G_CALLBACK (demo_plugin_error), NULL);
  g_signal_connect (manager, "grant-request",
                    G_CALLBACK (demo_grant_request), NULL);

  /* --plugin-dir names a directory to scan; the install root the session
     anchored on the data directory is already in the manager. Either may
     hold plugins, and neither is required. */
  if (demo.plugin_directory &&
      !chromium_plugin_manager_add_search_path (manager,
                                                demo.plugin_directory,
                                                &error))
    {
      g_printerr ("demo: --plugin-dir %s: %s\n", demo.plugin_directory,
                  error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  /* A path that cannot be read is not the end of the scan: the per-plugin
     refusals land in the model either way, and the demo prints them below. */
  if (!chromium_plugin_manager_scan (manager, &error))
    {
      g_printerr ("demo: plugin scan: %s\n",
                  error ? error->message : "unknown");
      g_clear_error (&error);
    }

  model = chromium_plugin_manager_get_plugins (manager);
  n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      ChromiumPlugin *plugin = g_list_model_get_item (model, i);
      ChromiumPluginState state = chromium_plugin_get_state (plugin);
      const char *id = chromium_plugin_get_id (plugin);
      GEnumClass *states = g_type_class_ref (CHROMIUM_TYPE_PLUGIN_STATE);
      GEnumValue *value = g_enum_get_value (states, state);

      g_printerr ("demo: plugin %s name=%s kind=%s state=%s%s%s\n", id,
                  chromium_plugin_get_name (plugin),
                  chromium_plugin_get_kind (plugin) ?
                    chromium_plugin_get_kind (plugin) : "-",
                  value ? value->value_nick : "?",
                  state == CHROMIUM_PLUGIN_STATE_REFUSED ? " error=" : "",
                  state == CHROMIUM_PLUGIN_STATE_REFUSED ?
                    chromium_plugin_get_error_message (plugin) : "");
      g_type_class_unref (states);

      if (state == CHROMIUM_PLUGIN_STATE_SCANNED)
        {
          if (chromium_plugin_manager_enable (manager, id, &error))
            g_printerr ("demo: plugin %s enabled\n", id);
          else
            g_printerr ("demo: plugin %s not enabled: %s\n", id,
                        error ? error->message : "unknown");
          g_clear_error (&error);
        }

      /* One action button per enabled plugin, the way §2.9 sketches it. The
         click is the manager's activate_action: ::plugin-action for the
         host's own popup policy, chrome.action.onClicked for the plugin. */
      if (chromium_plugin_get_enabled (plugin) && demo.header &&
          !g_hash_table_contains (demo.plugin_buttons, id))
        {
          /* ::plugin-action lives on the manager; connect here, once, on the
             manager the session owns. */
          g_signal_connect (manager, "plugin-action",
                            G_CALLBACK (session_plugin_action), NULL);

          GtkWidget *action = gtk_button_new_with_label (
            chromium_plugin_get_name (plugin));

          gtk_widget_set_tooltip_text (action, id);
          g_object_set_data_full (G_OBJECT (action), "plugin-id",
                                  g_strdup (id), g_free);
          g_signal_connect (action, "clicked",
                            G_CALLBACK (plugin_action_clicked), NULL);
          gtk_header_bar_pack_end (GTK_HEADER_BAR (demo.header), action);
          g_hash_table_insert (demo.plugin_buttons, g_strdup (id),
                               g_object_ref (action));
        }
      g_object_unref (plugin);
    }
}

static void
plugin_action_clicked (GtkButton *button, gpointer data)
{
  ChromiumPluginManager *manager = demo.session ?
    chromium_session_get_plugin_manager (demo.session) : NULL;
  const char *id = g_object_get_data (G_OBJECT (button), "plugin-id");

  if (!manager || !id)
    return;
  g_printerr ("demo: plugin action pressed %s\n", id);
  chromium_plugin_manager_activate_action (manager, id);
}

/* ::plugin-action: 搭法一 — the popup opens in a window of its own, the same
   route devtools takes, and the demo's existing windowed-browser claiming
   gives it a surface. Nothing here sizes or anchors it: that is the part the
   plan leaves to the host, and the demo host does the simple thing.
   搭法二 — the "popup-mode popover" step switches the same action to a
   GtkPopover under the plugin's own button: the browser is created just the
   same (its CefWindow still makes the toplevel the FIFO pairs), but the
   surface is claimed by a view inside the popover, sized to the manifest's
   clamped popup box, and its frames are suspended the moment the popover
   closes. The plan's mechanism acceptance, not a library promise. */

/* A one-child container that is exactly the size it was built with. The
   popover sizes itself to its child's preferred size, and ChromiumView
   prefers 960x960 — so without this the popup page would configure its
   surface to 960x960 and the clamped popup box would exist only in the
   demo's intentions. */
#define DEMO_TYPE_POPUP_BIN (demo_popup_bin_get_type ())
G_DECLARE_FINAL_TYPE (DemoPopupBin, demo_popup_bin, DEMO, POPUP_BIN, GtkWidget)

struct _DemoPopupBin
{
  GtkWidget parent;
  GtkWidget *child;
  int width;
  int height;
};

G_DEFINE_TYPE (DemoPopupBin, demo_popup_bin, GTK_TYPE_WIDGET)

static GtkSizeRequestMode
demo_popup_bin_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void
demo_popup_bin_measure (GtkWidget *widget, GtkOrientation orientation,
                        int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  DemoPopupBin *self = DEMO_POPUP_BIN (widget);

  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL
                            ? self->width : self->height;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void
demo_popup_bin_size_allocate (GtkWidget *widget, int width, int height,
                              int baseline)
{
  DemoPopupBin *self = DEMO_POPUP_BIN (widget);
  GdkRectangle box = { 0, 0, width, height };

  if (self->child)
    gtk_widget_size_allocate (self->child, &box, baseline);
}

static void
demo_popup_bin_dispose (GObject *object)
{
  DemoPopupBin *self = DEMO_POPUP_BIN (object);

  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (demo_popup_bin_parent_class)->dispose (object);
}

static void
demo_popup_bin_init (DemoPopupBin *self)
{
}

static void
demo_popup_bin_class_init (DemoPopupBinClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = demo_popup_bin_dispose;
  widget_class->get_request_mode = demo_popup_bin_get_request_mode;
  widget_class->measure = demo_popup_bin_measure;
  widget_class->size_allocate = demo_popup_bin_size_allocate;
}

static GtkWidget *
demo_popup_bin_new (int width, int height)
{
  DemoPopupBin *self = g_object_new (DEMO_TYPE_POPUP_BIN, NULL);

  self->width = width;
  self->height = height;
  return GTK_WIDGET (self);
}

static void
popup_popover_closed (GtkPopover *popover, gpointer data)
{
  NestedCompositor *compositor = NULL;

  if (demo.popup_surface && demo.view)
    {
      compositor = chromium_view_get_compositor (demo.view);
      if (compositor)
        nested_compositor_set_frames_suspended (compositor,
                                                demo.popup_surface, TRUE);
    }
  g_printerr ("demo: popup popover closed (browser %d)\n",
              demo.popup_browser);
  /* The popup page is transient: the popover going away takes its browser
     with it, and ::browser-closed tears the rest down. */
  if (demo.session && demo.popup_browser)
    chromium_session_close_browser (demo.session, demo.popup_browser);
}

static void
open_popup_popover (int id)
{
  NestedCompositor *compositor;
  ChromiumView *view;
  GtkWidget *popover;
  GtkWidget *anchor;

  anchor = demo.plugin_buttons ?
    g_hash_table_lookup (demo.plugin_buttons, demo.popup_pending_plugin) : NULL;
  compositor = demo.view ? chromium_view_get_compositor (demo.view) : NULL;
  if (!anchor || !compositor)
    {
      /* No button or no compositor: the window form is the working fallback
         and the demo says so. */
      g_printerr ("demo: popup popover unavailable, opening a window\n");
      open_browser_window (id, FALSE, NULL);
      return;
    }

  view = chromium_view_new_shared (compositor, NULL);
  {
    GtkWidget *bin = demo_popup_bin_new (demo.popup_width, demo.popup_height);

    gtk_widget_set_parent (GTK_WIDGET (view), bin);
    popover = gtk_popover_new ();
    gtk_popover_set_child (GTK_POPOVER (popover), bin);
  }
  gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);
  gtk_widget_set_parent (popover, anchor);
  g_signal_connect (popover, "closed", G_CALLBACK (popup_popover_closed),
                    NULL);

  demo.popup_popover = popover;
  demo.popup_browser = id;
  g_hash_table_insert (demo.browser_views, GINT_TO_POINTER (id), view);
  demo.pending_window_ids = g_list_append (demo.pending_window_ids,
                                           GINT_TO_POINTER (id));
  assign_browser_surfaces ();
  gtk_popover_popup (GTK_POPOVER (popover));
  g_printerr ("demo: popup popover browser %d (%dx%d)\n",
              id, demo.popup_width, demo.popup_height);
}

static void
session_plugin_action (ChromiumPluginManager *manager, const char *id,
                       gpointer data)
{
  g_autoptr (ChromiumPlugin) plugin =
    chromium_plugin_manager_lookup (manager, id);
  const char *popup = plugin ?
    chromium_plugin_get_popup_uri (plugin) : NULL;
  int width = 0, height = 0;

  if (!popup || !*popup)
    {
      g_printerr ("demo: plugin action %s has no popup to show\n", id);
      return;
    }
  chromium_plugin_get_popup_size (plugin, &width, &height);
  g_printerr ("demo: opening popup %s (%dx%d)\n", popup, width, height);
  if (!demo.session)
    return;
  if (!demo.popup_in_popover)
    {
      /* 搭法一: the popup is an ordinary standalone browser from here on. */
      chromium_session_new_browser (demo.session, popup, TRUE);
      return;
    }
  g_free (demo.popup_pending_plugin);
  g_free (demo.popup_pending_uri);
  demo.popup_pending_plugin = g_strdup (id);
  demo.popup_pending_uri = g_strdup (popup);
  demo.popup_width = width;
  demo.popup_height = height;
  chromium_session_new_browser (demo.session, popup, TRUE);
}

/* The clear-data step: stop() first, then retry clear_data on a timer until
   the reaped helper's flock is gone — the helper holds the directory lock
   for as long as the process lives, and the reap is not synchronous. */
static gboolean
demo_clear_data_try (gpointer data)
{
  GError *error = NULL;
  static guint attempts = 0;

  if (chromium_session_clear_data (demo.session, &error))
    {
      attempts = 0;
      g_printerr ("demo: data cleared\n");
      script_run_next ();
      return G_SOURCE_REMOVE;
    }
  if (++attempts > 25)
    {
      attempts = 0;
      g_printerr ("demo: clear-data failed: %s\n",
                  error ? error->message : "unknown");
      g_clear_error (&error);
      demo.exit_status = 1;
      script_finish ();
      g_application_quit (G_APPLICATION (demo.app));
      return G_SOURCE_REMOVE;
    }
  g_clear_error (&error);
  return G_SOURCE_CONTINUE;
}

/* The restart step: stop() synchronously, then give the reap a turn on the
   main loop before start() — start() refuses with BUSY while the previous
   child is still being waited on. */
static gboolean
demo_restart_start (gpointer data)
{
  GError *error = NULL;

  demo.restart_pending = FALSE;

  if (!chromium_session_start (demo.session, NULL, &error))
    {
      g_printerr ("demo: restart failed: %s\n",
                  error ? error->message : "unknown");
      g_clear_error (&error);
      demo.exit_status = 1;
      script_finish ();
      g_application_quit (G_APPLICATION (demo.app));
      return G_SOURCE_REMOVE;
    }
  g_printerr ("demo: session restarted\n");
  script_run_next ();
  return G_SOURCE_REMOVE;
}

/* The first command of a session script lands here rather than on a timer. A
 * cold Chromium profile takes seconds to come up, and a load sent before the
 * channel is open is simply lost — the script then waits for a page that was
 * never asked for. */
static void
session_ready (ChromiumSession *session, gpointer data)
{
  /* A helper that reports ready is a new process; the previous child's exit,
   * if there was one, says nothing about this one. */
  demo.helper_exited = FALSE;
  demo.quit_when_helper_exits = FALSE;

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

static void
primary_read_done (GObject *clipboard, GAsyncResult *result, gpointer data)
{
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard),
                                                          result, NULL);

  g_printerr ("demo: primary is '%s'\n", text ? text : "(no text)");
}

static void
primary_changed (GObject *clipboard, GParamSpec *pspec, gpointer data)
{
  gdk_clipboard_read_text_async (GDK_CLIPBOARD (clipboard), NULL,
                                 primary_read_done, NULL);
}

static void
primary_seed (const char *why)
{
  const char *seed = g_getenv ("CHROMIUM_PRIMARYSET");
  GdkClipboard *clipboard;

  if (!seed || !*seed || !demo.window)
    return;

  clipboard = gtk_widget_get_primary_clipboard (GTK_WIDGET (demo.window));
  gdk_clipboard_set_text (clipboard, seed);
  g_printerr ("demo: seeded the primary selection with '%s' (%s, window active=%d)\n",
              seed, why, gtk_window_is_active (demo.window));
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
  primary_seed ("on activation");
}

static void
setup_clipboard_test (void)
{
  const char *seed = g_getenv ("CHROMIUM_CLIPSET");
  const char *watch = g_getenv ("CHROMIUM_CLIPWATCH");
  const char *primary = g_getenv ("CHROMIUM_PRIMARYSET");
  const char *primary_watch = g_getenv ("CHROMIUM_PRIMARYWATCH");

  if (!seed && !watch && !primary && !primary_watch)
    return;

  if (seed || watch)
    {
      GdkClipboard *clipboard =
        gdk_display_get_clipboard (gtk_widget_get_display (GTK_WIDGET (demo.window)));
      g_signal_connect (clipboard, "notify::content", G_CALLBACK (clipboard_changed), NULL);
    }

  if (primary || primary_watch)
    {
      GdkClipboard *clipboard =
        gtk_widget_get_primary_clipboard (GTK_WIDGET (demo.window));
      g_signal_connect (clipboard, "notify::content",
                        G_CALLBACK (primary_changed), NULL);
    }

  if ((seed && *seed) || (primary && *primary))
    {
      g_signal_connect (demo.window, "notify::active",
                        G_CALLBACK (window_active_changed), NULL);
      if (seed && *seed)
        clipboard_seed ("at startup");
      if (primary && *primary)
        primary_seed ("at startup");
    }
}

static uint64_t
demo_frame_count (void)
{
  NestedCompositor *compositor;

  if (demo.session)
    return chromium_session_get_frame_count (demo.session);
  compositor = demo.view ? chromium_view_get_compositor (demo.view) : NULL;
  return compositor ? nested_compositor_get_frame_count (compositor) : 0;
}

static gboolean
frame_metrics_tick (gpointer data)
{
  g_printerr ("demo: frame-count %" G_GUINT64_FORMAT "\n", demo_frame_count ());
  if (demo.session)
    {
      guint64 shm_commits, dmabuf_commits, copied, wrapped;

      chromium_session_get_commit_stats (demo.session, &shm_commits,
                                         &dmabuf_commits, &copied, &wrapped);
      g_printerr ("demo: commits shm=%" G_GUINT64_FORMAT " dmabuf=%"
                  G_GUINT64_FORMAT " copied=%" G_GUINT64_FORMAT
                  " wrapped=%" G_GUINT64_FORMAT "\n",
                  shm_commits, dmabuf_commits, copied, wrapped);
    }
  return G_SOURCE_CONTINUE;
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
  /* A plain quit left the helper drawing into a compositor nobody was
   * reading — a timeout(1)-wrapped run then hung at full CPU until its
   * SIGKILL (measured: 6 h 21 min at 97%). Stopping the session first takes
   * the page down; quit still fires from the main loop, which the signal
   * source already woke. */
  if (demo.session)
    chromium_session_stop (demo.session);
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
  demo.browser_windows = g_hash_table_new (g_direct_hash, g_direct_equal);
  demo.browser_views = g_hash_table_new (g_direct_hash, g_direct_equal);

  if (chromium_view_get_compositor (demo.view))
    {
      NestedCompositor *compositor = chromium_view_get_compositor (demo.view);

      g_signal_connect (compositor, "notify::surface",
                        G_CALLBACK (on_surface_notify), NULL);
      g_signal_connect (compositor, "toplevels-changed",
                        G_CALLBACK (on_toplevels_changed), NULL);

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
  demo.tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (demo.tabs, "toolbar");
  gtk_box_append (GTK_BOX (box), demo.tabs);
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
      build_dialog_panel (GTK_BOX (box));
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
  if (g_getenv ("CHROMIUM_FRAME_METRICS"))
    g_timeout_add_seconds (1, frame_metrics_tick, NULL);
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
   * the strings: the array is released with g_strfreev(), which owns them.
   * --data-dir= and --devtools are the demo's own options, so they are taken
   * out here rather than handed to a client that would only read them as a URL. */
  args = g_ptr_array_new ();
  for (i = 1; i < argc; i++)
    {
      if (g_str_has_prefix (argv[i], "--data-dir="))
        {
          g_free (demo.data_directory);
          demo.data_directory = g_strdup (argv[i] + strlen ("--data-dir="));
          continue;
        }

      if (g_str_has_prefix (argv[i], "--plugin-dir="))
        {
          g_free (demo.plugin_directory);
          demo.plugin_directory = g_strdup (argv[i] + strlen ("--plugin-dir="));
          continue;
        }

      if (!strcmp (argv[i], "--devtools"))
        {
          demo.devtools = TRUE;
          continue;
        }

      g_ptr_array_add (args, g_strdup (argv[i]));
    }
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
  g_free (demo.data_directory);
  g_free (demo.plugin_directory);
  /* Not a fixed 0: a scripted run cut short by its helper dying sets
   * demo.exit_status, and a harness only sees the difference if it is handed out. */
  return demo.exit_status;
}
