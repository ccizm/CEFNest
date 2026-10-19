/* Drives the nested seat without any real input device: it starts the shm test
 * client on the compositor's socket, injects a scripted pointer and keyboard
 * sequence, then writes the client's own buffer out as a PNG. What the client
 * painted in response is the evidence that the events arrived.
 *
 * The composition steps stand in for a real input method: they call the same
 * emitter the embedding widget does, so a client's preedit and commits are
 * evidence of the zwp_text_input_v3 round trip. */
#define G_LOG_DOMAIN "seat-selftest"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nested.h"

#define BTN_LEFT 0x110
#define KEY_A    30 /* evdev, i.e. xkb keycode 38 */

typedef enum {
  STEP_SETTLE,
  STEP_POINTER_ENTER,
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
  STEP_DUMP,
  STEP_DUMP_TI,
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
  { STEP_POINTER_ENTER, 300, 200,  60 },
  { STEP_KEYBOARD_ENTER,  0,   0,  60 },
  { STEP_BUTTON_PRESS, BTN_LEFT, 1, 60 },
  { STEP_AXIS,              1, 100, 60 },
  { STEP_KEY_DOWN,      KEY_A,   1, 60 },
  { STEP_KEY_UP,        KEY_A,   0, 60 },
  { STEP_SETTLE,          0,   0, 200 }, /* one client redraw after the input */
  { STEP_DUMP,            0,   0,   0 },
  { STEP_TI_STATE,        0,   0, 150 }, /* the client's enable and report land */
  { STEP_TI_PREEDIT,      0,   0,  80 },
  { STEP_TI_COMMIT,       0,   0,  80 },
  { STEP_TI_DELETE,      -2,   2, 150 },
  { STEP_DUMP_TI,         0,   0,   0 },
  { STEP_KEYBOARD_LEAVE,  0,   0,  80 },
  { STEP_SETTLE,          0,   0, 120 },
  { STEP_BUTTON_RELEASE, BTN_LEFT, 0, 0 },
};

static const char *step_names[N_STEP_KINDS] = {
  "settle", "pointer-enter", "keyboard-enter", "keyboard-leave", "button-press",
  "button-release", "axis", "key-down", "key-up", "text-input-state",
  "preedit", "commit", "delete-surrounding", "dump", "dump-text-input",
};

#define PREEDIT_TEXT "nihao"
#define COMMIT_TEXT  "你好" /* two characters, six bytes */

static NestedCompositor *compositor;
static GSubprocess *child;
static size_t step_index;
static char *dump_path;
static char *dump_ti_path;

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
      nested_compositor_notify_pointer_axis (compositor, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                             step->b, step->b / 10);
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
    case STEP_DUMP:
      save_texture (dump_path);
      break;
    case STEP_DUMP_TI:
      save_texture (dump_ti_path);
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

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GSubprocessLauncher *launcher;
  GMainLoop *loop;
  GPtrArray *client_argv;
  const char *client = argc > 2 ? argv[2] : CLIENT_PATH;
  int i;

  dump_path = argc > 1 ? argv[1] : (char *) "/tmp/seat-selftest.png";

  /* The composition frames go beside the plain one, with the same name plus a
   * mark, so a run leaves both pictures. */
  if (g_str_has_suffix (dump_path, ".png"))
    dump_ti_path = g_strdup_printf ("%.*s.text-input.png",
                                    (int) strlen (dump_path) - 4, dump_path);
  else
    dump_ti_path = g_strdup_printf ("%s.text-input.png", dump_path);

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

  g_print ("selftest: finished at step %zu\n", step_index);

  g_main_loop_unref (loop);

  g_subprocess_force_exit (child);
  g_object_unref (child);
  g_object_unref (compositor);
  g_free (dump_ti_path);

  return 0;
}
