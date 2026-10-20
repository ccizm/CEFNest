#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

  g_printerr ("demo: uri=%s title=%s loading=%d back=%d forward=%d\n",
              chromium_session_get_uri (session),
              chromium_session_get_title (session),
              chromium_session_get_loading (session),
              chromium_session_get_can_go_back (session),
              chromium_session_get_can_go_forward (session));
}

static void
session_load_end (ChromiumSession *session, const char *uri, int http_code,
                  gpointer data)
{
  g_printerr ("demo: loaded %s (HTTP %d)\n", uri, http_code);
}

static void
session_script_result (ChromiumSession *session, int id, gboolean ok,
                       const char *value, const char *message, gpointer data)
{
  g_printerr ("demo: script %d %s value=%s%s%s\n", id, ok ? "ok" : "failed",
              value, message && *message ? " message=" : "",
              message && *message ? message : "");
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
  g_signal_connect (demo.session, "script-result",
                    G_CALLBACK (session_script_result), NULL);
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
 * "click <x> <y> [button]", "close", "quit". This is how a headless run proves
 * navigation, history and repaint all work together, and "close" is how it
 * proves the way out of the window stops the helper with it. A "js" step is
 * tagged with its own line number, which is what comes back on
 * ::script-result, so a result can be matched to the step that asked for it.
 * The page steps need a helper session; "hover", "click", "shot", "dump" and
 * "close" work against any client, so the same script drives the nested
 * compositor without Chromium. */
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
                            !strcmp (line, "stop")))
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
      gdk_clipboard_set_text (clipboard, seed);
      g_printerr ("demo: seeded the clipboard with '%s'\n", seed);
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
    build_toolbar (GTK_HEADER_BAR (header));
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
                                 "application-id", "org.chromiumgtk.Demo",
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
