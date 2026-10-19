/* Creates a virtual mouse and keyboard with uinput and replays a script of
 * events, so the widget -> nested seat path can be driven without a human.
 * Needs write access to /dev/uinput. */

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>

static int mouse_fd = -1;
static int kbd_fd = -1;

/* Physical pointing devices claimed with EVIOCGRAB for the duration of the
 * script. Mutter drives one cursor for every pointer, so a trackpoint that
 * twitches would otherwise be mixed into the injected walk. The kernel drops
 * the grab when the process exits, so no cleanup handler is needed. */
#define MAX_GRABS 16
static int grab_fd[MAX_GRABS];
static int n_grabs;

/* The event nodes this program created itself, so the grab loop can tell them
 * apart from the hardware. Matching on the sysfs name proved unreliable. */
static char our_nodes[MAX_GRABS][32];
static int n_our_nodes;

static void
remember_node (const char *d_name)
{
  if (n_our_nodes < MAX_GRABS)
    snprintf (our_nodes[n_our_nodes++], sizeof our_nodes[0], "%s", d_name);
}

static int
is_our_node (const char *d_name)
{
  int i;

  for (i = 0; i < n_our_nodes; i++)
    if (!strcmp (our_nodes[i], d_name))
      return 1;
  return 0;
}

static void
grab_physical_pointers (void)
{
  DIR *dir;
  struct dirent *de;

  dir = opendir ("/sys/class/input");
  if (!dir)
    return;

  while ((de = readdir (dir)) && n_grabs < MAX_GRABS)
    {
      char path[512], rel[128], name[128], node[512];
      FILE *f;
      char *p;
      int fd;
      int has_rel = 0;

      if (strncmp (de->d_name, "event", 5) != 0)
        continue;

      if (is_our_node (de->d_name))
        continue;

      snprintf (path, sizeof path, "/sys/class/input/%s/device/capabilities/rel", de->d_name);
      f = fopen (path, "r");
      if (f)
        {
          if (fgets (rel, sizeof rel, f))
            {
              for (p = rel; *p; p++)
                if (*p >= '1' && *p <= '9' && *p != ' ')
                  has_rel = 1;
            }
          fclose (f);
        }
      if (!has_rel)
        continue;

      name[0] = '\0';
      snprintf (path, sizeof path, "/sys/class/input/%s/device/name", de->d_name);
      f = fopen (path, "r");
      if (f)
        {
          if (!fgets (name, sizeof name, f))
            name[0] = '\0';
          fclose (f);
        }

      snprintf (node, sizeof node, "/dev/input/%s", de->d_name);
      fd = open (node, O_RDWR | O_CLOEXEC);
      if (fd < 0)
        continue;

      if (ioctl (fd, EVIOCGRAB, 1) == 0)
        {
          grab_fd[n_grabs++] = fd;
          fprintf (stderr, "inject: grabbed %s (%s)\n", node, name);
        }
      else
        close (fd);
    }

  closedir (dir);
}

static void
sleep_ms (int ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long) (ms % 1000) * 1000000L;
  nanosleep (&ts, NULL);
}

static void
emit (int fd, unsigned short type, unsigned short code, int value)
{
  struct input_event ev;

  memset (&ev, 0, sizeof ev);
  ev.type = type;
  ev.code = code;
  ev.value = value;

  if (write (fd, &ev, sizeof ev) < 0)
    fprintf (stderr, "inject: write %u/%u/%d failed: %s\n", type, code, value,
             strerror (errno));
}

static void
emit_sync (int fd)
{
  emit (fd, EV_SYN, 0, 0);
}

static const char *
find_event_node (int fd, char *buf, size_t buf_size)
{
  /* UI_GET_SYSNAME gives "devices/virtual/input/inputN"; keep only the event
   * node so the log says which /dev/input/eventX the session picked up. */
  char sysname[64] = "";
  DIR *dir;
  struct dirent *de;

  snprintf (buf, buf_size, "?");

  if (ioctl (fd, UI_GET_SYSNAME (sizeof sysname), sysname) <= 0)
    return buf;

  dir = opendir ("/sys/class/input");
  if (!dir)
    return buf;

  while ((de = readdir (dir)))
    {
      char path[512], target[512];
      ssize_t n;

      if (strncmp (de->d_name, "event", 5) != 0)
        continue;

      snprintf (path, sizeof path, "/sys/class/input/%s/device", de->d_name);
      n = readlink (path, target, sizeof target - 1);
      if (n <= 0)
        continue;
      target[n] = '\0';

      if (strstr (target, sysname))
        {
          snprintf (buf, buf_size, "/dev/input/%s", de->d_name);
          break;
        }
    }

  closedir (dir);
  return buf;
}

static int
open_dev (const char *name, int is_keyboard)
{
  struct uinput_user_dev dev;
  char node[64];
  int fd, i;

  fd = open ("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    {
      fprintf (stderr, "inject: cannot open /dev/uinput: %s\n", strerror (errno));
      return -1;
    }

  if (ioctl (fd, UI_SET_EVBIT, EV_SYN) < 0)
    goto fail;

  if (is_keyboard)
    {
      if (ioctl (fd, UI_SET_EVBIT, EV_KEY) < 0)
        goto fail;
      for (i = KEY_ESC; i < 256; i++)
        if (ioctl (fd, UI_SET_KEYBIT, i) < 0)
          goto fail;
    }
  else
    {
      static const unsigned short buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE,
                                                 BTN_SIDE, BTN_EXTRA };
      static const unsigned short rels[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL };

      if (ioctl (fd, UI_SET_EVBIT, EV_KEY) < 0 ||
          ioctl (fd, UI_SET_EVBIT, EV_REL) < 0)
        goto fail;

      for (i = 0; i < (int) (sizeof buttons / sizeof buttons[0]); i++)
        if (ioctl (fd, UI_SET_KEYBIT, buttons[i]) < 0)
          goto fail;
      for (i = 0; i < (int) (sizeof rels / sizeof rels[0]); i++)
        if (ioctl (fd, UI_SET_RELBIT, rels[i]) < 0)
          goto fail;
    }

  memset (&dev, 0, sizeof dev);
  snprintf (dev.name, UINPUT_MAX_NAME_SIZE, "%s", name);
  dev.id.bustype = BUS_VIRTUAL;
  dev.id.vendor = 0x1234;
  dev.id.product = is_keyboard ? 0x0001 : 0x0002;
  dev.id.version = 1;

  if (write (fd, &dev, sizeof dev) < 0)
    goto fail;
  if (ioctl (fd, UI_DEV_SETUP, &dev) < 0)
    goto fail;
  if (ioctl (fd, UI_DEV_CREATE) < 0)
    goto fail;

  fprintf (stderr, "inject: %s is %s\n", is_keyboard ? "keyboard" : "mouse",
           find_event_node (fd, node, sizeof node));
  {
    char *base = strrchr (node, '/');
    remember_node (base ? base + 1 : node);
  }
  return fd;

fail:
  fprintf (stderr, "inject: setup failed: %s\n", strerror (errno));
  close (fd);
  return -1;
}

static void
close_devs (void)
{
  if (mouse_fd >= 0)
    {
      ioctl (mouse_fd, UI_DEV_DESTROY);
      close (mouse_fd);
    }
  if (kbd_fd >= 0)
    {
      ioctl (kbd_fd, UI_DEV_DESTROY);
      close (kbd_fd);
    }
}

/* Grabbing the hardware is only ever needed to keep a stray finger out of an
 * injected pointer walk; a keyboard-only script must not leave the touchpad
 * dead while it runs. */
static int
script_uses_pointer (FILE *script)
{
  char line[256];
  int uses = 0;

  while (fgets (line, sizeof line, script))
    if (!strncmp (line, "m ", 2) || !strncmp (line, "b ", 2) ||
        !strncmp (line, "w ", 2) || !strncmp (line, "h ", 2))
      uses = 1;

  rewind (script);
  return uses;
}

static int
run (FILE *script)
{  char line[256];

  while (fgets (line, sizeof line, script))
    {
      char cmd[8] = "";
      long a = 0, b = 0, c = 1, d = 40;
      char *text;

      if (line[0] == '#' || line[0] == '\n')
        continue;

      sscanf (line, "%7s %ld %ld %ld %ld", cmd, &a, &b, &c, &d);
      text = strchr (line, '\n');
      if (text)
        *text = '\0';

      if (!strcmp (cmd, "sleep"))
        sleep_ms ((int) a);
      else if (!strcmp (cmd, "mark"))
        {
          char *mark = line + strlen (cmd);

          while (*mark == ' ')
            mark++;
          fprintf (stderr, "inject: >>> %s\n", mark);
          fflush (stderr);
        }
      else if (!strcmp (cmd, "m"))
        {
          long i;

          for (i = 0; i < c; i++)
            {
              emit (mouse_fd, EV_REL, REL_X, (int) a);
              emit (mouse_fd, EV_REL, REL_Y, (int) b);
              emit_sync (mouse_fd);
              sleep_ms ((int) d);
            }
        }
      else if (!strcmp (cmd, "b"))
        {
          emit (mouse_fd, EV_KEY, (unsigned short) a, (int) b);
          emit_sync (mouse_fd);
          sleep_ms ((int) c);
        }
      else if (!strcmp (cmd, "w") || !strcmp (cmd, "h"))
        {
          int steps = (int) a;
          int i;

          for (i = 0; i < (int) (steps < 0 ? -steps : steps); i++)
            {
              emit (mouse_fd, EV_REL,
                    cmd[0] == 'w' ? REL_WHEEL : REL_HWHEEL, steps > 0 ? 1 : -1);
              emit_sync (mouse_fd);
              sleep_ms ((int) b);
            }
        }
      else if (!strcmp (cmd, "k"))
        {
          emit (kbd_fd, EV_KEY, (unsigned short) a, (int) b);
          emit_sync (kbd_fd);
          sleep_ms ((int) c);
        }
      else
        fprintf (stderr, "inject: unknown command '%s'\n", cmd);
    }

  return 0;
}

int
main (int argc, char *argv[])
{
  FILE *script;

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s SCRIPT\n", argv[0]);
      return 2;
    }

  script = fopen (argv[1], "r");
  if (!script)
    {
      fprintf (stderr, "inject: cannot read %s: %s\n", argv[1], strerror (errno));
      return 2;
    }

  mouse_fd = open_dev ("qoder-inject mouse", 0);
  kbd_fd = open_dev ("qoder-inject keyboard", 1);
  if (mouse_fd < 0 || kbd_fd < 0)
    {
      close_devs ();
      return 1;
    }

  /* Give udev and the compositor a moment to claim the new devices. */
  sleep_ms (1200);
  if (script_uses_pointer (script))
    grab_physical_pointers ();
  sleep_ms (300);
  run (script);
  fclose (script);
  sleep_ms (200);
  close_devs ();

  fprintf (stderr, "inject: done\n");
  return 0;
}
