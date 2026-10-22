/* Hides and shows the embedding widget and counts what each showing leaves on it.
 *
 * A window is hidden and shown again all the time — a tab switch, a minimise, a
 * workspace change, a close into the tray — and every slot its `map` handler armed
 * on the way in is still armed on the way out, plus one more. GLib keeps a separate
 * marshalling slot per connection and GTK a separate controller per add, so five
 * showings mean five answers to one right click and one file dropped runs the whole
 * drag-in sequence five times. Nothing logs that: the first handler takes the event
 * and the rest fire behind it on a widget that is done with it.
 *
 * This is the only place the pile-up can be counted rather than argued: GLib is
 * asked how many handlers it would block for one signal of one object, so every
 * number below is what the object actually holds. It also counts the shape that is
 * allowed — the handler armed per showing and taken down per showing — and says
 * which one it saw. The static half, test/audit-map-arms.py, catches what a program
 * cannot see: a GtkEventController has no public list to count, so controllers can
 * only be audited where they are armed.
 *
 * Needs a display; without one it exits 77, which meson reads as SKIPPED. */
#define G_LOG_DOMAIN "map-cycle"

#include <string.h>

#include <gtk/gtk.h>

#include "chromium-view.h"

#define CYCLES 5u
#define SHARED_CYCLES 3u

static int failures;
static guint map_count;
static guint unmap_count;

static void
on_view_map (GtkWidget *widget, gpointer data)
{
  map_count++;
}

static void
on_view_unmap (GtkWidget *widget, gpointer data)
{
  unmap_count++;
}

static void
expect (const char *what, guint got, guint want)
{
  if (got == want)
    {
      g_print ("ok: %s is %u\n", what, got);
      return;
    }

  g_printerr ("map-cycle: %s is %u, expected %u\n", what, got, want);
  failures++;
}

/* How many slots GLib holds for one signal of one object. There is no
 * "find all handlers" call, but block_matched() answers with the number it
 * blocked, so the count is taken by blocking them and putting them back in the
 * same breath — no loop iteration can run in between, so nothing can fire. */
static guint
slots (gpointer object, const char *signal_name)
{
  GType type = G_TYPE_FROM_INSTANCE (object);
  g_autofree char *base = NULL;
  const char *detail_name = strstr (signal_name, "::");
  GSignalMatchType mask = G_SIGNAL_MATCH_ID;
  guint signal_id;
  guint detail = 0;
  guint count;

  if (detail_name)
    {
      base = g_strndup (signal_name, (gsize) (detail_name - signal_name));
      signal_name = base;
      detail = g_quark_from_string (detail_name + 2);
      mask |= G_SIGNAL_MATCH_DETAIL;
    }

  signal_id = g_signal_lookup (signal_name, type);
  if (signal_id == 0)
    {
      g_printerr ("map-cycle: %s is not a signal of %s, so this run is asking the "
                  "wrong object\n", signal_name, g_type_name (type));
      exit (2);
    }

  count = g_signal_handlers_block_matched (object, mask, signal_id, detail,
                                           NULL, NULL, NULL);
  g_signal_handlers_unblock_matched (object, mask, signal_id, detail,
                                     NULL, NULL, NULL);

  return count;
}

static void
pump (guint ms)
{
  gint64 until = g_get_monotonic_time () + (gint64) ms * 1000;

  /* Never a blocking iteration: a window that is just sitting there produces no
   * event at all, and a wait for one would be a wait for nothing. */
  while (g_get_monotonic_time () < until)
    {
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);
      g_usleep (2000);
    }
}

/* Pull the view out of the window and put it back: that is the hide-and-show the
 * widget sees, with no window manager in the loop to decide whether a hidden
 * toplevel really unmaps. The two peaks are what the cycles left behind, not what
 * the last one did — a pile-up only shows in the highest number. */
static void
hide_and_show (GtkWidget *window, GtkWidget *view, guint times,
               guint *mapped_peak, guint *unmapped_peak)
{
  const char *signal = "notify::fullscreened";

  for (guint i = 0; i < times; i++)
    {
      g_object_ref (view);
      gtk_window_set_child (GTK_WINDOW (window), NULL);
      pump (120);
      *unmapped_peak = MAX (*unmapped_peak, slots (window, signal));
      gtk_window_set_child (GTK_WINDOW (window), view);
      pump (120);
      *mapped_peak = MAX (*mapped_peak, slots (window, signal));
      g_object_unref (view);
    }
}

int
main (void)
{
  GtkWidget *window;
  GtkWidget *view;
  GtkWidget *shared_window;
  ChromiumView *shared;
  NestedCompositor *compositor;
  guint mapped_peak = 0;
  guint unmapped_peak = 0;
  guint shared_peak = 0;
  guint shared_off_peak = 0;

  if (!gtk_init_check ())
    {
      g_print ("  skip map-cycle: needs a display\n");
      return 77;
    }

  window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 480, 320);
  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  compositor = chromium_view_get_compositor (CHROMIUM_VIEW (view));
  if (!compositor)
    {
      g_printerr ("map-cycle: the view has no compositor, so there is nothing to "
                  "count slots on\n");
      return 1;
    }

  g_signal_connect (view, "map", G_CALLBACK (on_view_map), NULL);
  g_signal_connect (view, "unmap", G_CALLBACK (on_view_unmap), NULL);

  /* Before a single showing: what the widget armed for itself, once, in init. */
  expect ("window-fullscreen-request slots before any showing",
          slots (compositor, "window-fullscreen-request"), 1);
  expect ("frame slots before any showing", slots (compositor, "frame"), 1);

  gtk_window_set_child (GTK_WINDOW (window), view);
  gtk_window_present (GTK_WINDOW (window));
  pump (300);
  hide_and_show (window, view, CYCLES, &mapped_peak, &unmapped_peak);

  /* The cycles have to have really happened, or every number below is a number
   * about a window that was only ever shown once. */
  if (map_count != CYCLES + 1 || unmap_count != CYCLES)
    {
      g_printerr ("map-cycle: the view was mapped %u times and unmapped %u over %u "
                  "hide-and-show cycles, so this run graded nothing it claims to "
                  "have graded\n", map_count, unmap_count, CYCLES);
      return 1;
    }
  g_print ("ok: the view was hidden and shown %u times\n", CYCLES);

  /* The arm that is made per showing and taken down per showing: one while the
   * window is on screen, none while it is not. Two would mean the unmap half is
   * missing, zero that the map half never ran. */
  expect ("root notify::fullscreened slots at the top of a showing", mapped_peak, 1);
  expect ("root notify::fullscreened slots at the bottom of a hiding", unmapped_peak, 0);

  expect ("window-fullscreen-request slots after the showings",
          slots (compositor, "window-fullscreen-request"), 1);
  expect ("frame slots after the showings", slots (compositor, "frame"), 1);
  expect ("text-input-state slots after the showings",
          slots (compositor, "text-input-state"), 1);

  /* A second view over the first one's compositor is what a standalone CEF window
   * gets. It draws a surface of someone else's display, so it neither answers that
   * display's fullscreen requests nor touches the window that holds it. */
  shared = chromium_view_new_shared (compositor, NULL);
  shared_window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (shared_window), 320, 240);
  gtk_window_set_child (GTK_WINDOW (shared_window), GTK_WIDGET (shared));
  gtk_window_present (GTK_WINDOW (shared_window));
  pump (300);
  hide_and_show (shared_window, GTK_WIDGET (shared), SHARED_CYCLES,
                 &shared_peak, &shared_off_peak);

  expect ("window-fullscreen-request slots with a shared view over them",
          slots (compositor, "window-fullscreen-request"), 1);
  expect ("frame slots with the owner and a shared view",
          slots (compositor, "frame"), 2);
  expect ("notify::fullscreened slots on a shared view's own window",
          MAX (shared_peak, shared_off_peak), 0);

  /* The window owns the view from set_child() on, so destroying it is what frees
   * each one; nothing here holds a ref. */
  gtk_window_destroy (GTK_WINDOW (shared_window));
  gtk_window_destroy (GTK_WINDOW (window));
  pump (100);

  if (failures)
    {
      g_printerr ("map-cycle: %d failure(s)\n", failures);
      return 1;
    }

  g_print ("map-cycle: nothing a showing armed is left over\n");
  return 0;
}
