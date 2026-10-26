#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"
#include "nested.h"

/* Multi-window. windowed=TRUE makes the helper create a standalone CefWindow,
 * which appears as another toplevel on this view's nested compositor. A host
 * that wants each of those as a real GTK window does three things:
 *
 *   1. on ::browser-created (windowed=TRUE) open a GTK window holding a view
 *      from chromium_view_new_shared() over the first view's compositor, and
 *      queue the browser id;
 *   2. on ::toplevels-changed walk nested_compositor_get_toplevels() and pin
 *      each queued id's surface onto its view — the first toplevel is the
 *      shared tab window and stays on the primary view;
 *   3. on ::browser-closed or the GTK window's close-request, tear the pair
 *      down together (close_browser() / gtk_window_destroy()).
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-windows
 *   then press "New window".
 */
typedef struct {
  GtkWidget *window;         /* NULL when the main window is gone */
  GtkWidget *entry;
  ChromiumView *view;        /* the primary view; owns the compositor */
  NestedCompositor *compositor;
  NestedSurface *tab_surface;/* the shared tab window's toplevel */
  ChromiumSession *session;
  GHashTable *windows;       /* int id -> GtkWidget* */
  GHashTable *views;         /* int id -> ChromiumView* */
  GHashTable *assigned;      /* NestedSurface* -> int id */
  GList *pending;            /* ids waiting for their surface */
  int active_id;             /* focused browser, 0 = the main window */
  GtkWidget *menu_parent;    /* window a context menu should pop on */
  GtkWidget *popover;        /* the session menu request being answered */
  int menu_answered;
} Windows;

static Windows wins;

static void menu_teardown (void);

static void
assign_surfaces (void)
{
  GList *tops, *l, *keys, *k;

  if (!wins.compositor || !wins.assigned)
    return;

  tops = nested_compositor_get_toplevels (wins.compositor);

  /* A key has to be a surface the compositor still has: closing a window drops
   * its view, which was the last reference on the surface, and the helper's
   * next toplevel can come back at the same address. */
  keys = g_hash_table_get_keys (wins.assigned);
  for (k = keys; k; k = k->next)
    {
      NestedSurface *dead = k->data;
      ChromiumView *view;

      if (g_list_find (tops, dead))
        continue;

      if (dead == wins.tab_surface)
        {
          view = wins.view;
          g_clear_object (&wins.tab_surface);
        }
      else
        view = g_hash_table_lookup (wins.views,
                                    g_hash_table_lookup (wins.assigned, dead));
      if (view)
        chromium_view_set_target_surface (view, NULL);
      g_hash_table_remove (wins.assigned, dead);
    }
  g_list_free (keys);

  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;

      if (g_hash_table_contains (wins.assigned, surface))
        continue;

      if (!wins.tab_surface)
        {
          /* The first toplevel is the shared tab window; the primary view
           * paints it. */
          wins.tab_surface = g_object_ref (surface);
          chromium_view_set_target_surface (wins.view, surface);
          g_hash_table_insert (wins.assigned, surface, GINT_TO_POINTER (0));
          continue;
        }

      /* A window can be destroyed before its surface arrives; its id then
       * waits in the queue with no view, and must not swallow a surface. */
      while (wins.pending)
        {
          int id = GPOINTER_TO_INT (wins.pending->data);
          ChromiumView *view = g_hash_table_lookup (wins.views,
                                                    GINT_TO_POINTER (id));

          wins.pending = g_list_delete_link (wins.pending, wins.pending);
          if (view)
            {
              chromium_view_set_target_surface (view, surface);
              g_hash_table_insert (wins.assigned, surface,
                                   GINT_TO_POINTER (id));
              break;
            }
        }
    }
  g_list_free_full (tops, g_object_unref);
}

static void
debug_state (const char *when)
{
  GList *tops, *l;

  if (!g_getenv ("CEFNEST_DEBUG"))
    return;
  tops = wins.compositor ?
    nested_compositor_get_toplevels (wins.compositor) : NULL;
  g_printerr ("win: %s — %u toplevels, tab=%p in=%s\n", when,
              g_list_length (tops), (void *) wins.tab_surface,
              wins.tab_surface && g_list_find (tops, wins.tab_surface)
                ? "yes" : "NO");
  for (l = tops; l; l = l->next)
    g_printerr ("win:   top %p\n", (void *) l->data);
  g_list_free_full (tops, g_object_unref);
  if (wins.view)
    g_printerr ("win:   main view mapped=%d visible=%d %dx%d realized=%d\n",
                gtk_widget_get_mapped (GTK_WIDGET (wins.view)),
                gtk_widget_get_visible (GTK_WIDGET (wins.view)),
                gtk_widget_get_width (GTK_WIDGET (wins.view)),
                gtk_widget_get_height (GTK_WIDGET (wins.view)),
                gtk_widget_get_realized (GTK_WIDGET (wins.view)));
}

static gboolean
auto_open (gpointer data)
{
  if (wins.session && chromium_session_get_running (wins.session))
    {
      g_printerr ("win: auto-open windowed browser\n");
      chromium_session_new_browser (wins.session, "https://example.org",
                                    TRUE);
    }
  return G_SOURCE_REMOVE;
}

static gboolean
tick_probe (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  static int ticks = 0;
  GList *stack, *primary;

  ticks++;
  if (g_getenv ("CEFNEST_DEBUG") && ticks % 120 == 0)
    {
      stack = (wins.compositor && wins.tab_surface)
        ? nested_compositor_get_paint_stack_for (wins.compositor,
                                                 wins.tab_surface)
        : NULL;
      /* Both getters hand back a list with one reference per entry, so the
       * primary stack needs the same free as stack_for() — reading its length
       * inline leaked the whole stack every 120 ticks. */
      primary = wins.compositor
        ? nested_compositor_get_paint_stack (wins.compositor)
        : NULL;
      g_printerr ("win: tick %d — stack_for(tab)=%u stack(primary)=%u "
                  "view %dx%d mapped=%d parent=%p in-window=%d\n",
                  ticks, g_list_length (stack), g_list_length (primary),
                  gtk_widget_get_width (GTK_WIDGET (wins.view)),
                  gtk_widget_get_height (GTK_WIDGET (wins.view)),
                  gtk_widget_get_mapped (GTK_WIDGET (wins.view)),
                  (void *) gtk_widget_get_parent (GTK_WIDGET (wins.view)),
                  wins.window &&
                    gtk_window_get_child (GTK_WINDOW (wins.window)) ==
                      gtk_widget_get_parent (GTK_WIDGET (wins.view)));
      g_list_free_full (stack, g_object_unref);
      g_list_free_full (primary, g_object_unref);
    }
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static gboolean
forced_redraw (gpointer data)
{
  g_printerr ("win: raising the main window\n");
  if (wins.window)
    {
      gtk_window_present (GTK_WINDOW (wins.window));
      gtk_widget_queue_draw (GTK_WIDGET (wins.view));
    }
  return G_SOURCE_REMOVE;
}

static void
on_toplevels_changed (NestedCompositor *compositor, gpointer data)
{
  GHashTableIter iter;
  gpointer key, value;

  assign_surfaces ();
  debug_state ("toplevels-changed");

  /* A retargeted view shows nothing new until it is told to draw: a static
   * page never damages itself, so without this every window stays frozen on
   * the frame it first presented — the primary's. */
  if (wins.view)
    gtk_widget_queue_draw (GTK_WIDGET (wins.view));
  g_hash_table_iter_init (&iter, wins.views);
  while (g_hash_table_iter_next (&iter, &key, &value))
    gtk_widget_queue_draw (value);
}

static void
window_gone (GtkWidget *widget, gpointer data)
{
  /* "destroy" hands over (widget, user_data): a one-argument handler would
   * read the widget pointer as the id and remove nothing, leaving dangling
   * rows for ::browser-closed to trip over. */
  int id = GPOINTER_TO_INT (data);

  menu_teardown ();
  g_hash_table_remove (wins.windows, GINT_TO_POINTER (id));
  g_hash_table_remove (wins.views, GINT_TO_POINTER (id));
  wins.pending = g_list_remove (wins.pending, GINT_TO_POINTER (id));
  if (wins.active_id == id)
    wins.active_id = 0;
}

static void
main_window_gone (GtkWidget *widget, gpointer data)
{
  menu_teardown ();
  wins.window = NULL;
  if (wins.menu_parent == GTK_WIDGET (widget))
    wins.menu_parent = NULL;
}

static void
main_window_active (GObject *object, GParamSpec *pspec, gpointer data)
{
  if (gtk_window_is_active (GTK_WINDOW (object)))
    {
      wins.active_id = 0;   /* the tab window's browser is the active one */
      wins.menu_parent = GTK_WIDGET (object);
    }
}

static void
browser_window_active (GObject *object, GParamSpec *pspec, gpointer data)
{
  /* Focus is host policy for a standalone browser: clicking a window selects
   * its browser as the session's active one, and context menus pop on the
   * window the user is actually looking at. */
  if (gtk_window_is_active (GTK_WINDOW (object)) && wins.session)
    {
      wins.active_id = GPOINTER_TO_INT (data);
      wins.menu_parent = GTK_WIDGET (object);
      chromium_session_select_browser (wins.session, wins.active_id);
    }
}

static gboolean
browser_window_close (GtkWindow *window, gpointer data)
{
  /* The user closed a browser window: tell the helper, then tear the GTK side
   * down. ::browser-closed will also arrive and find the row already gone. */
  if (wins.menu_parent == GTK_WIDGET (window))
    wins.menu_parent = NULL;
  menu_teardown ();
  chromium_session_close_browser (wins.session, GPOINTER_TO_INT (data));
  gtk_window_destroy (window);
  return TRUE;
}

static void
open_browser_window (int id)
{
  ChromiumView *view;
  GtkWidget *window;

  if (!wins.compositor || !wins.window)
    return;

  window = gtk_application_window_new (g_object_get_data (
      G_OBJECT (wins.window), "gtk-app"));
  view = chromium_view_new_shared (wins.compositor, NULL);
  gtk_window_set_title (GTK_WINDOW (window), "CEFNest window");
  gtk_window_set_default_size (GTK_WINDOW (window), 900, 650);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));

  g_hash_table_insert (wins.windows, GINT_TO_POINTER (id), window);
  g_hash_table_insert (wins.views, GINT_TO_POINTER (id), view);
  wins.pending = g_list_append (wins.pending, GINT_TO_POINTER (id));
  g_signal_connect (window, "destroy", G_CALLBACK (window_gone),
                    GINT_TO_POINTER (id));
  g_signal_connect (window, "close-request",
                    G_CALLBACK (browser_window_close), GINT_TO_POINTER (id));
  g_signal_connect (window, "notify::is-active",
                    G_CALLBACK (browser_window_active), GINT_TO_POINTER (id));

  assign_surfaces ();
}

static void
session_browser_created (ChromiumSession *session, int id, const char *url,
                         gboolean windowed, gboolean devtools,
                         gboolean background, gpointer data)
{
  if (windowed)
    open_browser_window (id);
}

static void
session_browser_closed (ChromiumSession *session, int id, gpointer data)
{
  /* A windowed browser reports its close; the GTK window goes with it. Take
   * the rows out before destroying so the destroy handler's own removal is a
   * no-op instead of a second owner of a dying pointer. */
  GtkWidget *window = g_hash_table_lookup (wins.windows,
                                           GINT_TO_POINTER (id));

  if (window)
    {
      g_hash_table_remove (wins.windows, GINT_TO_POINTER (id));
      g_hash_table_remove (wins.views, GINT_TO_POINTER (id));
      wins.pending = g_list_remove (wins.pending, GINT_TO_POINTER (id));
      if (wins.active_id == id)
        wins.active_id = 0;
      menu_teardown ();
      gtk_window_destroy (GTK_WINDOW (window));
    }
}

static void
on_new_window (GtkButton *button, gpointer data)
{
  const char *typed = wins.entry ?
    gtk_editable_get_text (GTK_EDITABLE (wins.entry)) : NULL;

  chromium_session_new_browser (wins.session,
                                typed && *typed ? typed : "about:blank",
                                TRUE);
}

/* ------------------------------------------------------- the context menu --
 * The session's default menu is one popover for the whole session, parented to
 * the view set_view() was given — the main window. With several windows that
 * means a right click anywhere pops the menu in the wrong one, so a
 * multi-window host draws the menu itself (exactly like examples/menus.c) and
 * parents it to the focused window. */

static void
menu_answer (int id)
{
  if (wins.menu_answered)
    {
      if (id)
        chromium_session_context_menu_select (wins.session, id);
      else
        chromium_session_context_menu_dismiss (wins.session);
      wins.menu_answered = 0;
    }
}

static void
on_menu_item (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  menu_answer (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row),
                                                   "item-id")));
  if (wins.popover)
    gtk_popover_popdown (GTK_POPOVER (wins.popover));
}

static void
menu_teardown (void)
{
  if (wins.popover)
    {
      GtkWidget *popover = wins.popover;

      wins.popover = NULL;
      if (wins.menu_answered && wins.session &&
          chromium_session_get_running (wins.session))
        chromium_session_context_menu_dismiss (wins.session);
      wins.menu_answered = 0;
      gtk_popover_popdown (GTK_POPOVER (popover));
      gtk_widget_unparent (popover);
    }
}

static void
on_menu_closed (GtkPopover *popover, gpointer data)
{
  menu_answer (0);   /* clicked away: the request still wants its dismiss */
}

static gboolean
session_context_menu (ChromiumSession *session, int x, int y, GList *items,
                      gpointer data)
{
  /* (x, y) are the click's own coordinates in the active browser's view, so
   * the popover has to be parented to that view: pointing_to is read in the
   * parent widget's coordinate space, and a window has a toolbar above the
   * view while a standalone window does not. */
  ChromiumView *active = wins.active_id
    ? g_hash_table_lookup (wins.views, GINT_TO_POINTER (wins.active_id))
    : wins.view;
  GtkWidget *parent = GTK_WIDGET (active ? active : wins.view);
  GtkWidget *list;
  GdkRectangle rect;
  GList *l;

  if (!parent || !wins.window)
    return FALSE;   /* no UI to draw with: let the session answer */

  menu_teardown ();   /* a leftover popover from a window going away */

  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_menu_item), NULL);

  for (l = items; l; l = l->next)
    {
      ChromiumMenuItem *item = l->data;
      GtkWidget *row;
      GtkWidget *label;
      g_autofree char *text = NULL;

      if (item->type && !strcmp (item->type, "separator"))
        {
          gtk_list_box_append (GTK_LIST_BOX (list),
                               gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
          continue;
        }

      text = g_strdup_printf ("%s%s%s",
                              g_str_has_prefix (item->type ? item->type : "",
                                                "submenu") ? "  ↳ " : "",
                              item->checked ? "✓ " : "",
                              item->label && *item->label ? item->label
                                                          : "(no label)");
      label = gtk_label_new (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      row = gtk_list_box_row_new ();
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
      gtk_widget_set_sensitive (row, item->enabled);
      g_object_set_data (G_OBJECT (row), "item-id",
                         GINT_TO_POINTER (item->id));
      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }

  wins.popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (wins.popover), list);
  gtk_widget_set_parent (wins.popover, parent);
  g_signal_connect (wins.popover, "closed", G_CALLBACK (on_menu_closed),
                    NULL);

  rect.x = x;
  rect.y = y;
  rect.width = 1;
  rect.height = 1;
  gtk_popover_set_pointing_to (GTK_POPOVER (wins.popover), &rect);

  wins.menu_answered = 1;
  gtk_popover_popup (GTK_POPOVER (wins.popover));
  return TRUE;
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (wins.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *new_window;
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  wins.window = gtk_application_window_new (app);
  g_object_set_data (G_OBJECT (wins.window), "gtk-app", app);
  gtk_window_set_title (GTK_WINDOW (wins.window), "CEFNest windows");
  gtk_window_set_default_size (GTK_WINDOW (wins.window), 1000, 700);

  new_window = gtk_button_new_with_label ("New window");
  g_signal_connect (new_window, "clicked", G_CALLBACK (on_new_window), NULL);
  gtk_box_append (GTK_BOX (toolbar), new_window);

  wins.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (wins.entry),
                                  "address for the next window");
  gtk_widget_set_hexpand (wins.entry, TRUE);
  gtk_box_append (GTK_BOX (toolbar), wins.entry);
  gtk_widget_set_margin_start (toolbar, 6);
  gtk_widget_set_margin_end (toolbar, 6);
  gtk_widget_set_margin_top (toolbar, 3);
  gtk_box_append (GTK_BOX (root), toolbar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (wins.window), root);

  wins.session = chromium_session_new (NULL);
  chromium_session_set_view (wins.session, view);
  wins.view = view;   /* the assign step retargets this view; NULL would
                         make every later set_target_surface a no-op */
  wins.compositor = chromium_view_get_compositor (view);

  g_signal_connect (wins.compositor, "toplevels-changed",
                    G_CALLBACK (on_toplevels_changed), NULL);
  g_signal_connect (wins.session, "browser-created",
                    G_CALLBACK (session_browser_created), NULL);
  g_signal_connect (wins.session, "browser-closed",
                    G_CALLBACK (session_browser_closed), NULL);
  g_signal_connect (wins.session, "context-menu",
                    G_CALLBACK (session_context_menu), NULL);
  g_signal_connect (wins.window, "close-request", G_CALLBACK (on_close), NULL);
  g_signal_connect (wins.window, "destroy",
                    G_CALLBACK (main_window_gone), NULL);
  g_signal_connect (wins.window, "notify::is-active",
                    G_CALLBACK (main_window_active), NULL);

  gtk_window_present (GTK_WINDOW (wins.window));

  wins.windows = g_hash_table_new (g_direct_hash, g_direct_equal);
  wins.views = g_hash_table_new (g_direct_hash, g_direct_equal);
  wins.assigned = g_hash_table_new (g_direct_hash, g_direct_equal);

  if (!chromium_session_start (wins.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  if (g_getenv ("CEFNEST_AUTOWIN"))
    {
      g_timeout_add_seconds (5, auto_open, NULL);
      gtk_widget_add_tick_callback (GTK_WIDGET (view), tick_probe, NULL, NULL);
      g_timeout_add_seconds (9, forced_redraw, NULL);
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (wins.window), "session",
                          g_object_ref (wins.session), g_object_unref);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  if (wins.session)
    chromium_session_stop (wins.session);
  g_clear_pointer (&wins.windows, g_hash_table_unref);
  g_clear_pointer (&wins.views, g_hash_table_unref);
  g_clear_pointer (&wins.assigned, g_hash_table_unref);
  g_clear_list (&wins.pending, NULL);
  g_clear_object (&wins.tab_surface);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.windows",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
