/* Drives the nested seat without any real input device: it starts the shm test
 * client on the compositor's socket, injects a scripted pointer and keyboard
 * sequence, then writes the client's own buffer out as a PNG. What the client
 * painted in response is the evidence that the events arrived.
 *
 * The composition steps stand in for a real input method: they call the same
 * emitter the embedding widget does, so a client's preedit and commits are
 * evidence of the zwp_text_input_v3 round trip.
 *
 * With a client that opens popups (--popup), the stack steps print where the
 * compositor placed them and a second PNG is composited from the whole stack,
 * which is what the embedding widget draws.
 *
 * --drag-out hands a page's drag over to the host: the client starts a real
 * wl_data_device drag from its button press, the script takes the pointer out of
 * the window twice — once with a coordinate past the edge, once by telling the
 * seat the pointer is gone — and the run then asserts on the mime, the payload
 * and the type list that came back, and on the drag being called off.
 *
 * --drag-in is the same handover the other way round: this process is the host,
 * so it hands the compositor bytes and asserts on what comes back the other end.
 * The client is the destination, and the run checks the verdict the compositor
 * reports when the drag ends — taken after a drop it read, not taken after a
 * cancel — that a second drag cannot arrive while the first holds the pointer,
 * and that a third can arrive once the second has been given up. Whether the
 * bytes themselves came back right is only knowable by the client that reads
 * them, so it exits non-zero on a mismatch and client_exited() fails the run.
 *
 * --gtk-client finishes the run by launching this same binary again as a GTK
 * client of its own compositor. libwayland hands events over as they arrive and
 * a raw client reads what it likes; a toolkit builds state out of the same
 * events and can die on that, so only a real client can prove a sequence is
 * survivable. The raw client does check one thing of its own, and stops the run
 * if the compositor gets it wrong: see client_exited().
 *
 * --second-window is two windows of one client at once, which is what two
 * independent CEF browser windows are: one wl_client, two toplevels, and one
 * pointer and keyboard the host has to route between them. The client paints
 * whichever window holds the keyboard red and the other green, so the run can
 * read the seat's decision off the frames the compositor imported — and off the
 * focus the compositor itself records. A window closing is part of the same
 * script: the client destroys the second toplevel when the key it is asked to
 * press arrives, which only happens if the keyboard really was in it. */
#define G_LOG_DOMAIN "seat-selftest"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cairo.h>

#include "nested.h"
#include "nested-internal.h"

#define BTN_LEFT 0x110
#define KEY_A    30 /* evdev, i.e. xkb keycode 38 */
#define KEY_ESC  1  /* the one --second-window closes itself on */

typedef enum {
  STEP_SETTLE,
  STEP_POINTER_ENTER,
  STEP_POINTER_MOTION,
  STEP_KEYBOARD_ENTER,
  STEP_KEYBOARD_LEAVE,
  STEP_BUTTON_PRESS,
  STEP_BUTTON_RELEASE,
  STEP_AXIS,
  STEP_AXIS_STOP,
  STEP_KEY_DOWN,
  STEP_KEY_UP,
  STEP_TI_STATE,
  STEP_TI_PREEDIT,
  STEP_TI_COMMIT,
  STEP_TI_DELETE,
  STEP_STACK,
  STEP_DUMP,
  STEP_DUMP_TI,
  STEP_DUMP_COMPOSITE,
  STEP_GTK_CLIENT,
  STEP_DRAG_OUT,
  STEP_DRAG_OUT_BACK,
  STEP_DRAG_OUT_LEAVE,
  STEP_DRAG_OUT_CHECK,
  STEP_DRAG_IN,
  STEP_DRAG_IN_MOTION,
  STEP_DRAG_IN_SECOND,
  STEP_DRAG_IN_DROP,
  STEP_DRAG_IN_CANCEL,
  STEP_DRAG_IN_CHECK,
  STEP_SECOND_COUNT,
  STEP_SECOND_ROUTE,
  STEP_SECOND_EXPECT,
  STEP_DAMAGE_SCALE,
  STEP_DRAG_NO_SOURCE,
  N_STEP_KINDS
} StepKind;

typedef struct {
  StepKind kind;
  int a;
  int b;
  int delay;
} Step;

static const Step default_script[] = {
  { STEP_SETTLE,          0,   0, 800 }, /* let the client map and draw */
  /* The probe commits two frames of its own and then stops drawing, so this is
   * as late as the picture it leaves can still be found untouched. */
  { STEP_DAMAGE_SCALE,    0,   0, 300 },
  { STEP_STACK,           0,   0,   0 }, /* where the popups were placed */
  { STEP_DUMP_COMPOSITE,  1,   0,  60 },
  { STEP_POINTER_ENTER, 300, 200,  60 },
  { STEP_KEYBOARD_ENTER,  0,   0,  60 },
  { STEP_BUTTON_PRESS, BTN_LEFT, 1, 60 },
  /* A --drag-out run starts here: the press above is where the client's
   * wl_data_device.start_drag comes from, and this is the pointer leaving the
   * window. Out past the edge the compositor configured, then back, so that the
   * rest of the script finds the seat where every other mode leaves it. */
  { STEP_DRAG_OUT,       700, 500,  60 },
  { STEP_DRAG_OUT_BACK,  300, 200,  60 },
  { STEP_DRAG_OUT_CHECK,   1,   0, 200 }, /* the payload read has to land first */
  /* A --drag-in run starts here. The pointer is over the client's surface from
   * the enter above, so this is the host arriving with a drag of its own: it
   * moves within the surface, tries to bring a second one in while the first
   * still holds the pointer, then lets go. a and b of the check are the number
   * of verdicts wanted so far and whether the last one was a take. */
  { STEP_DRAG_IN,           0,   0, 150 },
  { STEP_DRAG_IN_MOTION,  340, 260, 150 },
  { STEP_DRAG_IN_SECOND,    0,   0, 150 },
  { STEP_DRAG_IN_DROP,      0,   0, 300 },
  { STEP_DRAG_IN_CHECK,     1,   1, 150 },
  /* The host giving its drag back, then picking one up again: a cancel that left
   * a live drag behind would refuse the third one, so this is the half of the
   * run that checks the cleanup rather than the wire. */
  { STEP_DRAG_IN,           0,   0, 150 },
  { STEP_DRAG_IN_CANCEL,    0,   0, 150 },
  { STEP_DRAG_IN_CHECK,     2,   0, 150 },
  { STEP_DRAG_IN,           0,   0, 150 },
  { STEP_DRAG_IN_DROP,      0,   0, 300 },
  { STEP_DRAG_IN_CHECK,     3,   1, 150 },
  { STEP_AXIS, WL_POINTER_AXIS_VERTICAL_SCROLL, 100, 60 },
  /* The end of the gesture is a call of its own with no distance, the way GDK
   * reports a touchpad lift. It has to stay a separate step: a stop sent in the
   * frame of the axis events it stops makes Chromium drop the delta. */
  { STEP_AXIS_STOP,     0,   0,   60 },
  { STEP_KEY_DOWN,      KEY_A,   1, 60 },
  /* GDK hands over the host's auto-repeat as another press of the same key. The
   * client has to keep seeing those, but the key must not be counted twice: the
   * leave and re-enter below are what let it say how many keys it believes are
   * physically down. */
  { STEP_KEY_DOWN,      KEY_A,   2, 60 },
  { STEP_KEY_UP,        KEY_A,   0, 60 },
  { STEP_KEYBOARD_LEAVE,  0,   0,  60 },
  { STEP_KEYBOARD_ENTER,  0,   0,  60 },
  { STEP_SETTLE,          0,   0, 200 }, /* one client redraw after the input */
  { STEP_DUMP,            0,   0,   0 },
  { STEP_TI_STATE,        0,   0, 150 }, /* the client's enable and report land */
  { STEP_TI_PREEDIT,      0,   0,  80 },
  { STEP_TI_COMMIT,       0,   0,  80 },
  { STEP_TI_DELETE,      -2,   2, 150 },
  { STEP_DUMP_TI,         0,   0,   0 },
  { STEP_KEYBOARD_LEAVE,  0,   0,  80 },
  { STEP_SETTLE,          0,   0, 120 },
  { STEP_BUTTON_RELEASE, BTN_LEFT, 0, 60 },
  /* In a --drag-no-source run the release above is the drop of a drag that owns
   * no payload, and the client turns the drag's icon into a toplevel the moment
   * the drop lands. This is where the window it made has to be on the screen. */
  { STEP_DRAG_NO_SOURCE,   0,   0, 500 },
  /* Off the menu, and a press there: xdg_popup.grab means the compositor owns
   * the dismissal, so the client should report popup_done for the menu. */
  { STEP_POINTER_MOTION,   60,  60, 80 },
  { STEP_BUTTON_PRESS, BTN_LEFT,  1, 60 },
  /* The same handover again, but asked for the way a real pointer asks: no
   * coordinate outside the window, just the seat being told the pointer is gone.
   * The two triggers have to end in the same signal, and only one of them is
   * what a widget can forward. */
  { STEP_DRAG_OUT_LEAVE,  0,   0,  60 },
  { STEP_DRAG_OUT_CHECK,  2,   0, 200 },
  { STEP_BUTTON_RELEASE, BTN_LEFT, 0, 120 },
  { STEP_STACK,           0,   0,   0 },
  { STEP_DUMP_COMPOSITE,  2,   0,   0 },
  /* Last, so a client that dies takes nothing else down with it: the run has
   * already printed its stack and written its pictures. */
  { STEP_GTK_CLIENT,      0,   0,   0 },
};

/* A --second-window run has its own script: the one above presses, drags and
 * composes against a single window, and none of that belongs to a run about
 * which of two windows the seat hands the keyboard to. */
static const Step second_script[] = {
  { STEP_SETTLE,         0,   0, 900 }, /* both windows map, configure, paint */
  { STEP_SECOND_COUNT,   2,   0,   0 },
  { STEP_POINTER_ENTER, 300, 200,  80 },
  { STEP_KEYBOARD_ENTER,  0,   0,  80 },
  /* The first window is not the compositor's primary — the second mapped last
   * and took that — so this is the override being what puts the input there. */
  { STEP_SECOND_ROUTE,    0,   0, 250 },
  { STEP_SECOND_EXPECT,   0,   0,  80 },
  /* A window closing that does not own the input must leave it alone. With the
   * first window holding it and the second as primary, a clear that ignored the
   * owner would drop input to the second one and fail the next check. */
  { STEP_SECOND_ROUTE,    3,   0, 200 },
  { STEP_SECOND_EXPECT,   0,   0,  80 },
  /* The owner hands back: input goes where it goes when nothing overrides it,
   * which is the primary — the second window. */
  { STEP_SECOND_ROUTE,    2,   0, 250 },
  { STEP_SECOND_EXPECT,   1,   0,  80 },
  /* And naming the second window directly puts it there too. */
  { STEP_SECOND_ROUTE,    1,   0, 250 },
  { STEP_SECOND_EXPECT,   1,   0,  80 },
  /* The key only reaches the window the seat has the keyboard in, and that one
   * closes itself on it: the close is driven by the routing, not beside it. */
  { STEP_KEY_DOWN,  KEY_ESC,   1,  80 },
  { STEP_KEY_UP,    KEY_ESC,   0, 300 },
  { STEP_SECOND_COUNT,   1,   0,   0 },
  { STEP_SECOND_EXPECT,   2,   0, 120 },
  { STEP_DUMP,            0,   0,   0 },
};

static const char *step_names[N_STEP_KINDS] = {
  "settle", "pointer-enter", "pointer-motion", "keyboard-enter", "keyboard-leave",
  "button-press", "button-release", "axis", "axis-stop", "key-down", "key-up",
  "text-input-state", "preedit", "commit", "delete-surrounding", "stack",
  "dump", "dump-text-input", "dump-composite", "gtk-client", "drag-out",
  "drag-out-back", "drag-out-leave", "drag-out-check", "drag-in",
  "drag-in-motion", "drag-in-second", "drag-in-drop", "drag-in-cancel",
  "drag-in-check", "second-count", "second-route", "second-expect",
  "damage-scale", "drag-no-source",
};

#define PREEDIT_TEXT "nihao"
#define COMMIT_TEXT  "你好" /* two characters, six bytes */

static NestedCompositor *compositor;
static int fs_enter_seen, fs_exit_seen, fullscreen_gate;
static GSubprocess *child;
static GSubprocess *gtk_child;
static size_t step_index;
/* One of the two scripts below, picked by the flags in main(). */
static const Step *script;
static size_t script_count;
static char *dump_path;
static char *dump_ti_path;
static char *dump_composite_path;
static gboolean gtk_client_mode;
static gboolean gtk_client_failed;
static gboolean client_failed;
static gboolean dmabuf_mode;
static gboolean run_finished;
static guint gtk_client_watchdog_id;
static gboolean drag_out_mode;
static gboolean drag_out_failed;
static int drag_out_seen;
static int fds_at_drag_out;
static gboolean drag_in_mode;
static gboolean drag_in_failed;
static int drag_in_seen;
static gboolean drag_in_taken;
static int drag_in_refused;
static char drag_in_mime[128];
static int fds_at_drag_in;
static gboolean second_mode;
static gboolean second_failed;
/* --damage-scale is graded on the pixels the compositor imported, so the only
 * state it needs is whether the run has complained yet. */
static gboolean damage_scale_mode;
static gboolean damage_scale_failed;
/* --drag-no-source is graded twice: the client says what the wire carried (an
 * enter with no offer, then a drop), and this process says whether the surface
 * that carried the drag's icon ever reaches the screen again. */
static gboolean drag_no_source_mode;
static gboolean drag_no_source_failed;
/* Whether the compositor ever held a client frame to draw: a run that ends with
 * none has shown that nothing it was sent reached the screen, and that is not a
 * pass however tidily the client left. Three modes end their own connection on
 * purpose — --refused, --pool-budget, --pool-shrink — and those judge themselves
 * on the wire instead of on a picture. */
static gboolean client_frame_seen;
static gboolean paint_exempt;
/* The two toplevels the client maps, found by app-id rather than by position:
 * which of them is the compositor's primary changes when one closes, and the
 * run has to be able to say which window it is talking about either way. */
static NestedSurface *main_surface;
static NestedSurface *second_surface;

static int  open_fds (void);
static void check_drag_out (int expected);
static void check_drag_in (int expected, int expected_taken);
static gboolean host_drag_in (gboolean can_move);

/* The compositor runs in this process and reports its dmabuf doings through
 * g_log under the "nested" domain. Counting those lines turns a silent shm
 * fallback into something the run cannot paint over: --dmabuf with no import
 * is a failed run, not a green one. */
static int dmabuf_advertised; /* 1 advertised, -1 a node without 8888 formats */
static int dmabuf_imports;
static int dmabuf_refusals;

static void
count_dmabuf_lines (const char *log_domain, GLogLevelFlags log_level,
                    const char *message, gpointer data)
{
  if (strstr (message, "probed dmabuf formats"))
    dmabuf_advertised = 1;
  else if (strstr (message, "supports no 8888 format"))
    dmabuf_advertised = -1;
  else if (strstr (message, "imported dmabuf"))
    dmabuf_imports++;
  else if (strstr (message, "cannot import a") ||
           strstr (message, "GDK is not initialized"))
    dmabuf_refusals++;

  /* A handler that matches takes the line over completely, so this has to print
     it itself or the run would lose the lines it is watching. It is registered
     without G_LOG_LEVEL_CRITICAL in its mask for the same reason: a critical in
     this domain has to fall through to the writer below, which is what fails the
     run on it. */
  g_log_default_handler (log_domain, log_level, message, data);
}

/* A g_warning is how this library answers a hostile client, so warnings are part
 * of the design and a run may produce them. A g_critical is not: it means the
 * compositor called into GLib with an argument GLib refuses to touch, and a
 * client can often arrange that — an empty selection payload reached
 * g_utf8_make_valid() as a NULL string, and the same NULL one step further down
 * was a segfault. Nothing graded that before, and the tree was not free of it.
 *
 * Every record that no domain handler has claimed passes here, which is the only
 * way to see a critical under a name this test does not know in advance: GLib for
 * a g_return_if_fail, but also Gio, Gsk or Gdk. Counting is all it does; the
 * default writer stays responsible for what the run prints. */
static int glib_criticals;

static GLogWriterOutput
count_critical_records (GLogLevelFlags log_level, const GLogField *fields,
                        gsize n_fields, gpointer user_data)
{
  const char *message = NULL;
  gsize i;

  if (log_level & G_LOG_LEVEL_CRITICAL)
    {
      for (i = 0; i < n_fields; i++)
        if (!strcmp (fields[i].key, "MESSAGE"))
          message = fields[i].value;

      glib_criticals++;
      g_printerr ("selftest: the compositor reached a GLib critical: %s\n",
                  message ? message : "(no message)");
    }

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}

/* The raw client checks the shape of what it is sent — an axis_stop may not ride
 * in the frame of the axis events it stops — and leaves non-zero when the
 * compositor got that wrong. It also leaves non-zero when the compositor hung up
 * on it, so an early exit 0 is a claim the client made itself: --refused and
 * --pool-budget say so before the first frame, --pool-shrink after the refusal it
 * was looking for, and a closed toplevel is the run ending as designed. Asking a
 * live child what its status is would be a GLib critical, so this waits instead
 * of polling. */
static void
client_exited (GObject *object, GAsyncResult *result, gpointer data)
{
  GSubprocess *client = G_SUBPROCESS (object);
  GMainLoop *loop = data;
  GError *error = NULL;
  gboolean complained = FALSE;

  /* Every run ends by killing the client, so a wait that completes after the
   * script ran out is the tidy-up rather than news. */
  if (run_finished)
    return;

  if (!g_subprocess_wait_finish (client, result, &error))
    {
      complained = TRUE;
      g_printerr ("selftest: cannot wait for the client: %s\n",
                  error ? error->message : "?");
      g_clear_error (&error);
    }
  else if (g_subprocess_get_if_signaled (client))
    {
      complained = TRUE;
      g_printerr ("selftest: the client was killed by signal %d\n",
                  g_subprocess_get_term_sig (client));
    }
  else if (g_subprocess_get_exit_status (client) != 0)
    {
      complained = TRUE;
      g_printerr ("selftest: the client exited with status %d\n",
                  g_subprocess_get_exit_status (client));
    }

  if (complained)
    {
      client_failed = TRUE;
      g_main_loop_quit (loop);
    }
}

static void
save_texture (const char *path)
{
  NestedSurface *surface = nested_compositor_get_surface (compositor);
  GdkTexture *texture = surface ? nested_surface_get_texture (surface) : NULL;

  if (!texture)
    {
      g_printerr ("selftest: no client frame to save\n");
      return;
    }

  client_frame_seen = TRUE;

  if (!gdk_texture_save_to_png (texture, path))
    g_printerr ("selftest: cannot write %s\n", path);
  else
    g_print ("selftest: wrote %s (%dx%d)\n", path,
             gdk_texture_get_width (texture),
             gdk_texture_get_height (texture));
}

/* What the embedding widget sees: the toplevel plus every popup the compositor
 * placed over it, in draw order. Printing it is the direct check of the
 * positioner rules the client handed over. */
static void
print_stack (void)
{
  GList *stack = nested_compositor_get_paint_stack (compositor);
  GList *l;
  int i = 0;

  for (l = stack; l; l = l->next, i++)
    {
      NestedSurface *surface = l->data;
      int width = 0, height = 0, x = 0, y = 0;
      const char *app_id = nested_surface_get_app_id (surface);

      nested_surface_get_size (surface, &width, &height);
      nested_surface_get_position (surface, &x, &y);

      g_print ("selftest: stack[%d] %s at %d,%d size %dx%d\n", i,
               app_id ? app_id : "(popup)", x, y, width, height);
    }

  g_print ("selftest: %d surfaces painted\n", i);

  g_list_free_full (stack, g_object_unref);
}

/* The same stack the widget would snapshot, drawn here in memory so a run
 * leaves one picture with the menus where the compositor put them. */
static void
save_composite (const char *path)
{
  GList *stack = nested_compositor_get_paint_stack (compositor);
  GList *l;
  cairo_surface_t *image;
  cairo_t *cr;
  int width = 0, height = 0;

  if (!stack)
    {
      g_printerr ("selftest: nothing painted, cannot composite\n");
      return;
    }

  /* The bottom of the stack is the toplevel, and that is the picture's size. */
  nested_surface_get_size ((NestedSurface *) stack->data, &width, &height);

  image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create (image);

  for (l = stack; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      GdkTexture *texture = nested_surface_get_texture (surface);
      graphene_rect_t src, dst;
      unsigned char *pixels;
      cairo_surface_t *layer;
      double scale_x, scale_y;
      int x = 0, y = 0, stride;

      if (!texture)
        continue;

      client_frame_seen = TRUE;

      nested_surface_get_position (surface, &x, &y);
      nested_surface_get_paint_rect (surface, &src, &dst);

      stride = gdk_texture_get_width (texture) * 4;
      pixels = g_malloc ((size_t) stride * gdk_texture_get_height (texture));
      gdk_texture_download (texture, pixels, stride);

      /* gdk_texture_download hands back B8G8R8A8, which is cairo's native
       * ARGB32 on this architecture. */
      layer = cairo_image_surface_create_for_data (pixels, CAIRO_FORMAT_ARGB32,
                                                   gdk_texture_get_width (texture),
                                                   gdk_texture_get_height (texture),
                                                   stride);
      scale_x = dst.size.width / src.size.width;
      scale_y = dst.size.height / src.size.height;

      cairo_save (cr);
      cairo_rectangle (cr, x + dst.origin.x, y + dst.origin.y,
                       dst.size.width, dst.size.height);
      cairo_clip (cr);
      cairo_translate (cr, x + dst.origin.x - src.origin.x * scale_x,
                           y + dst.origin.y - src.origin.y * scale_y);
      cairo_scale (cr, scale_x, scale_y);
      cairo_set_source_surface (cr, layer, 0, 0);
      cairo_paint (cr);
      cairo_restore (cr);

      cairo_surface_destroy (layer);
      g_free (pixels);
    }

  cairo_destroy (cr);

  if (cairo_surface_write_to_png (image, path) != CAIRO_STATUS_SUCCESS)
    g_printerr ("selftest: cannot write %s\n", path);
  else
    g_print ("selftest: wrote composite %s (%dx%d, %d surfaces)\n", path,
             width, height, g_list_length (stack));

  cairo_surface_destroy (image);
  g_list_free_full (stack, g_object_unref);
}

/* ------------------------------------------------------- --second-window */

#define MAIN_APP_ID   "nested-shm-test"
#define SECOND_APP_ID "nested-shm-test-second"

/* The two colours the client paints, and nothing else, in this mode. */
#define PAINT_RED_R    0xc0
#define PAINT_RED_G    0x40
#define PAINT_RED_B    0x40
#define PAINT_GREEN_R  0x40
#define PAINT_GREEN_G  0xc0
#define PAINT_GREEN_B  0x40

static void
second_fail (const char *format, ...)
#if defined (__GNUC__)
  __attribute__ ((format (printf, 1, 2)))
#endif
  ;

static void
second_fail (const char *format, ...)
{
  va_list args;
  char *text;

  va_start (args, format);
  text = g_strdup_vprintf (format, args);
  va_end (args);

  second_failed = TRUE;
  g_printerr ("selftest: second-window gate FAILED: %s\n", text);
  g_free (text);
}

/* The app-ids are the only stable names two toplevels of one client have: the
 * compositor's primary is whichever is at the head of its list, and that
 * changes when one of them closes. */
static const char *
surface_name (NestedSurface *surface)
{
  const char *app_id;

  if (!surface)
    return "(none)";
  if (surface == main_surface)
    return "the first window";
  if (surface == second_surface)
    return "the second window";

  app_id = nested_surface_get_app_id (surface);
  return app_id ? app_id : "(other)";
}

/* The colour at one pixel of what a surface last committed, counted in the
 * buffer's own units: the texture the compositor keeps *is* the client's buffer,
 * so a rectangle it chose to read out of the new frame shows here, and one it
 * chose to skip shows the frame before it. That is what makes a partial copy
 * something a run can be made to answer for instead of a claim about state. */
static gboolean
surface_pixel (NestedSurface *surface, int x, int y, int *r, int *g, int *b)
{
  GdkTexture *texture = nested_surface_get_texture (surface);
  int width, height, stride;
  guchar *pixels;
  size_t at;

  if (!texture)
    return FALSE;

  width = gdk_texture_get_width (texture);
  height = gdk_texture_get_height (texture);
  if (x < 0 || y < 0 || x >= width || y >= height)
    return FALSE;

  stride = width * 4;
  pixels = g_malloc ((size_t) stride * height);
  gdk_texture_download (texture, pixels, stride);

  /* gdk_texture_download hands back B8G8R8A8. */
  at = (size_t) y * stride + (size_t) x * 4;
  *b = pixels[at];
  *g = pixels[at + 1];
  *r = pixels[at + 2];
  g_free (pixels);

  return TRUE;
}

/* The colour at the centre of what a surface last committed. The client paints
 * nothing but the two focus colours here, so this is the client's own answer to
 * "which window did wl_keyboard.enter name" — read off the frame the
 * compositor imported, not off the state behind it. */
static gboolean
center_color (NestedSurface *surface, int *r, int *g, int *b)
{
  GdkTexture *texture;

  if (!(texture = nested_surface_get_texture (surface)))
    return FALSE;

  return surface_pixel (surface, (int) gdk_texture_get_width (texture) / 2,
                        (int) gdk_texture_get_height (texture) / 2, r, g, b);
}

/* ---------------------------------------------------------- --damage-scale */

/* The probe in shm-client commits a 200x200 solid red buffer whole, then commits
 * a 200x200 solid green one having damaged 50x50 *surface* pixels and set
 * wl_surface.set_buffer_scale to 2 in that same round. Both requests are
 * double-buffered, so the commit is what decides which scale the damage was
 * written under: the rectangle that has to come out of the new buffer is
 * (0,0,100,100), not the (0,0,50,50) the surface was measured in when the client
 * named it and not the whole buffer either. */
#define PROBE_DS_SIZE    200
#define PROBE_DS_GREEN_R   0
#define PROBE_DS_GREEN_G 255
#define PROBE_DS_GREEN_B   0
#define PROBE_DS_RED_R   255
#define PROBE_DS_RED_G     0
#define PROBE_DS_RED_B     0

static void
damage_scale_fail (const char *format, ...)
{
  va_list args;
  char *text;

  va_start (args, format);
  text = g_strdup_vprintf (format, args);
  va_end (args);

  damage_scale_failed = TRUE;
  g_printerr ("selftest: damage-scale gate FAILED: %s\n", text);
  g_free (text);
}

static void
check_damage_scale (void)
{
  static const struct {
    int x, y;
    int r, g, b;
    const char *where;
    const char *diagnosis;
  } points[] = {
    {  25,  25, PROBE_DS_GREEN_R, PROBE_DS_GREEN_G, PROBE_DS_GREEN_B,
      "inside the rectangle as the client named it",
      "the second commit never reached the screen" },
    {  75,  75, PROBE_DS_GREEN_R, PROBE_DS_GREEN_G, PROBE_DS_GREEN_B,
      "inside it only once the scale this commit applied is used",
      "the damage was multiplied by the scale the surface still had when the "
      "request arrived, so a quarter of the new buffer was read out of it" },
    { 150, 150, PROBE_DS_RED_R, PROBE_DS_RED_G, PROBE_DS_RED_B,
      "outside it, where the frame before has to still be showing",
      "the whole buffer was re-read, so this run exercised no partial copy at "
      "all and cannot tell either scale was wrong" },
  };
  NestedSurface *surface = nested_compositor_get_surface (compositor);
  GdkTexture *texture;
  size_t i;

  if (!surface)
    {
      damage_scale_fail ("there is no window to read a picture off");
      return;
    }

  texture = nested_surface_get_texture (surface);
  if (!texture)
    {
      damage_scale_fail ("the compositor imported no frame from this client");
      return;
    }

  if (gdk_texture_get_width (texture) != PROBE_DS_SIZE ||
      gdk_texture_get_height (texture) != PROBE_DS_SIZE)
    {
      damage_scale_fail ("the frame on screen is %dx%d, not the probe's %dx%d, so "
                         "the ordinary drawing overwrote the picture this gate "
                         "reads",
                         gdk_texture_get_width (texture),
                         gdk_texture_get_height (texture),
                         PROBE_DS_SIZE, PROBE_DS_SIZE);
      return;
    }

  for (i = 0; i < G_N_ELEMENTS (points); i++)
    {
      int r, g, b;

      if (!surface_pixel (surface, points[i].x, points[i].y, &r, &g, &b))
        {
          damage_scale_fail ("pixel (%d,%d) is not in the frame the compositor has",
                             points[i].x, points[i].y);
          continue;
        }

      if (r == points[i].r && g == points[i].g && b == points[i].b)
        continue;

      damage_scale_fail ("pixel (%d,%d), %s, is %02x%02x%02x, wanted "
                         "%02x%02x%02x: %s",
                         points[i].x, points[i].y, points[i].where,
                         r, g, b, points[i].r, points[i].g, points[i].b,
                         points[i].diagnosis);
    }

  if (!damage_scale_failed)
    g_print ("selftest: damage-scale ok: the 50x50 surface rectangle became the "
             "100x100 buffer one, and the other 30000 pixels stayed the frame "
             "before\n");
}

/* ------------------------------------------------------- --drag-no-source */

/* The app-id the client puts on the toplevel it builds out of the drag's icon,
 * and the colour its buffer is filled with. Same numbers as NS_APP_ID and
 * NS_MAGENTA in shm-client.c. */
#define NS_APP_ID      "cefnest-drag-no-source"
#define NS_MAGENTA_R     0xff
#define NS_MAGENTA_G     0x00
#define NS_MAGENTA_B     0xff

static void
drag_no_source_fail (const char *format, ...)
{
  va_list args;
  char *text;

  va_start (args, format);
  text = g_strdup_vprintf (format, args);
  va_end (args);

  drag_no_source_failed = TRUE;
  g_printerr ("selftest: drag-no-source gate FAILED: %s\n", text);
  g_free (text);
}

static void
check_drag_no_source (void)
{
  GList *tops, *l, *stack, *s;
  NestedSurface *icon_window = NULL;
  gboolean painted = FALSE;
  int r, g, b;

  tops = nested_compositor_get_toplevels (compositor);
  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      const char *app_id = nested_surface_get_app_id (surface);

      if (app_id && !strcmp (app_id, NS_APP_ID))
        {
          icon_window = g_object_ref (surface);
          break;
        }
    }
  g_list_free_full (tops, g_object_unref);

  if (!icon_window)
    {
      drag_no_source_fail ("no toplevel with app-id %s, although the client said "
                           "the drop arrived and its icon got a role and a "
                           "configure, so the compositor lost one of those",
                           NS_APP_ID);
      return;
    }

  if (nested_compositor_drag_active (compositor))
    drag_no_source_fail ("a drag still holds the pointer after the release that "
                         "dropped it, so every window underneath has stopped "
                         "taking clicks");

  /* Asked for this window on its own: a second toplevel of one client is not the
   * compositor's primary, and which one is primary has nothing to do with the
   * drag. What is being graded is whether the host is told to draw this surface. */
  stack = nested_compositor_get_paint_stack_for (compositor, icon_window);
  for (s = stack; s; s = s->next)
    if (s->data == icon_window)
      painted = TRUE;
  g_list_free_full (stack, g_object_unref);

  if (!painted)
    {
      drag_no_source_fail ("the window built on a drag's icon is mapped, configured "
                           "and full of pixels and is not in the stack the host is "
                           "told to draw: the drop left the drag alive, and a "
                           "surface marked as an icon is only drawn while a drag is "
                           "carrying it");
    }
  else if (!surface_pixel (icon_window, 60, 60, &r, &g, &b))
    drag_no_source_fail ("the window built on the drag's icon has no frame to read");
  else if (r != NS_MAGENTA_R || g != NS_MAGENTA_G || b != NS_MAGENTA_B)
    drag_no_source_fail ("the window built on the drag's icon paints %02x%02x%02x "
                         "where the magenta it committed should be", r, g, b);
  else if (!drag_no_source_failed)
    g_print ("selftest: drag-no-source ok: the sourceless drag was retired by its "
             "own drop and the surface that carried its icon is a window on screen "
             "again\n");

  g_clear_object (&icon_window);
}

static void
count_toplevels (int expected)
{
  GList *tops = nested_compositor_get_toplevels (compositor);
  GList *l;
  int n = g_list_length (tops);

  g_clear_object (&main_surface);
  g_clear_object (&second_surface);

  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      const char *app_id = nested_surface_get_app_id (surface);
      int width = 0, height = 0;

      nested_surface_get_size (surface, &width, &height);
      g_print ("selftest: toplevel '%s' %dx%d\n",
               app_id ? app_id : "(no app-id)", width, height);

      if (!app_id)
        continue;
      if (!strcmp (app_id, MAIN_APP_ID))
        main_surface = g_object_ref (surface);
      else if (!strcmp (app_id, SECOND_APP_ID))
        second_surface = g_object_ref (surface);
    }

  g_list_free_full (tops, g_object_unref);

  if (n != expected)
    second_fail ("%d toplevel(s) on screen, wanted %d", n, expected);
  if (expected >= 1 && !main_surface)
    second_fail ("no toplevel with app-id %s", MAIN_APP_ID);
  if (expected >= 2 && !second_surface)
    second_fail ("no toplevel with app-id %s", SECOND_APP_ID);
}

static void
route_input (int what)
{
  switch (what)
    {
    case 0:
      nested_compositor_set_input_surface (compositor, main_surface);
      break;
    case 1:
      nested_compositor_set_input_surface (compositor, second_surface);
      break;
    case 2:
      nested_compositor_clear_input_surface (compositor, main_surface);
      break;
    default:
      /* A view unmaps when its window closes or hides. It has to take its own
       * claim back, and nothing else's: a clear that did not check the owner
       * would unroute the window that has since taken the input over. */
      nested_compositor_clear_input_surface (compositor, second_surface);
      break;
    }
}

static void
check_input (int expected)
{
  NestedSurface *want;
  const char *what;
  GList *tops, *l;

  if (expected == 0)
    {
      want = main_surface;
      what = "the first window";
    }
  else if (expected == 1)
    {
      want = second_surface;
      what = "the second window";
    }
  else
    {
      want = nested_compositor_get_surface (compositor);
      what = "the compositor's primary";
    }

  if (!want)
    {
      second_fail ("%s is not on screen to hold the input", what);
      return;
    }

  if (compositor->keyboard_focus != want)
    second_fail ("the keyboard focus is %s, wanted %s",
                 surface_name (compositor->keyboard_focus), what);
  if (compositor->pointer_focus != want)
    second_fail ("the pointer focus is %s, wanted %s",
                 surface_name (compositor->pointer_focus), what);

  /* Both windows state in their own picture where they believe the keyboard
   * went, and only the one named above may claim it. */
  tops = nested_compositor_get_toplevels (compositor);
  for (l = tops; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      int r, g, b;

      if (!center_color (surface, &r, &g, &b))
        {
          second_fail ("%s has no frame to read", surface_name (surface));
          continue;
        }

      if (surface == want)
        {
          if (r != PAINT_RED_R || g != PAINT_RED_G || b != PAINT_RED_B)
            second_fail ("%s painted %02x%02x%02x while the input was said to "
                         "be in it", surface_name (surface), r, g, b);
        }
      else if (r != PAINT_GREEN_R || g != PAINT_GREEN_G || b != PAINT_GREEN_B)
        second_fail ("%s painted %02x%02x%02x while the input was in %s",
                     surface_name (surface), r, g, b, what);
    }
  g_list_free_full (tops, g_object_unref);

  if (!second_failed)
    g_print ("selftest: input is in %s and every window painted that\n", what);
}

/* ---------------------------------------------------------- --gtk-client */

/* The other half of --gtk-client: this same binary launched again with its GDK
 * display pointed at the compositor of the process that started it. No picture
 * is asked for here, and no window is needed to make the point — the sequence
 * that killed the CEF helper was over long before a window could be mapped,
 * because GDK asks for its wl_data_device while still inside gtk_init_check(),
 * and an offer answered in that same dispatch makes it build a clipboard for a
 * display that is not the default yet. See the walk-through in
 * docs/clipboard-offer-during-client-init.md. */
static void
client_clipboard_read (GObject *clipboard, GAsyncResult *result, gpointer data)
{
  char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard), result, NULL);

  /* Reaching this line at all is the result: an offer handled one dispatch too
   * early aborts the process before the main loop has run anything. */
  g_print ("selftest-client: clipboard holds '%s'\n", text ? text : "(nothing)");
  g_free (text);
}

static gboolean
client_read_clipboard (gpointer data)
{
  gdk_clipboard_read_text_async (gdk_display_get_clipboard (gdk_display_get_default ()),
                                 NULL, client_clipboard_read, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean
client_finish (gpointer loop)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

static int
run_as_gtk_client (void)
{
  GMainLoop *loop;

  if (!gtk_init_check ())
    {
      g_printerr ("selftest-client: cannot open the nested compositor as a display\n");
      return 2;
    }

  g_print ("selftest-client: opened display '%s'\n",
           gdk_display_get_name (gdk_display_get_default ()));

  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (200, client_read_clipboard, NULL);
  g_timeout_add (1200, client_finish, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  return 0;
}

static gboolean
gtk_client_watchdog (gpointer loop)
{
  gtk_client_watchdog_id = 0;
  gtk_client_failed = TRUE;
  g_printerr ("selftest: the gtk4 client never came back\n");
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static void
gtk_client_waited (GObject *object, GAsyncResult *result, gpointer data)
{
  GMainLoop *loop = data;
  GError *error = NULL;
  GSubprocess *client = G_SUBPROCESS (object);

  if (gtk_client_watchdog_id)
    {
      g_source_remove (gtk_client_watchdog_id);
      gtk_client_watchdog_id = 0;
    }

  if (!g_subprocess_wait_finish (client, result, &error))
    {
      gtk_client_failed = TRUE;
      g_printerr ("selftest: cannot wait for the gtk4 client: %s\n",
                  error ? error->message : "?");
    }
  else if (g_subprocess_get_if_signaled (client))
    {
      /* A toolkit killed by its own assertion is exactly what this step exists
       * to catch: the Bail out! line it printed is in the log above. */
      gtk_client_failed = TRUE;
      g_printerr ("selftest: gtk4 client killed by signal %d\n",
                  g_subprocess_get_term_sig (client));
    }
  else if (g_subprocess_get_exit_status (client) != 0)
    {
      gtk_client_failed = TRUE;
      g_printerr ("selftest: gtk4 client exited with status %d\n",
                  g_subprocess_get_exit_status (client));
    }
  else
    g_print ("selftest: gtk4 client survived the events of its own display open\n");

  g_main_loop_quit (loop);
}

/* The step cannot answer on its own — the evidence is whether the child is
 * still alive when it says so — so the run's loop is handed over and waits for
 * the report instead of ending here. */
static void
start_gtk_client (GMainLoop *loop)
{
  GSubprocessLauncher *launcher;
  const char *self_argv[] = { "/proc/self/exe", "--as-gtk-client", NULL };
  GError *error = NULL;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  /* Only our socket, and no X11 display to fall back on: the client has to be a
   * client of the compositor under test or the step proves nothing. */
  g_subprocess_launcher_setenv (launcher, "WAYLAND_DISPLAY",
                                nested_compositor_get_socket_name (compositor), TRUE);
  g_subprocess_launcher_setenv (launcher, "GDK_BACKEND", "wayland", TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_SESSION_TYPE", "wayland", TRUE);
  g_subprocess_launcher_unsetenv (launcher, "DISPLAY");

  gtk_child = g_subprocess_launcher_spawnv (launcher, self_argv, &error);
  g_object_unref (launcher);

  if (!gtk_child)
    {
      gtk_client_failed = TRUE;
      g_printerr ("selftest: cannot start the gtk4 client: %s\n",
                  error ? error->message : "?");
      g_main_loop_quit (loop);
      return;
    }

  /* A client stuck on a display it cannot open would otherwise hold the test
   * until meson's timeout, with no line saying what is stuck. */
  gtk_client_watchdog_id = g_timeout_add_seconds (20, gtk_client_watchdog, loop);
  g_subprocess_wait_async (gtk_child, NULL, gtk_client_waited, loop);
}

/* Some steps belong to one mode, and a run of the others has nothing to report
 * about them: the script is one table, so they are skipped by name. */
static gboolean
step_is_ours (StepKind kind)
{
  switch (kind)
    {
    case STEP_GTK_CLIENT:
      return gtk_client_mode;
    case STEP_DRAG_OUT:
    case STEP_DRAG_OUT_BACK:
    case STEP_DRAG_OUT_LEAVE:
    case STEP_DRAG_OUT_CHECK:
      return drag_out_mode;
    case STEP_DRAG_IN:
    case STEP_DRAG_IN_MOTION:
    case STEP_DRAG_IN_SECOND:
    case STEP_DRAG_IN_DROP:
    case STEP_DRAG_IN_CANCEL:
    case STEP_DRAG_IN_CHECK:
      return drag_in_mode;
    case STEP_DAMAGE_SCALE:
      return damage_scale_mode;
    case STEP_DRAG_NO_SOURCE:
      return drag_no_source_mode;
    default:
      return TRUE;
    }
}

static gboolean
run_step (gpointer data)
{
  GMainLoop *loop = data;
  const Step *step = &script[step_index];

  if (step_is_ours (step->kind))
    g_print ("selftest: %s\n", step_names[step->kind]);

  switch (step->kind)
    {
    case STEP_POINTER_ENTER:
      nested_compositor_set_pointer_active (compositor, TRUE);
      nested_compositor_notify_pointer_motion (compositor, step->a, step->b);
      break;
    case STEP_POINTER_MOTION:
      nested_compositor_notify_pointer_motion (compositor, step->a, step->b);
      break;
    case STEP_KEYBOARD_ENTER:
      nested_compositor_set_keyboard_active (compositor, TRUE);
      break;
    case STEP_KEYBOARD_LEAVE:
      nested_compositor_set_keyboard_active (compositor, FALSE);
      break;
    case STEP_BUTTON_PRESS:
      nested_compositor_notify_pointer_button (compositor, step->a, TRUE);
      break;
    case STEP_BUTTON_RELEASE:
      nested_compositor_notify_pointer_button (compositor, step->a, FALSE);
      break;
    case STEP_AXIS:
      /* a is the axis, b the surface pixels; twelve pixels per tenth of a
       * detent, so 120 back-converts the one wheel click this step means. No
       * stop: a detent is a step, not the end of a gesture. */
      nested_compositor_notify_pointer_axis (compositor,
          step->a == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? (double) step->b : 0.0,
          step->a == WL_POINTER_AXIS_VERTICAL_SCROLL ? (double) step->b : 0.0,
          step->a == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? step->b * 12 : 0,
          step->a == WL_POINTER_AXIS_VERTICAL_SCROLL ? step->b * 12 : 0,
          WL_POINTER_AXIS_SOURCE_WHEEL, FALSE);
      break;
    case STEP_AXIS_STOP:
      /* A finger lifting off a touchpad is the only device that really ends a
       * gesture here, and it carries no distance with the end. */
      nested_compositor_notify_pointer_axis (compositor, 0.0, 0.0, 0, 0,
          WL_POINTER_AXIS_SOURCE_FINGER, TRUE);
      break;
    case STEP_KEY_DOWN:
      nested_compositor_notify_keyboard_key (compositor, step->a, TRUE);
      break;
    case STEP_KEY_UP:
      nested_compositor_notify_keyboard_key (compositor, step->a, FALSE);
      break;
    case STEP_TI_STATE:
      {
        const char *surrounding;
        int cursor, anchor;
        GdkRectangle rect;
        gboolean active = nested_compositor_text_input_active (compositor);

        nested_compositor_get_text_input_state (compositor, &surrounding,
                                                &cursor, &anchor, &rect);
        g_print ("selftest: text input active=%d surrounding='%s' caret=%d "
                 "anchor=%d rectangle=%dx%d+%d+%d\n",
                 active, surrounding ? surrounding : "(withheld)", cursor, anchor,
                 rect.width, rect.height, rect.x, rect.y);
      }
      break;
    case STEP_TI_PREEDIT:
      nested_compositor_set_preedit_text (compositor, PREEDIT_TEXT, 2);
      break;
    case STEP_TI_COMMIT:
      nested_compositor_commit_text (compositor, COMMIT_TEXT);
      break;
    case STEP_TI_DELETE:
      nested_compositor_delete_surrounding_text (compositor, step->a, step->b);
      break;
    case STEP_STACK:
      print_stack ();
      break;
    case STEP_DUMP:
      save_texture (dump_path);
      break;
    case STEP_DUMP_TI:
      save_texture (dump_ti_path);
      break;
    case STEP_DUMP_COMPOSITE:
      {
        /* One picture per step, so the run leaves the stack both before the
         * dismissal and after it. */
        char *path = g_strdup_printf ("%s.%d.png", dump_composite_path, step->a);

        save_composite (path);
        g_free (path);
      }
      break;
    case STEP_DRAG_OUT:
      if (!drag_out_mode)
        break;

      fds_at_drag_out = open_fds ();
      /* Past the edge the compositor configured. The widget stops forwarding at
       * its own border, so this is what a pointer that left the window looks
       * like to a seat built out of coordinates. */
      nested_compositor_notify_pointer_motion (compositor, step->a, step->b);
      break;
    case STEP_DRAG_OUT_BACK:
      if (!drag_out_mode)
        break;

      nested_compositor_notify_pointer_motion (compositor, step->a, step->b);
      break;
    case STEP_DRAG_OUT_LEAVE:
      if (!drag_out_mode)
        break;

      fds_at_drag_out = open_fds ();
      /* The other trigger: what the widget reports when GDK tells it the cursor
       * crossed out. No coordinate outside the window is ever named. */
      nested_compositor_set_pointer_active (compositor, FALSE);
      break;
    case STEP_DRAG_OUT_CHECK:
      if (!drag_out_mode)
        break;

      check_drag_out (step->a);
      break;
    case STEP_DRAG_IN:
      if (!drag_in_mode)
        break;

      if (!drag_in_seen)
        fds_at_drag_in = open_fds ();

      if (!host_drag_in (FALSE))
        {
          drag_in_refused++;
          g_printerr ("selftest: drag-in gate FAILED: the compositor would not "
                      "take the host's drag\n");
        }
      break;
    case STEP_DRAG_IN_MOTION:
      if (!drag_in_mode)
        break;

      /* The same call a widget makes when the cursor moves over it: a drag the
       * host is carrying in has to be routed to the surface under the pointer by
       * the seat path, not by a drag-in of its own. */
      nested_compositor_notify_pointer_motion (compositor, step->a, step->b);
      break;
    case STEP_DRAG_IN_SECOND:
      {
        gboolean accepted;

        if (!drag_in_mode)
          break;

        accepted = host_drag_in (TRUE);
        if (accepted)
          {
            g_printerr ("selftest: drag-in gate FAILED: a second drag arrived "
                        "while the first still held the pointer\n");
            drag_in_failed = TRUE;
          }
        else
          g_print ("selftest: the second drag was refused, which is the answer "
                   "a held pointer owes it\n");
      }
      break;
    case STEP_DRAG_IN_DROP:
      if (!drag_in_mode)
        break;

      nested_compositor_drag_from_host_drop (compositor);
      break;
    case STEP_DRAG_IN_CANCEL:
      if (!drag_in_mode)
        break;

      nested_compositor_drag_from_host_cancel (compositor);
      break;
    case STEP_DRAG_IN_CHECK:
      if (!drag_in_mode)
        break;

      check_drag_in (step->a, step->b);
      break;
    case STEP_GTK_CLIENT:
      if (!gtk_client_mode)
        break;

      step_index++;
      start_gtk_client (loop);
      return G_SOURCE_REMOVE;
    case STEP_SECOND_COUNT:
      count_toplevels (step->a);
      break;
    case STEP_SECOND_ROUTE:
      route_input (step->a);
      break;
    case STEP_SECOND_EXPECT:
      check_input (step->a);
      break;
    case STEP_DAMAGE_SCALE:
      if (!damage_scale_mode)
        break;

      check_damage_scale ();
      break;
    case STEP_DRAG_NO_SOURCE:
      if (!drag_no_source_mode)
        break;

      check_drag_no_source ();
      break;
    default:
      break;
    }

  step_index++;

  if (step_index >= script_count)
    {
      g_main_loop_quit (loop);
      return G_SOURCE_REMOVE;
    }

  g_timeout_add (script[step_index].delay ? script[step_index].delay : 20,
                 run_step, loop);

  return G_SOURCE_REMOVE;
}

/* The compositor runs in this process, so the descriptors a client hands it are
 * counted here. A run that gets refused forty imports must not end with forty
 * more open than it started with. */
static int
open_fds (void)
{
  GDir *dir = g_dir_open ("/proc/self/fd", 0, NULL);
  int n = 0;

  if (dir)
    {
      while (g_dir_read_name (dir))
        n++;
      g_dir_close (dir);
    }

  return n;
}

/* ------------------------------------------------------------ --drag-out */

/* A drag that leaves the window has no destination this compositor can hand to,
 * so the compositor reads the payload itself and gives the whole thing to the
 * host. What a real page's text drag was measured to offer is three names for
 * one string, and --drag-out replays exactly that: the handover has to carry all
 * three types and the payload under the one the compositor picked out of the
 * list. */
#define DRAG_OUT_MIME    "text/plain;charset=utf-8"
#define DRAG_OUT_PAYLOAD "drag-out-payload"
#define DRAG_OUT_TYPES   DRAG_OUT_MIME "|text/plain|chromium/x-web-custom-data"

static char drag_out_mime[128];
static char drag_out_types[512];
static char drag_out_payload[64];
static gsize drag_out_size;

static void
drag_out_handed_over (NestedCompositor *compositor, const char *mime,
                      GBytes *bytes, char **types, gpointer data)
{
  const char *payload = bytes ? g_bytes_get_data (bytes, NULL) : NULL;
  char *joined = types ? g_strjoinv ("|", types) : NULL;
  unsigned n_types = types ? g_strv_length (types) : 0;
  int take;

  (void) compositor;
  (void) data;

  drag_out_seen++;
  drag_out_size = bytes ? g_bytes_get_size (bytes) : 0;
  take = (int) drag_out_size;
  if (take > (int) sizeof drag_out_payload - 1)
    take = (int) sizeof drag_out_payload - 1;

  /* Copied, not kept: the signal hands over nothing that outlives its handler. */
  g_snprintf (drag_out_mime, sizeof drag_out_mime, "%s", mime ? mime : "(none)");
  g_snprintf (drag_out_types, sizeof drag_out_types, "%s", joined ? joined : "(none)");
  g_snprintf (drag_out_payload, sizeof drag_out_payload, "%.*s", take,
              payload ? payload : "");
  g_free (joined);

  g_print ("selftest: drag-out #%d %s (%" G_GSIZE_FORMAT " bytes, %u type%s) '%s'\n",
           drag_out_seen, drag_out_mime, drag_out_size, n_types,
           n_types == 1 ? "" : "s", drag_out_payload);
}

/* Both triggers of the handover end here, so this is where a run says whether
 * what came out of the page is what the page put in. */
static void
check_drag_out (int expected)
{
  int fds_now = open_fds ();
  gboolean failed = FALSE;

  if (drag_out_seen != expected)
    {
      g_printerr ("selftest: drag-out gate FAILED: %d handover(s), wanted %d\n",
                  drag_out_seen, expected);
      failed = TRUE;
    }

  if (drag_out_seen >= expected)
    {
      if (strcmp (drag_out_mime, DRAG_OUT_MIME) != 0)
        {
          g_printerr ("selftest: drag-out gate FAILED: handed over '%s', wanted '%s'\n",
                      drag_out_mime, DRAG_OUT_MIME);
          failed = TRUE;
        }

      if (strcmp (drag_out_types, DRAG_OUT_TYPES) != 0)
        {
          g_printerr ("selftest: drag-out gate FAILED: types '%s', wanted '%s'\n",
                      drag_out_types, DRAG_OUT_TYPES);
          failed = TRUE;
        }

      if (drag_out_size != strlen (DRAG_OUT_PAYLOAD) ||
          memcmp (drag_out_payload, DRAG_OUT_PAYLOAD, drag_out_size) != 0)
        {
          g_printerr ("selftest: drag-out gate FAILED: payload '%s' of %" G_GSIZE_FORMAT
                      " bytes, wanted '%s'\n", drag_out_payload, drag_out_size,
                      DRAG_OUT_PAYLOAD);
          failed = TRUE;
        }
    }

  /* The page's drag is the run's other half: a handover that left it alive
   * would strand a drag with no destination, and one that left the read pipe
   * open is a descriptor the compositor never gives back. */
  {
    char **live = nested_compositor_drag_get_types (compositor);

    if (g_strv_length (live) > 0)
      {
        g_printerr ("selftest: drag-out gate FAILED: the page kept its drag, "
                    "still offering %s\n", g_strjoinv ("|", live));
        failed = TRUE;
      }

    g_strfreev (live);
  }

  if (fds_now > fds_at_drag_out)
    {
      g_printerr ("selftest: drag-out gate FAILED: the read left %d descriptor(s) "
                  "behind\n", fds_now - fds_at_drag_out);
      failed = TRUE;
    }

  if (failed)
    drag_out_failed = TRUE;
  else
    g_print ("selftest: drag-out gate ok, %d handover(s) and the drag is gone\n",
             drag_out_seen);
}

/* ------------------------------------------------------------ --drag-in */

/* The host's side of an inbound drag, which is what a real one would be: bytes
 * already in this process, and the two text names a drop of plain text carries.
 * The destination this run has is the shm client, and a page was measured taking
 * the short of the two names — so that is the one the verdict has to name. */
#define DRAG_IN_MIME    "text/plain"
#define DRAG_IN_PAYLOAD "drag-in-payload"

static const char *drag_in_types[] = { "text/plain;charset=utf-8",
                                       "text/plain", NULL };

static gboolean
host_drag_in (gboolean can_move)
{
  GBytes *payload;
  gboolean accepted;

  payload = g_bytes_new (DRAG_IN_PAYLOAD, strlen (DRAG_IN_PAYLOAD));
  accepted = nested_compositor_drag_from_host (compositor, drag_in_types,
                                               payload, can_move);
  /* The compositor keeps its own reference for as long as the drag lives, so
   * this one is the run's to give back whether or not it was taken. */
  g_bytes_unref (payload);

  return accepted;
}

static void
drag_in_verdict (NestedCompositor *compositor, gboolean taken, const char *mime,
                 gpointer data)
{
  (void) compositor;
  (void) data;

  drag_in_seen++;
  drag_in_taken = taken;
  g_snprintf (drag_in_mime, sizeof drag_in_mime, "%s", mime ? mime : "(none)");

  g_print ("selftest: drag-in-end #%d %s, %s\n", drag_in_seen, taken ? "taken" : "given back",
           drag_in_mime);
}

/* One verdict per drag that ended, which is the only report the host gets: the
 * destination read what it was handed, or it did not. */
static void
check_drag_in (int expected, int expected_taken)
{
  int fds_now = open_fds ();
  gboolean failed = FALSE;

  if (drag_in_seen != expected)
    {
      g_printerr ("selftest: drag-in gate FAILED: %d verdict(s), wanted %d\n",
                  drag_in_seen, expected);
      failed = TRUE;
    }

  if (drag_in_refused)
    {
      g_printerr ("selftest: drag-in gate FAILED: %d arrival(s) the compositor "
                  "refused\n", drag_in_refused);
      failed = TRUE;
    }

  if (drag_in_seen >= expected)
    {
      if (drag_in_taken != (gboolean) expected_taken)
        {
          g_printerr ("selftest: drag-in gate FAILED: the last drag ended "
                      "taken=%d, wanted taken=%d\n", drag_in_taken, expected_taken);
          failed = TRUE;
        }

      /* Only a taken drag is owed a type: a cancel leaves nothing for the
       * destination to have accepted. */
      if (expected_taken && strcmp (drag_in_mime, DRAG_IN_MIME) != 0)
        {
          g_printerr ("selftest: drag-in gate FAILED: taken as '%s', wanted '%s'\n",
                      drag_in_mime, DRAG_IN_MIME);
          failed = TRUE;
        }
    }

  /* The drag has to be gone by the time the verdict arrives, or the next one
   * from the host has nothing to arrive into. */
  {
    char **live = nested_compositor_drag_get_types (compositor);

    if (g_strv_length (live) > 0)
      {
        g_printerr ("selftest: drag-in gate FAILED: the drag outlived its verdict, "
                    "still offering %s\n", g_strjoinv ("|", live));
        failed = TRUE;
      }

    g_strfreev (live);
  }

  if (fds_now > fds_at_drag_in)
    {
      g_printerr ("selftest: drag-in gate FAILED: the drags left %d descriptor(s) "
                  "behind\n", fds_now - fds_at_drag_in);
      failed = TRUE;
    }

  if (failed)
    drag_in_failed = TRUE;
  else
    g_print ("selftest: drag-in gate ok, %d verdict(s) and the drag is gone\n",
             drag_in_seen);
}

static void
fullscreen_request_seen (NestedCompositor *compositor, gboolean enter, gpointer data)
{
  (void) compositor; (void) data;
  if (enter)
    {
      fs_enter_seen++;
      g_print ("selftest: fullscreen-request ENTER (%d)\n", fs_enter_seen);
    }
  else
    {
      fs_exit_seen++;
      g_print ("selftest: fullscreen-request EXIT (%d)\n", fs_exit_seen);
    }
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GSubprocessLauncher *launcher;
  GMainLoop *loop;
  GPtrArray *client_argv;
  int fds_at_start;
  const char *client = argc > 2 ? argv[2] : CLIENT_PATH;
  int i;

  /* --gtk-client runs in two processes, and this is how the second one knows it
   * is the client and not the compositor: argv[1] is the role, not the dump
   * path. */
  if (argc > 1 && !strcmp (argv[1], "--as-gtk-client"))
    return run_as_gtk_client ();

  for (i = 3; i < argc; i++)
    {
      gtk_client_mode = gtk_client_mode || !strcmp (argv[i], "--gtk-client");
      dmabuf_mode = dmabuf_mode || !strcmp (argv[i], "--dmabuf");
      drag_out_mode = drag_out_mode || !strcmp (argv[i], "--drag-out");
      drag_in_mode = drag_in_mode || !strcmp (argv[i], "--drag-in");
      second_mode = second_mode || !strcmp (argv[i], "--second-window");
      damage_scale_mode = damage_scale_mode || !strcmp (argv[i], "--damage-scale");
      drag_no_source_mode = drag_no_source_mode || !strcmp (argv[i], "--drag-no-source");
    }

  script = second_mode ? second_script : default_script;
  script_count = second_mode ? G_N_ELEMENTS (second_script)
                             : G_N_ELEMENTS (default_script);

  dump_path = argc > 1 ? argv[1] : (char *) "/tmp/seat-selftest.png";

  /* The compositor paints into GdkTextures and reads the render node through
   * GDK, so this test is only meaningful where a display can be opened.
   *
   * 77 is what meson reads as SKIPPED; 0 it reads as a pass, which is how a run
   * on a machine nobody is sitting at used to report thirteen green tests that
   * had composited nothing. The verdict has to say which of the two happened. */
  if (!gtk_init_check ())
    {
      g_print ("selftest: no GDK display, skipping\n");
      return 77;
    }

  /* The composition frames go beside the plain one, with the same name plus a
   * mark, so a run leaves both pictures. */
  if (g_str_has_suffix (dump_path, ".png"))
    {
      dump_ti_path = g_strdup_printf ("%.*s.text-input.png",
                                      (int) strlen (dump_path) - 4, dump_path);
      dump_composite_path = g_strdup_printf ("%.*s.composite",
                                             (int) strlen (dump_path) - 4, dump_path);
    }
  else
    {
      dump_ti_path = g_strdup_printf ("%s.text-input.png", dump_path);
      dump_composite_path = g_strdup_printf ("%s.composite", dump_path);
    }

  fds_at_start = open_fds ();

  /* Before any handler is registered, and for every mode rather than the one
   * that found the bug: a critical means the library misused its own arguments,
   * and the client that provoked it is only the reporter. */
  g_log_set_writer_func (count_critical_records, NULL, NULL);

  if (dmabuf_mode)
    g_log_set_handler ("nested",
                       G_LOG_LEVEL_MASK & ~G_LOG_LEVEL_CRITICAL,
                       count_dmabuf_lines, NULL);

  compositor = nested_compositor_new (&error);
  if (!compositor)
    {
      g_printerr ("selftest: %s\n", error ? error->message : "no compositor");
      return 1;
    }

  g_print ("selftest: compositor on %s, client %s\n",
           nested_compositor_get_socket_name (compositor), client);
  g_signal_connect (compositor, "window-fullscreen-request",
                    G_CALLBACK (fullscreen_request_seen), NULL);
  /* Listening is what makes the handover happen: a host that never asked to be
   * given a drag's payload leaves the page's drag alone, and that is how every
   * other mode of this test runs. */
  if (drag_out_mode)
    g_signal_connect (compositor, "drag-out", G_CALLBACK (drag_out_handed_over), NULL);
  /* The verdict of an inbound drag is the only thing this process gets to know
   * about it, and a host has to answer its own outer GdkDrop with that verdict,
   * so listening is half of what the mode checks. */
  if (drag_in_mode)
    g_signal_connect (compositor, "drag-in-end", G_CALLBACK (drag_in_verdict), NULL);
  for (int i = 3; i < argc; i++)
    {
      if (!strcmp (argv[i], "--fullscreen"))
        fullscreen_gate = 1;
      /* The three modes that end their own connection: each judges the answer it
       * got and leaves, so no picture is ever on offer here. */
      else if (!strcmp (argv[i], "--refused") || !strcmp (argv[i], "--pool-budget") ||
               !strcmp (argv[i], "--pool-shrink"))
        paint_exempt = TRUE;
    }

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv (launcher, "WAYLAND_DISPLAY",
                                nested_compositor_get_socket_name (compositor), TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_SESSION_TYPE", "wayland", TRUE);
  g_subprocess_launcher_unsetenv (launcher, "DISPLAY");

  /* Everything from argv[3] on goes to the client, so the same script can drive
   * it with --dmabuf or --text-input. */
  client_argv = g_ptr_array_new ();
  g_ptr_array_add (client_argv, (gpointer) client);
  for (i = 3; i < argc; i++)
    g_ptr_array_add (client_argv, (gpointer) argv[i]);
  /* An offer is only sent when something owns the selection, and the host
   * clipboard may well be empty. Asking the client to take one makes the gtk4
   * step send the same events on every machine it runs on. */
  if (gtk_client_mode)
    g_ptr_array_add (client_argv, (gpointer) "--copy");
  g_ptr_array_add (client_argv, NULL);

  child = g_subprocess_launcher_spawnv (launcher,
                                        (const char * const *) client_argv->pdata,
                                        &error);
  g_ptr_array_free (client_argv, TRUE);
  g_object_unref (launcher);

  if (!child)
    {
      g_printerr ("selftest: cannot start %s: %s\n", client,
                  error ? error->message : "?");
      return 1;
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (script[0].delay, run_step, loop);
  /* Waiting, not polling: a client that leaves while the script still has steps
   * to run has complained, and the complaint ends the run there. */
  g_subprocess_wait_async (child, NULL, client_exited, loop);

  g_main_loop_run (loop);

  run_finished = TRUE;
  g_print ("selftest: finished at step %zu, %d fds open (%d at start)\n",
           step_index, open_fds (), fds_at_start);

  g_main_loop_unref (loop);

  g_subprocess_force_exit (child);
  g_object_unref (child);
  if (gtk_child)
    {
      g_subprocess_force_exit (gtk_child);
      g_object_unref (gtk_child);
    }
  g_object_unref (compositor);
  g_clear_object (&main_surface);
  g_clear_object (&second_surface);
  g_free (dump_ti_path);
  g_free (dump_composite_path);

  /* --dmabuf only proves something when the picture really came through the
   * import: the client falls back to wl_shm on any refusal and still exits 0,
   * so the counters, not the exit status, are the evidence. A machine with no
   * render node never advertises dmabuf at all — there this run exercises the
   * fallback, and says so instead of implying an import it did not do. */
  if (dmabuf_mode)
    {
      if (dmabuf_refusals)
        g_printerr ("selftest: the compositor refused %d dmabuf import(s)\n",
                    dmabuf_refusals);
      else if (dmabuf_advertised != 1)
        g_print ("selftest: no usable render node, so no dmabuf was "
                 "advertised — this run is fallback evidence, not import "
                 "evidence\n");
      else if (!dmabuf_imports)
        g_printerr ("selftest: dmabuf was advertised but the compositor "
                    "imported nothing\n");
      else
        g_print ("selftest: %d dmabuf buffer(s) imported from the client\n",
                 dmabuf_imports);
    }

  /* A client that died on what the compositor sent is a failure of the
   * compositor, not of the test — and that is true of the toolkit one as much
   * as of this raw one. */
  if (fullscreen_gate)
    {
      if (fs_enter_seen && fs_exit_seen)
        g_print ("selftest: fullscreen edges seen (%d enter, %d exit)\n",
                 fs_enter_seen, fs_exit_seen);
      else
        g_printerr ("selftest: fullscreen gate FAILED: enter=%d exit=%d\n",
                    fs_enter_seen, fs_exit_seen);
    }

  /* Two windows of one client, and the seat putting the pointer and the
   * keyboard in the one the host named — including when the window it named
   * stops existing. Each check prints its own complaint, so a run says which
   * of the two disagree. */
  if (second_mode && !second_failed)
    g_print ("selftest: second-window gate ok, input followed the routing and "
             "both windows painted it\n");

  /* A client that painted and a compositor that imported the picture are two
   * different claims. Until now only the first was graded: a run whose frames
   * never became a texture still ended with the client leaving tidily and a green
   * line, which is the false pass this check exists to stop. */
  if (!client_frame_seen && !paint_exempt)
    g_printerr ("selftest: the compositor never had a client frame to paint\n");

  return (gtk_client_failed || client_failed || drag_out_failed || drag_in_failed ||
          second_failed || damage_scale_failed || drag_no_source_failed ||
          glib_criticals ||
          (!client_frame_seen && !paint_exempt) ||
          dmabuf_refusals ||
          (fullscreen_gate && !(fs_enter_seen && fs_exit_seen)) ||
          (dmabuf_mode && dmabuf_advertised == 1 && !dmabuf_imports)) ? 1 : 0;
}
