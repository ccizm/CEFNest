#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* The context menu. Chromium builds the model — every item it offers, with the
 * edit state and media taken into account — and hands it to the host on
 * ::context-menu (x, y, items). A handler that draws the menu itself returns
 * TRUE and then owns the answer: exactly one context_menu_select() or
 * context_menu_dismiss() has to follow, and it has to follow promptly, because
 * the helper holds one menu request at a time — an unanswered menu costs every
 * later right click too. Returning FALSE, or connecting nothing, lets the
 * session pop its own working menu for free.
 *
 * This demo draws its own popover: a flat list where submenu entries are
 * indented, checked/radio rows get a mark, and separators stay separators.
 * Item ids are CEF's own (100 back, 117 select all, 132 view source, …) plus
 * the helper's link/image rows (26501-26507).
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-menus
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *view;       /* the ::context-menu x, y are in this widget */
  GtkWidget *popover;    /* rebuilt for every ::context-menu */
  ChromiumSession *session;
  gboolean pending;      /* the helper holds an unanswered menu request */
} Menus;

static Menus menus;

static void
answer (int id)
{
  if (menus.pending)
    {
      chromium_session_context_menu_select (menus.session, id);
      menus.pending = FALSE;
    }
}

static void
on_item_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  answer (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "item-id")));
  gtk_popover_popdown (GTK_POPOVER (menus.popover));
}

static void
on_popover_closed (GtkPopover *popover, gpointer data)
{
  /* Closed by clicking away: the request still needs its answer, and "nothing"
   * is what dismiss is for. */
  if (menus.pending)
    {
      chromium_session_context_menu_dismiss (menus.session);
      menus.pending = FALSE;
    }
}

static GtkWidget *
menu_row (const ChromiumMenuItem *item)
{
  GtkWidget *row;
  GtkWidget *label;
  g_autofree char *text = NULL;
  const char *mark = item->checked ? "✓ " : "";
  const char *indent = g_str_has_prefix (item->type, "submenu") ? "  ↳ " : "";

  text = g_strdup_printf ("%s%s%s", indent, mark,
                          item->label && *item->label ? item->label
                                                      : "(no label)");
  label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
  gtk_widget_set_sensitive (row, item->enabled);
  g_object_set_data (G_OBJECT (row), "item-id", GINT_TO_POINTER (item->id));
  return row;
}

static gboolean
session_context_menu (ChromiumSession *session, int x, int y, GList *items,
                      gpointer data)
{
  GtkWidget *list;
  GdkRectangle rect;
  GList *l;

  if (!menus.window || !menus.view)
    return FALSE;   /* no UI to draw with: let the session answer */

  /* Rebuild from scratch each time: the GList belongs to the session for the
   * duration of this emission only. */
  if (menus.popover)
    gtk_widget_unparent (menus.popover);

  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_item_activated),
                    NULL);

  for (l = items; l; l = l->next)
    {
      ChromiumMenuItem *item = l->data;

      if (!strcmp (item->type, "separator"))
        gtk_list_box_append (GTK_LIST_BOX (list), gtk_separator_new (
                               GTK_ORIENTATION_HORIZONTAL));
      else if (item->type && !strcmp (item->type, "command"))
        gtk_list_box_append (GTK_LIST_BOX (list), menu_row (item));
      else
        {
          /* check / radio / submenu / none: shown with a mark or indent, and
           * activated like a command. A real host builds nested menus. */
          GtkWidget *row = menu_row (item);

          gtk_list_box_append (GTK_LIST_BOX (list), row);
        }
    }

  menus.popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (menus.popover), list);
  /* The parent is the view, not the window: gtk_popover_set_pointing_to()
   * reads its rectangle in the parent's own coordinates, and x, y come from
   * the page. Parented to the window here, every menu would land one hint
   * label too high. */
  gtk_widget_set_parent (menus.popover, menus.view);
  g_signal_connect (menus.popover, "closed", G_CALLBACK (on_popover_closed),
                    NULL);

  /* x, y are the click's own coordinates in the page — the same logical
   * pixels the pointer is fed in. */
  rect.x = x;
  rect.y = y;
  rect.width = 1;
  rect.height = 1;
  gtk_popover_set_pointing_to (GTK_POPOVER (menus.popover), &rect);

  /* Returning TRUE is the contract: from here the helper holds one open menu
   * request, and the popover's closed handler owes it select or dismiss. */
  menus.pending = TRUE;
  gtk_popover_popup (GTK_POPOVER (menus.popover));
  return TRUE;
}

/* Two menu actions are host policy, not answers to the menu itself. */
static void
session_copy_text (ChromiumSession *session, const char *text, gpointer data)
{
  if (text && *text && menus.window)
    gdk_clipboard_set_text (
      gdk_display_get_clipboard (gtk_widget_get_display (menus.window)),
      text);
}

static void
session_open_url (ChromiumSession *session, const char *url, gpointer data)
{
  if (url && *url)
    chromium_session_new_browser (session, url, FALSE);   /* open in a tab */
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (menus.session);
  return FALSE;
}

static void
on_window_destroy (GtkWindow *window, gpointer data)
{
  menus.window = NULL;
  menus.view = NULL;
  menus.popover = NULL;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *hint;
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  menus.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (menus.window), "CEFNest menus");
  gtk_window_set_default_size (GTK_WINDOW (menus.window), 1000, 700);

  hint = gtk_label_new ("right click draws this app's own popover menu");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 6);
  gtk_widget_set_margin_top (hint, 3);
  gtk_box_append (GTK_BOX (root), hint);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (menus.window), root);
  menus.view = GTK_WIDGET (view);

  menus.session = chromium_session_new (NULL);
  chromium_session_set_view (menus.session, view);

  g_signal_connect (menus.session, "context-menu",
                    G_CALLBACK (session_context_menu), NULL);
  g_signal_connect (menus.session, "copy-text",
                    G_CALLBACK (session_copy_text), NULL);
  g_signal_connect (menus.session, "open-url",
                    G_CALLBACK (session_open_url), NULL);
  g_signal_connect (menus.window, "close-request", G_CALLBACK (on_close),
                    NULL);
  g_signal_connect (menus.window, "destroy", G_CALLBACK (on_window_destroy),
                    NULL);

  gtk_window_present (GTK_WINDOW (menus.window));

  if (!chromium_session_start (menus.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  /* Two references, two jobs: the window drops its copy when the window goes,
   * and this file's global keeps its own so a handler always answers against
   * a live session. examples/minimal.c has no global and hands its only
   * reference to the window instead. */
  g_object_set_data_full (G_OBJECT (menus.window), "session",
                          g_object_ref (menus.session), g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.menus",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
