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
 * which is what the embedding widget draws. */
#define G_LOG_DOMAIN "seat-selftest"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cairo.h>

#include "nested.h"

#define BTN_LEFT 0x110
#define KEY_A    30 /* evdev, i.e. xkb keycode 38 */

typedef enum {
  STEP_SETTLE,
  STEP_POINTER_ENTER,
  STEP_POINTER_MOTION,
  STEP_KEYBOARD_ENTER,
  STEP_KEYBOARD_LEAVE,
  STEP_BUTTON_PRESS,
  STEP_BUTTON_RELEASE,
  STEP_AXIS,
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
  N_STEP_KINDS
} StepKind;

typedef struct {
  StepKind kind;
  int a;
  int b;
  int delay;
} Step;

static const Step script[] = {
  { STEP_SETTLE,          0,   0, 800 }, /* let the client map and draw */
  { STEP_STACK,           0,   0,   0 }, /* where the popups were placed */
  { STEP_DUMP_COMPOSITE,  1,   0,  60 },
  { STEP_POINTER_ENTER, 300, 200,  60 },
  { STEP_KEYBOARD_ENTER,  0,   0,  60 },
  { STEP_BUTTON_PRESS, BTN_LEFT, 1, 60 },
  { STEP_AXIS, WL_POINTER_AXIS_VERTICAL_SCROLL, 100, 60 },
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
  /* Off the menu, and a press there: xdg_popup.grab means the compositor owns
   * the dismissal, so the client should report popup_done for the menu. */
  { STEP_POINTER_MOTION,   60,  60, 80 },
  { STEP_BUTTON_PRESS, BTN_LEFT,  1, 60 },
  { STEP_BUTTON_RELEASE, BTN_LEFT, 0, 120 },
  { STEP_STACK,           0,   0,   0 },
  { STEP_DUMP_COMPOSITE,  2,   0,   0 },
};

static const char *step_names[N_STEP_KINDS] = {
  "settle", "pointer-enter", "pointer-motion", "keyboard-enter", "keyboard-leave",
  "button-press", "button-release", "axis", "key-down", "key-up",
  "text-input-state", "preedit", "commit", "delete-surrounding", "stack",
  "dump", "dump-text-input", "dump-composite",
};

#define PREEDIT_TEXT "nihao"
#define COMMIT_TEXT  "你好" /* two characters, six bytes */

static NestedCompositor *compositor;
static GSubprocess *child;
static size_t step_index;
static char *dump_path;
static char *dump_ti_path;
static char *dump_composite_path;

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

static gboolean
run_step (gpointer data)
{
  GMainLoop *loop = data;
  const Step *step = &script[step_index];

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
       * detent, so 120 back-converts the one wheel click this step means. */
      nested_compositor_notify_pointer_axis (compositor,
          step->a == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? (double) step->b : 0.0,
          step->a == WL_POINTER_AXIS_VERTICAL_SCROLL ? (double) step->b : 0.0,
          step->a == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? step->b * 12 : 0,
          step->a == WL_POINTER_AXIS_VERTICAL_SCROLL ? step->b * 12 : 0,
          WL_POINTER_AXIS_SOURCE_WHEEL, TRUE);
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
    default:
      break;
    }

  step_index++;

  if (step_index >= G_N_ELEMENTS (script))
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

  dump_path = argc > 1 ? argv[1] : (char *) "/tmp/seat-selftest.png";

  /* The compositor paints into GdkTextures and reads the render node through
   * GDK, so this test is only meaningful where a display can be opened. Without
   * one it reports itself skipped rather than failed — `meson test` runs on
   * machines nobody is sitting at. */
  if (!gtk_init_check ())
    {
      g_print ("selftest: no GDK display, skipping\n");
      return 0;
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

  compositor = nested_compositor_new (&error);
  if (!compositor)
    {
      g_printerr ("selftest: %s\n", error ? error->message : "no compositor");
      return 1;
    }

  g_print ("selftest: compositor on %s, client %s\n",
           nested_compositor_get_socket_name (compositor), client);

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

  g_main_loop_run (loop);

  g_print ("selftest: finished at step %zu, %d fds open (%d at start)\n",
           step_index, open_fds (), fds_at_start);

  g_main_loop_unref (loop);

  g_subprocess_force_exit (child);
  g_object_unref (child);
  g_object_unref (compositor);
  g_free (dump_ti_path);
  g_free (dump_composite_path);

  return 0;
}
