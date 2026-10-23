#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* In-page search. chromium_session_find() starts a search or steps through the
 * one already running — the pattern goes over every time, because the helper
 * keeps no find state. The answer is ::find-result: a request that finds
 * matches answers twice (a counting update, then the same numbers with
 * final=TRUE); a request that finds nothing answers once with count 0. A new
 * search reports active_ordinal 0; a step names which match it landed on.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-findbar
 */
typedef struct {
  GtkWidget *window;
  GtkWidget *bar;       /* the find bar revealer */
  GtkWidget *entry;
  GtkWidget *status;    /* "3 matches, #2" */
  ChromiumSession *session;
} Find;

static Find find;

static void
set_status (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;

  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  gtk_label_set_text (GTK_LABEL (find.status), text);
}

static void
session_find_result (ChromiumSession *session, int identifier, int count,
                     int active_ordinal, gboolean final, gpointer data)
{
  /* Only the final answer of a request needs to move the UI; the counting
   * update carries the same numbers with final=FALSE first. */
  if (final)
    {
      if (count == 0)
        set_status ("no matches");
      else if (active_ordinal == 0)
        set_status ("%d matches", count);
      else
        set_status ("%d matches, #%d", count, active_ordinal);
    }
}

static void
do_find (gboolean forward, gboolean next)
{
  const char *pattern = gtk_editable_get_text (GTK_EDITABLE (find.entry));

  if (!pattern || !*pattern)
    return;
  /* match_case FALSE: search case-insensitively, which is what a toolbar
   * usually means. @next FALSE starts a new search, TRUE steps the running
   * one. */
  chromium_session_find (find.session, pattern, forward, FALSE, next);
}

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  do_find (TRUE, FALSE);
}

static void
on_next (GtkButton *button, gpointer data)
{
  do_find (TRUE, TRUE);
}

static void
on_prev (GtkButton *button, gpointer data)
{
  do_find (FALSE, TRUE);
}

static void
on_close_find (GtkButton *button, gpointer data)
{
  /* Ends the search; clear_selection=TRUE also drops the highlight Chromium
   * painted on the match. stop_finding answers nothing. */
  chromium_session_stop_finding (find.session, TRUE);
  set_status ("");
  gtk_revealer_set_reveal_child (GTK_REVEALER (find.bar), FALSE);
}

/* Ctrl+F / Escape via a shortcut controller on the window. */
static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
  if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F) &&
      (state & GDK_CONTROL_MASK))
    {
      gtk_revealer_set_reveal_child (GTK_REVEALER (find.bar), TRUE);
      gtk_widget_grab_focus (find.entry);
      return TRUE;
    }
  /* Escape closes the bar — and only while the bar is open. This controller
   * runs in capture phase on the window, so answering unconditionally takes
   * Escape away from the page on every press (a page that dismisses its own
   * overlay with it never sees the key). */
  if (keyval == GDK_KEY_Escape &&
      gtk_revealer_get_reveal_child (GTK_REVEALER (find.bar)))
    {
      on_close_find (NULL, NULL);
      return TRUE;
    }
  return FALSE;
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (find.session);
  return FALSE;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *bar_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *next = gtk_button_new_from_icon_name ("go-down-symbolic");
  GtkWidget *prev = gtk_button_new_from_icon_name ("go-up-symbolic");
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkEventController *controller;
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  find.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (find.window), "CEFNest find");
  gtk_window_set_default_size (GTK_WINDOW (find.window), 1000, 700);

  find.bar = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (find.bar),
                                    GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);

  find.entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (find.entry), "find in page");
  gtk_widget_set_hexpand (find.entry, TRUE);
  g_signal_connect (find.entry, "activate", G_CALLBACK (on_entry_activate),
                    NULL);
  gtk_box_append (GTK_BOX (bar_box), find.entry);
  g_signal_connect (prev, "clicked", G_CALLBACK (on_prev), NULL);
  gtk_box_append (GTK_BOX (bar_box), prev);
  g_signal_connect (next, "clicked", G_CALLBACK (on_next), NULL);
  gtk_box_append (GTK_BOX (bar_box), next);
  find.status = gtk_label_new ("");
  gtk_box_append (GTK_BOX (bar_box), find.status);
  g_signal_connect (close, "clicked", G_CALLBACK (on_close_find), NULL);
  gtk_box_append (GTK_BOX (bar_box), close);

  gtk_widget_set_margin_start (bar_box, 6);
  gtk_widget_set_margin_end (bar_box, 6);
  gtk_widget_set_margin_top (bar_box, 3);
  gtk_widget_set_margin_bottom (bar_box, 3);
  gtk_revealer_set_child (GTK_REVEALER (find.bar), bar_box);
  gtk_box_append (GTK_BOX (root), find.bar);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (find.window), root);

  find.session = chromium_session_new (NULL);
  chromium_session_set_view (find.session, view);
  g_signal_connect (find.session, "find-result",
                    G_CALLBACK (session_find_result), NULL);
  g_signal_connect (find.window, "close-request", G_CALLBACK (on_close), NULL);

  controller = gtk_event_controller_key_new ();
  /* CAPTURE, not the default bubble: the focused widget is the view, and its
   * own key controller forwards every key to the page — a bubble-phase
   * handler on the window never sees Ctrl+F. Capturing from the toplevel
   * intercepts the chords first; non-matching keys return FALSE and reach the
   * page untouched. */
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "key-pressed", G_CALLBACK (on_key_pressed),
                    NULL);
  gtk_widget_add_controller (find.window, controller);

  gtk_window_present (GTK_WINDOW (find.window));

  if (!chromium_session_start (find.session,
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
  g_object_set_data_full (G_OBJECT (find.window), "session",
                          g_object_ref (find.session), g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.findbar",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
