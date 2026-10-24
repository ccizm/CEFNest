/* focus-chain.c — GTK widgets and page elements in one Tab order.
 *
 * The window is [GTK button] [GTK button] [ChromiumView] [GTK button], and
 * the page inside holds two buttons of its own. The program drives the GTK
 * side the way the Tab key does (the window's move-focus) and the page the
 * way a keypress would (chromium_view_advance_page_focus), asserting every
 * crossing of the boundary in both directions:
 *
 *   gtk1 → gtk2 → page:a → page:b → gtk3          (forward; entry focuses
 *                                                  the page's first element)
 *   gtk3 → view → page:a → gtk2                   (backward; entry is the
 *                                                  page's own restoration,
 *                                                  then the walk continues)
 *   gtk2 → gtk3                                   (a page with nothing
 *                                                  focusable is skipped)
 *
 * The page reports its focused element through document.title, so every step
 * has evidence from both sides of the boundary. Prints one line per
 * assertion and exits non-zero on the first mismatch.
 */

#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

static const char PAGE_WITH_BUTTONS[] =
  "data:text/html,<html><body><button id='a'>one</button> "
  "<button id='b'>two</button><script>"
  "document.title='[none]';"
  "document.addEventListener('focusin',function(){"
  "document.title='['+(document.activeElement.id||'none')+']'});"
  "document.addEventListener('focusout',function(){"
  "document.title='[none]'});"
  "</script></body></html>";

static const char PAGE_WITH_NOTHING[] = "data:text/html,<p>nothing here</p>";

static GtkWidget *window, *gtk1, *gtk2, *gtk3;
static ChromiumView *view;
static ChromiumSession *session;
static int fails;

static const char *
focus_name (void)
{
  GtkWidget *focus = gtk_root_get_focus (GTK_ROOT (window));

  if (focus == gtk1) return "gtk1";
  if (focus == gtk2) return "gtk2";
  if (focus == gtk3) return "gtk3";
  if (focus == GTK_WIDGET (view)) return "view";
  return focus ? "other-widget" : "nothing";
}

static const char *
page_focus (void)
{
  const char *title = chromium_session_get_title (session);

  return title ? title : "";
}

static void
check (const char *step, const char *want, const char *got)
{
  if (!strcmp (want, got))
    printf ("  ok   %-34s %s\n", step, got);
  else
    {
      printf ("  FAIL %-34s wanted %s, got %s\n", step, want, got);
      fails++;
    }
  fflush (stdout);
}

/* ------------------------------------------------------------------ *
 * The sequencer                                                       *
 *                                                                      *
 * Every crossing of the boundary is a round trip through the helper,  *
 * so the walk runs as a chain of "act, then wait for the evidence,    *
 * then act again" steps on the main loop.                              *
 * ------------------------------------------------------------------ */

typedef struct {
  gboolean (*condition) (void);
  void (*action) (void);
  GTimer *timer;
} Waiter;

static void
quit_with_verdict (void)
{
  printf (fails ? "focus-chain: FAILED (%d)\n" : "focus-chain: all ok\n", fails);
  fflush (stdout);
  gtk_window_destroy (GTK_WINDOW (window));
}

static gboolean
run_action (gpointer data)
{
  void (*action) (void) = data;

  action ();
  return G_SOURCE_REMOVE;
}

static gboolean
poll_waiter (gpointer data)
{
  Waiter *waiter = data;

  if (waiter->condition ())
    {
      void (*action) (void) = waiter->action;

      g_timer_destroy (waiter->timer);
      g_free (waiter);
      if (action)
        g_idle_add (run_action, action);
      return G_SOURCE_REMOVE;
    }
  if (g_timer_elapsed (waiter->timer, NULL) > 10.0)
    {
      printf ("  FAIL step reached the 10s timeout "
              "(focus=%s page=%s)\n", focus_name (), page_focus ());
      fails++;
      fflush (stdout);
      g_timer_destroy (waiter->timer);
      g_free (waiter);
      quit_with_verdict ();
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

/* Runs @action once @condition holds — now, on a later poll, or never (the
 * run fails on timeout instead). NULL @action only waits. */
static void
wait_then (gboolean (*condition) (void), void (*action) (void))
{
  Waiter *waiter = g_new (Waiter, 1);

  waiter->condition = condition;
  waiter->action = action;
  waiter->timer = g_timer_new ();
  g_timeout_add (50, poll_waiter, waiter);
}

/* Conditions. */

static gboolean focus_is_gtk1 (void) { return !strcmp (focus_name (), "gtk1"); }
static gboolean focus_is_gtk2 (void) { return !strcmp (focus_name (), "gtk2"); }
static gboolean focus_is_gtk3 (void) { return !strcmp (focus_name (), "gtk3"); }
static gboolean focus_is_view (void) { return !strcmp (focus_name (), "view"); }

static gboolean page_has_a (void) { return !strcmp (page_focus (), "[a]"); }
static gboolean page_has_b (void) { return !strcmp (page_focus (), "[b]"); }
static gboolean page_has_none (void) { return !strcmp (page_focus (), "[none]"); }

static gboolean
loading_done (void)
{
  return !chromium_session_get_loading (session);
}

/* The page's script sets document.title the moment it runs, so a non-empty
 * title is "the page exists and its focusin listener is armed". */
static gboolean
page_alive (void)
{
  const char *title = chromium_session_get_title (session);

  return title && *title;
}

static void
move_focus (GtkDirectionType direction)
{
  g_signal_emit_by_name (window, "move-focus", direction);
}

/* --- forward walk: gtk1 → gtk2 → view(a) → (b) → gtk3 ------------------- */

static void step_tab_forward_2 (void);
static void step_tab_forward_3 (void);
static void step_tab_forward_4 (void);
static void step_tab_forward_5 (void);
static void step_tab_forward_6 (void);
static void step_tab_backward_2 (void);
static void step_empty_page_2 (void);

static void
step_tab_forward (void)
{
  check ("walk starts at gtk1", "gtk1", focus_name ());
  move_focus (GTK_DIR_TAB_FORWARD);
  wait_then (focus_is_gtk2, step_tab_forward_2);
}

static void
step_tab_forward_2 (void)
{
  check ("tab from gtk1", "gtk2", focus_name ());
  move_focus (GTK_DIR_TAB_FORWARD);
  wait_then (focus_is_view, step_tab_forward_3);
}

static void
step_tab_forward_3 (void)
{
  check ("tab from gtk2 enters the view", "view", focus_name ());
  /* The entry asked the page for one advance: its first element. */
  wait_then (page_has_a, step_tab_forward_4);
}

static void
step_tab_forward_4 (void)
{
  check ("page focused its first element", "[a]", page_focus ());
  chromium_session_run_javascript_expression (session,
      "document.getElementById('b').focus()", 0);
  wait_then (page_has_b, step_tab_forward_5);
}

static void
step_tab_forward_5 (void)
{
  check ("page focused its last element", "[b]", page_focus ());
  /* A Tab's worth of the page's traversal, off the edge: the walk comes back
   * to GTK, to the widget after the view. */
  chromium_view_advance_page_focus (view, TRUE);
  wait_then (focus_is_gtk3, step_tab_forward_6);
}

static void step_tab_forward_6 (void);

static void
step_tab_forward_6 (void)
{
  check ("tab past the page's edge", "gtk3", focus_name ());
  /* Backward entry synthesizes no key: the page's own focus restoration is
   * the entry — the remembered element is #b (focused last before the exit
   * crossing), exactly what a browser refocuses a window back to. */
  move_focus (GTK_DIR_TAB_BACKWARD);
  wait_then (focus_is_view, step_tab_backward_2);
}

/* --- backward walk: gtk3 → view(b) → (a) → gtk2 ------------------------- */

static void step_tab_backward_2 (void);
static void step_tab_backward_3 (void);
static void step_tab_backward_4 (void);
static void step_tab_backward_5 (void);

static void
step_tab_backward_2 (void)
{
  check ("shift-tab from gtk3 enters the view", "view", focus_name ());
  /* Backward entry restores whatever the page remembers — which element
   * that is belongs to Blink's own memory rules, not to the chain — so the
   * walk picks its own starting point from here. */
  step_tab_backward_3 ();
}

static void
step_tab_backward_3 (void)
{
  chromium_session_run_javascript_expression (session,
      "document.getElementById('a').focus()", 0);
  wait_then (page_has_a, step_tab_backward_4);
}

static void
step_tab_backward_4 (void)
{
  check ("page focused its first element", "[a]", page_focus ());
  chromium_view_advance_page_focus (view, FALSE);
  wait_then (focus_is_gtk2, step_tab_backward_5);
}

static void
step_tab_backward_5 (void)
{
  check ("shift-tab past the page's edge", "gtk2", focus_name ());
  chromium_session_load_uri (session, PAGE_WITH_NOTHING);
  wait_then (loading_done, step_empty_page_2);
}

/* --- a page with nothing focusable is skipped ---------------------------- */

static void step_empty_page_2 (void);
static void step_empty_page_3 (void);
static void step_empty_page_4 (void);

static void
step_empty_page_2 (void)
{
  /* Enter the chain from the widget before the view and watch the walk come
   * out the far side: the entry advance finds nothing focusable, the page
   * answers take-focus right back, and the guard declines the return so the
   * walk runs on to gtk3 instead of looping. */
  gtk_widget_grab_focus (gtk2);
  wait_then (focus_is_gtk2, step_empty_page_3);
}

static void
step_empty_page_3 (void)
{
  move_focus (GTK_DIR_TAB_FORWARD);
  wait_then (focus_is_gtk3, step_empty_page_4);
}

static void
step_empty_page_4 (void)
{
  check ("empty page skipped by the chain", "gtk3", focus_name ());
  quit_with_verdict ();
}

/* --- setup ---------------------------------------------------------------- */

static gboolean
on_close (GtkWindow *win, gpointer data)
{
  chromium_session_stop (CHROMIUM_SESSION (data));
  return FALSE;
}

static void
start_walk (void)
{
  gtk_widget_grab_focus (gtk1);
  wait_then (focus_is_gtk1, step_tab_forward);
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *box;
  GError *error = NULL;

  window = gtk_application_window_new (app);
  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  session = chromium_session_new (NULL);

  gtk1 = gtk_button_new_with_label ("GTK 1");
  gtk2 = gtk_button_new_with_label ("GTK 2");
  gtk3 = gtk_button_new_with_label ("GTK 3");
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (box), gtk1);
  gtk_box_append (GTK_BOX (box), gtk2);
  gtk_widget_set_hexpand (GTK_WIDGET (view), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (view));
  gtk_box_append (GTK_BOX (box), gtk3);

  gtk_window_set_title (GTK_WINDOW (window), "CEFNest focus chain");
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
  gtk_window_set_child (GTK_WINDOW (window), box);
  gtk_window_present (GTK_WINDOW (window));

  chromium_session_set_view (session, view);
  g_object_set_data_full (G_OBJECT (window), "session", session, g_object_unref);
  g_signal_connect_swapped (window, "close-request", G_CALLBACK (on_close), session);

  if (!chromium_session_start (session, PAGE_WITH_BUTTONS, &error))
    {
      g_warning ("cannot start the helper: %s",
                 error ? error->message : "no helper");
      g_clear_error (&error);
      gtk_window_destroy (GTK_WINDOW (window));
      return;
    }

  printf ("== focus chain: gtk1 gtk2 [page:a page:b] gtk3\n");
  fflush (stdout);
  /* Start from the first GTK button once the page is up, so the entry
   * advance has a page to land on. */
  /* Start from the first GTK button once the page is up — and its script
   * has run, which is what sets the title: loading_done alone can be true
   * before the helper even starts (loading starts FALSE), and walking in
   * that early loses the entry advance to a page that does not exist yet. */
  wait_then (page_alive, start_walk);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.focus-chain",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return fails ? 1 : status;
}
