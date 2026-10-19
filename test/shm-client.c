/* Minimal client used to validate the nested compositor: it draws a moving
 * pattern with a red border, so a stale, blank or wrongly sized frame is
 * obvious just by looking at the GTK window. With --dmabuf the same picture is
 * uploaded through a GBM buffer and zwp_linux_dmabuf_v1 instead of wl_shm. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "fractional-scale-client-protocol.h"
#include "linux-dmabuf-client-protocol.h"
#include "text-input-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define DEFAULT_WIDTH  640
#define DEFAULT_HEIGHT 400

#define MAX_PLANES 4

typedef struct {
  void *data;
  int width;
  int height;
  int stride;
  int mapped;
  struct wl_buffer *wl_buffer;
  /* dmabuf only: a bo backs the buffer and data maps its first plane. */
  struct gbm_bo *bo;
  int fd;
  uint32_t plane_offset[MAX_PLANES];
  uint32_t plane_stride[MAX_PLANES];
  int n_planes;
} Buffer;

typedef struct {
  uint32_t format;
  uint32_t pad;
  uint64_t modifier;
} DmabufEntry;   /* one record of the feedback format table, 16 bytes */

static const struct wl_buffer_listener buffer_listener;
static const struct wl_callback_listener frame_listener;
static const struct zwp_linux_buffer_params_v1_listener params_listener;

static void draw (void);
static void buffer_destroy (Buffer *buffer);
static void buffer_create_shm (Buffer *buffer, int w, int h);
static void buffer_create_all_deferred (void);
static void imports_settled (void);

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback;
static struct xdg_wm_base *wm_base;
static struct wp_viewporter *viewporter;
static struct wp_viewport *viewport;
static struct wp_fractional_scale_manager_v1 *fractional_manager;
static struct wp_fractional_scale_v1 *fractional_scale;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;

static struct gbm_device *gbm;
static int use_dmabuf;
static int dmabuf_failed;

/* Clipboard. --copy makes the client take a selection, --paste makes it ask
 * for whatever the compositor advertises, so both directions get exercised
 * without a second client. */
static struct wl_data_device_manager *data_manager;
static struct wl_data_device *data_device;
static struct wl_data_offer *selection_offer;
static char selection_mimes[8][128];
static int n_selection_mimes;
static int use_copy;
static int use_paste;
static int copy_done;
static int paste_done;
static int paste_pending;
static const char copy_text[] = "text from the nested client";
static int advertised_formats;
static int advertised_modifiers;
static char render_node[64] = "/dev/dri/renderD128";

/* Text input (zwp_text_input_v3). --text-input makes the client a field the
 * compositor can compose into: it reports surrounding text as soon as the text
 * input enters, and --text-input-password does the same for a password field,
 * where the compositor is expected to keep the text away from the engine. */
static struct zwp_text_input_manager_v3 *text_input_manager;
static struct zwp_text_input_v3 *text_input;
static int use_text_input;
static int text_input_password;
static int ti_enabled;
static int ti_commits;      /* commit requests sent, for checking done serials */
static int ti_committed;    /* commit_string events received */
static int ti_preedit_events; /* preedit_string events received */
static char field[192] = "field:";
static int field_caret;     /* byte index of the caret inside field */
static char preedit[128];
static int32_t preedit_begin, preedit_end;

/* What zwp_linux_dmabuf_feedback_v1 told us: the device the compositor wants
 * buffers on, and its (format, modifier) pairs in preference order. */
#define FEEDBACK_MAX_PAIRS 16

static struct {
  const DmabufEntry *entries;
  size_t table_size;
  size_t n_entries;
  uint64_t main_device;
  uint64_t target_device;
  uint32_t flags;
  int n_tranches;
  uint32_t fourcc[FEEDBACK_MAX_PAIRS];
  uint64_t modifier[FEEDBACK_MAX_PAIRS];
  int n_pairs;
} feedback;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;

/* Input state, mirrored into the picture by buffer_paint(). */
static double pointer_x;
static double pointer_y;
static double scroll_x;
static double scroll_y;
static int pointer_inside;
static unsigned int button_mask;
static uint32_t last_keysym;
static int motion_events;

static const struct wl_seat_listener seat_listener;
static const struct wl_pointer_listener pointer_listener;
static const struct wl_keyboard_listener keyboard_listener;
static const struct wl_data_device_listener data_device_listener;
static const struct wl_data_offer_listener data_offer_listener;
static const struct wl_data_source_listener data_source_listener;
static const struct zwp_text_input_v3_listener text_input_listener;
static void do_copy (void);
static void ensure_data_device (void);
static void ensure_text_input (void);

static void
overlay_rect (Buffer *buffer, int x, int y, int w, int h, uint32_t color)
{
  int px, py;

  for (py = y; py < y + h; py++)
    {
      uint32_t *row;

      if (py < 0 || py >= buffer->height)
        continue;

      row = (uint32_t *) buffer->data + (size_t) py * (buffer->stride / 4);

      for (px = x; px < x + w; px++)
        if (px >= 0 && px < buffer->width)
          row[px] = color;
    }
}

static Buffer buffers[2];
static int width = DEFAULT_WIDTH;
static int height = DEFAULT_HEIGHT;
/* width and height are the logical size the compositor configured; the buffer
 * has to be that many pixels times the scale the compositor prefers, and
 * wp_viewport then scales it back down to the logical size again. */
static double client_scale = 1.0;
static int device_width = DEFAULT_WIDTH;
static int device_height = DEFAULT_HEIGHT;
static int resize_pending = 0;
static int mapped = 0;
static uint32_t phase;
static uint32_t frames;
/* wl_linux_buffer_params.create is asynchronous, so a resize must wait for the
 * imports in flight: replacing a buffer before its 'created' arrives would let
 * an event from the old bo take over the new buffer's slot. */
static int pending_imports;
static int recreate_pending;

static void
update_device_size (void)
{
  device_width = (int) (width * client_scale + 0.5);
  device_height = (int) (height * client_scale + 0.5);

  if (device_width < 1)
    device_width = 1;
  if (device_height < 1)
    device_height = 1;
}

static void *
map_pool (int size, int *fd_out)
{
  int fd;
  void *data;

  fd = memfd_create ("shm-client", MFD_CLOEXEC);
  if (fd < 0)
    {
      perror ("memfd_create");
      exit (1);
    }

  if (ftruncate (fd, size) < 0)
    {
      perror ("ftruncate");
      exit (1);
    }

  data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
    {
      perror ("mmap");
      exit (1);
    }

  *fd_out = fd;
  return data;
}

static void
buffer_create_shm (Buffer *buffer, int w, int h)
{
  struct wl_shm_pool *pool;
  int fd;

  buffer->width = w;
  buffer->height = h;
  buffer->stride = w * 4;
  buffer->data = map_pool (buffer->stride * h, &fd);

  pool = wl_shm_create_pool (shm, fd, buffer->stride * h);
  buffer->wl_buffer = wl_shm_pool_create_buffer (pool, 0, w, h, buffer->stride,
                                                WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);

  wl_buffer_add_listener (buffer->wl_buffer, &buffer_listener, buffer);
}

static void
params_created (void *data, struct zwp_linux_buffer_params_v1 *params,
                struct wl_buffer *wl_buffer)
{
  Buffer *buffer = data;

  pending_imports--;
  zwp_linux_buffer_params_v1_destroy (params);

  buffer->wl_buffer = wl_buffer;
  wl_buffer_add_listener (wl_buffer, &buffer_listener, buffer);

  imports_settled ();

  if (mapped)
    draw ();
}

static void
params_failed (void *data, struct zwp_linux_buffer_params_v1 *params)
{
  Buffer *buffer = data;

  pending_imports--;
  zwp_linux_buffer_params_v1_destroy (params);

  if (!dmabuf_failed)
    {
      dmabuf_failed = 1;
      fprintf (stderr, "client: the compositor refused our dmabuf, painting shm instead\n");
    }

  buffer_destroy (buffer);
  buffer_create_shm (buffer, device_width, device_height);

  imports_settled ();

  if (mapped)
    draw ();
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
  .created = params_created,
  .failed = params_failed,
};

/* Picks the pair this client can actually paint. The CPU painter writes
 * 0xXXRRGGBB words, so only XRGB8888 is safe: an ARGB buffer would come out
 * fully transparent and an XBGR one would swap red and blue. */
static int
feedback_pick_buffer_format (void)
{
  int i;

  for (i = 0; i < feedback.n_pairs; i++)
    if (feedback.fourcc[i] == DRM_FORMAT_XRGB8888 &&
        feedback.modifier[i] == DRM_FORMAT_MOD_LINEAR)
      return i;

  return -1;
}

static void
buffer_create_dmabuf (Buffer *buffer, int w, int h)
{
  struct zwp_linux_buffer_params_v1 *params;
  struct gbm_bo *bo;
  uint64_t modifier;
  uint32_t format = DRM_FORMAT_XRGB8888;
  uint64_t wanted = DRM_FORMAT_MOD_LINEAR;
  int pick;
  int i;

  pick = feedback_pick_buffer_format ();
  if (pick >= 0)
    {
      format = feedback.fourcc[pick];
      wanted = feedback.modifier[pick];
      fprintf (stderr, "client: feedback pair %d is %c%c%c%c modifier 0x%" PRIx64 "\n",
               pick, format & 0xff, (format >> 8) & 0xff, (format >> 16) & 0xff,
               (format >> 24) & 0xff, wanted);
    }
  else if (feedback.n_pairs > 0)
    fprintf (stderr, "client: feedback has no CPU-paintable pair, asking anyway\n");

  /* LINEAR is the modifier to ask for: implicit tiling hands back a Y-tiled bo
   * a CPU cannot paint into, and this client paints on the CPU. A GPU producer
   * such as Chromium would keep the tiled one. */
  bo = gbm_bo_create_with_modifiers2 (gbm, w, h, format, &wanted, 1,
                                      GBM_BO_USE_RENDERING);
  if (!bo)
    {
      fprintf (stderr, "client: no %dx%d linear bo, painting shm instead\n", w, h);
      dmabuf_failed = 1;
      buffer_create_shm (buffer, w, h);
      return;
    }

  modifier = gbm_bo_get_modifier (bo);
  buffer->bo = bo;
  buffer->width = w;
  buffer->height = h;
  buffer->n_planes = gbm_bo_get_plane_count (bo);
  buffer->fd = gbm_bo_get_fd_for_plane (bo, 0);
  if (buffer->fd < 0)
    {
      fprintf (stderr, "client: cannot export the bo, painting shm instead\n");
      gbm_bo_destroy (bo);
      buffer->bo = NULL;
      dmabuf_failed = 1;
      buffer_create_shm (buffer, w, h);
      return;
    }

  params = zwp_linux_dmabuf_v1_create_params (dmabuf);
  for (i = 0; i < buffer->n_planes; i++)
    {
      /* libwayland duplicates every fd argument while encoding, so plane 0 can
       * keep using the fd it was exported into. */
      int fd = i == 0 ? buffer->fd : gbm_bo_get_fd_for_plane (bo, i);

      if (fd < 0)
        {
          fprintf (stderr, "client: cannot export plane %d\n", i);
          exit (1);
        }

      buffer->plane_offset[i] = gbm_bo_get_offset (bo, i);
      buffer->plane_stride[i] = gbm_bo_get_stride_for_plane (bo, i);
      zwp_linux_buffer_params_v1_add (params, fd, i, buffer->plane_offset[i],
                                      buffer->plane_stride[i],
                                      modifier >> 32, modifier & 0xffffffffu);
      if (i != 0)
        close (fd);
    }

  buffer->stride = buffer->plane_stride[0];
  buffer->data = mmap (NULL, (size_t) buffer->stride * h, PROT_READ | PROT_WRITE,
                       MAP_SHARED, buffer->fd, buffer->plane_offset[0]);
  if (buffer->data == MAP_FAILED)
    {
      fprintf (stderr, "client: cannot map the dmabuf: %s\n", strerror (errno));
      exit (1);
    }

  zwp_linux_buffer_params_v1_add_listener (params, &params_listener, buffer);
  zwp_linux_buffer_params_v1_create (params, w, h, format, 0);
  pending_imports++;

  fprintf (stderr, "client: dmabuf %dx%d fourcc=%c%c%c%c modifier=0x%" PRIx64
           " planes=%d stride=%u, painted through the prime fd\n", w, h,
           format & 0xff, (format >> 8) & 0xff, (format >> 16) & 0xff,
           (format >> 24) & 0xff, modifier,
           buffer->n_planes, buffer->plane_stride[0]);
}

static void
buffer_create (Buffer *buffer, int w, int h)
{
  if (use_dmabuf && !dmabuf_failed)
    buffer_create_dmabuf (buffer, w, h);
  else
    buffer_create_shm (buffer, w, h);
}

static void
buffer_destroy (Buffer *buffer)
{
  if (buffer->data)
    munmap (buffer->data, (size_t) buffer->stride * buffer->height);

  if (buffer->wl_buffer)
    wl_buffer_destroy (buffer->wl_buffer);

  if (buffer->bo)
    {
      close (buffer->fd);
      gbm_bo_destroy (buffer->bo);
    }

  memset (buffer, 0, sizeof *buffer);
}

static void
buffer_create_all (void)
{
  int i;

  for (i = 0; i < 2; i++)
    {
      buffer_destroy (&buffers[i]);
      buffer_create (&buffers[i], device_width, device_height);
    }
}

/* Called once an import settles: build the buffers the compositor asked for,
 * unless an import is still on its way. */
static void
imports_settled (void)
{
  if (pending_imports > 0)
    return;

  if (recreate_pending)
    {
      recreate_pending = 0;
      buffer_create_all ();
    }
}

static void
buffer_create_all_deferred (void)
{
  if (pending_imports > 0)
    {
      recreate_pending = 1;
      return;
    }

  buffer_create_all ();
}

static void
buffer_paint (Buffer *buffer, uint32_t step)
{
  uint32_t *pixels = buffer->data;
  uint32_t edge = button_mask ? 0x0020ff20u : 0x00ff2020u; /* green while held */
  int x, y;

  for (y = 0; y < buffer->height; y++)
    {
      uint32_t *row = pixels + (size_t) y * (buffer->stride / 4);

      for (x = 0; x < buffer->width; x++)
        {
          if (x < 4 || y < 4 || x >= buffer->width - 4 || y >= buffer->height - 4)
            row[x] = edge; /* red edge: the exact client size */
          else
            row[x] = 0x00000000u |
                     (((x * 3 + step) & 0xff) << 16) |
                     (((y * 3 + step) & 0xff) << 8) |
                     (((x + y + step * 2) & 0xff));
        }
    }

  /* Input overlay, so a screenshot shows what the nested seat delivered:
   * a crosshair at the pointer, a bar for the scroll offset and a marker for
   * the last key. */
  if (pointer_inside)
    {
      /* Seat coordinates are logical, the picture is drawn in device pixels. */
      int cx = (int) (pointer_x * client_scale);
      int cy = (int) (pointer_y * client_scale);

      overlay_rect (buffer, cx - 40, cy - 2, 80, 4, 0x00ffff00u);
      overlay_rect (buffer, cx - 2, cy - 40, 4, 80, 0x00ffff00u);
    }

  /* Scrolled distance as a blue bar, last key as a magenta one. */
  {
    int scrolled = (int) (scroll_y < 0 ? -scroll_y : scroll_y);
    int pressed = (int) (last_keysym % (uint32_t) (buffer->width / 2));

    overlay_rect (buffer, 8, buffer->height - 40,
                  scrolled % (buffer->width / 2) + 1, 12, 0x002040ffu);
    overlay_rect (buffer, 8, buffer->height - 60, pressed + 1, 12, 0x00ff00ffu);
  }

  /* Composition history, so even the last frame shows what arrived: a yellow
   * block per preedit event and a white one per commit. */
  {
    int i;

    for (i = 0; i < ti_preedit_events && i < 16; i++)
      overlay_rect (buffer, 8 + i * 16, buffer->height - 100, 12, 12, 0x0000ffffu);
    for (i = 0; i < ti_committed && i < 16; i++)
      overlay_rect (buffer, 8 + i * 16, buffer->height - 120, 12, 12, 0x00ffffffu);
  }
}

static void
draw (void)
{
  struct wl_callback *callback;
  Buffer *buffer = NULL;
  int i;

  for (i = 0; i < 2; i++)
    {
      if (!buffers[i].mapped && buffers[i].wl_buffer)
        {
          buffer = &buffers[i];
          break;
        }
    }

  if (!buffer)
    return;

  buffer_paint (buffer, phase);

  wl_surface_attach (surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage_buffer (surface, 0, 0, buffer->width, buffer->height);
  /* The buffer is device pixels; the surface stays logical, so the compositor
   * knows how big we are regardless of the scale we rendered at. */
  if (viewport)
    wp_viewport_set_destination (viewport, width, height);
  wl_surface_commit (surface);
  buffer->mapped = 1;

  callback = wl_surface_frame (surface);
  wl_callback_add_listener (callback, &frame_listener, NULL);

  phase += 7;
  frames++;

  if ((frames % 60) == 1)
    fprintf (stderr, "client: frame %u at %dx%d logical, %dx%d buffer (scale %.4f)\n",
             frames, width, height, device_width, device_height, client_scale);
}

static void
buffer_release (void *data, struct wl_buffer *wl_buffer)
{
  Buffer *buffer = data;

  if (buffer->wl_buffer == wl_buffer)
    buffer->mapped = 0;
}

static const struct wl_buffer_listener buffer_listener = {
  .release = buffer_release,
};

static void
frame_done (void *data, struct wl_callback *callback, uint32_t time)
{
  wl_callback_destroy (callback);

  if (resize_pending)
    {
      resize_pending = 0;
      buffer_create_all_deferred ();
    }

  /* Copy once the picture is moving: the seat may not have advertised its
   * capabilities until after the first frame, so ask here rather than at
   * startup. */
  if (use_copy && !copy_done && frames > 3)
    {
      ensure_data_device ();
      if (data_device)
        {
          do_copy ();
          copy_done = 1;
        }
      else
        fprintf (stderr, "client: no data device, cannot copy\n");
    }

  if (mapped)
    draw ();
}

static const struct wl_callback_listener frame_listener = {
  .done = frame_done,
};

static void
toplevel_configure (void *data, struct xdg_toplevel *toplevel,
                    int32_t w, int32_t h, struct wl_array *states)
{
  if (w > 0 && h > 0 && (w != width || h != height))
    {
      width = w;
      height = h;
      update_device_size ();
      resize_pending = 1;
    }

  fprintf (stderr, "client: toplevel configure %dx%d\n", w, h);
}

static void
toplevel_close (void *data, struct xdg_toplevel *toplevel)
{
  fprintf (stderr, "client: close requested\n");
  exit (0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
  .configure = toplevel_configure,
  .close = toplevel_close,
};

static void
xdg_surface_configure (void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
  xdg_surface_ack_configure (xdg_surface, serial);

  if (!mapped)
    {
      mapped = 1;
      resize_pending = 0;
      buffer_create_all_deferred ();
    }
  else if (resize_pending)
    {
      resize_pending = 0;
      buffer_create_all_deferred ();
    }

  draw ();
}

static const struct xdg_surface_listener xdg_surface_listener = {
  .configure = xdg_surface_configure,
};

static void
wm_base_ping (void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
  xdg_wm_base_pong (wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
  .ping = wm_base_ping,
};

static void
surface_enter (void *data, struct wl_surface *surface, struct wl_output *output)
{
  fprintf (stderr, "client: entered an output\n");
}

static void
surface_leave (void *data, struct wl_surface *surface, struct wl_output *output)
{
  fprintf (stderr, "client: left an output\n");
}

static const struct wl_surface_listener surface_listener = {
  .enter = surface_enter,
  .leave = surface_leave,
};

/* ------------------------------------------------------------------ *
 * wl_seat                                                            *
 * ------------------------------------------------------------------ */

static char seat_name[64] = "(none)";

static void
pointer_enter (void *data, struct wl_pointer *wl_pointer, uint32_t serial,
               struct wl_surface *wl_surface, wl_fixed_t sx, wl_fixed_t sy)
{
  pointer_inside = 1;
  pointer_x = wl_fixed_to_double (sx);
  pointer_y = wl_fixed_to_double (sy);

  fprintf (stderr, "client: pointer enter serial=%u at %.1f,%.1f\n",
           serial, pointer_x, pointer_y);
}

static void
pointer_leave (void *data, struct wl_pointer *wl_pointer, uint32_t serial,
               struct wl_surface *wl_surface)
{
  pointer_inside = 0;

  fprintf (stderr, "client: pointer leave serial=%u\n", serial);
}

static void
pointer_motion (void *data, struct wl_pointer *wl_pointer, uint32_t time,
                wl_fixed_t sx, wl_fixed_t sy)
{
  pointer_x = wl_fixed_to_double (sx);
  pointer_y = wl_fixed_to_double (sy);

  if (++motion_events % 60 == 1)
    fprintf (stderr, "client: motion #%d at %.1f,%.1f\n", motion_events,
             pointer_x, pointer_y);
}

static void
pointer_button (void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                uint32_t time, uint32_t button, uint32_t state)
{
  unsigned int bit = button >= 0x110 && button - 0x110 < 32 ? 1u << (button - 0x110) : 0;

  if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    button_mask |= bit;
  else
    button_mask &= ~bit;

  fprintf (stderr, "client: button %u %s serial=%u mask=0x%x\n",
           button, state == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up",
           serial, button_mask);
}

static void
pointer_axis (void *data, struct wl_pointer *wl_pointer, uint32_t time,
              uint32_t axis, wl_fixed_t value)
{
  double v = wl_fixed_to_double (value);

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
    scroll_y += v;
  else
    scroll_x += v;

  fprintf (stderr, "client: axis %u %+.2f (total %+.1f)\n", axis, v,
           axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? scroll_y : scroll_x);
}

static void
pointer_frame (void *data, struct wl_pointer *wl_pointer)
{
  if (mapped)
    draw ();
}

static void
pointer_axis_source (void *data, struct wl_pointer *wl_pointer, uint32_t axis_source)
{
}

static void
pointer_axis_stop (void *data, struct wl_pointer *wl_pointer, uint32_t time,
                   uint32_t axis)
{
}

static void
pointer_axis_discrete (void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                       int32_t discrete)
{
  fprintf (stderr, "client: axis %d discrete %+d\n", axis, discrete);
}

static const struct wl_pointer_listener pointer_listener = {
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = pointer_motion,
  .button = pointer_button,
  .axis = pointer_axis,
  .frame = pointer_frame,
  .axis_source = pointer_axis_source,
  .axis_stop = pointer_axis_stop,
  .axis_discrete = pointer_axis_discrete,
};

static void
keyboard_keymap (void *data, struct wl_keyboard *wl_keyboard, uint32_t format,
                 int32_t fd, uint32_t size)
{
  void *map;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
      fprintf (stderr, "client: unsupported keymap format %u\n", format);
      close (fd);
      return;
    }

  map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close (fd);
  if (map == MAP_FAILED)
    {
      perror ("client: keymap mmap");
      return;
    }

  xkb_ctx = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  xkb_keymap = xkb_keymap_new_from_string (xkb_ctx, map,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap (map, size);
  xkb_state = xkb_state_new (xkb_keymap);

  fprintf (stderr, "client: keymap %u bytes, %s\n", size,
           xkb_keymap ? "compiled" : "FAILED to compile");
}

static void
keyboard_enter (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                struct wl_surface *wl_surface, struct wl_array *keys)
{
  fprintf (stderr, "client: keyboard enter serial=%u, %zu keys held\n",
           serial, keys->size / sizeof (uint32_t));
}

static void
keyboard_leave (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                struct wl_surface *wl_surface)
{
  fprintf (stderr, "client: keyboard leave serial=%u\n", serial);
}

static void
keyboard_key (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
              uint32_t time, uint32_t key, uint32_t state)
{
  char name[64] = "(none)";
  xkb_keysym_t sym = 0;

  if (xkb_state)
    {
      if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        xkb_state_update_key (xkb_state, key + 8, XKB_KEY_DOWN);
      sym = xkb_state_key_get_one_sym (xkb_state, key + 8);
      if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        xkb_state_update_key (xkb_state, key + 8, XKB_KEY_UP);
      xkb_keysym_get_name (sym, name, sizeof name);
    }

  last_keysym = sym;

  fprintf (stderr, "client: key %s evdev=%u keysym=%s mods=0x%x\n",
           state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up", key, name,
           xkb_state ? xkb_state_serialize_mods (xkb_state, XKB_STATE_MODS_DEPRESSED) : 0u);

  if (mapped)
    draw ();
}

static void
keyboard_modifiers (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                    uint32_t depressed, uint32_t latched, uint32_t locked,
                    uint32_t group)
{
  fprintf (stderr, "client: modifiers serial=%u depressed=0x%x locked=0x%x group=%u\n",
           serial, depressed, locked, group);
}

static void
keyboard_repeat_info (void *data, struct wl_keyboard *wl_keyboard, int32_t rate,
                      int32_t delay)
{
  fprintf (stderr, "client: repeat rate=%d delay=%d\n", rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
  .keymap = keyboard_keymap,
  .enter = keyboard_enter,
  .leave = keyboard_leave,
  .key = keyboard_key,
  .modifiers = keyboard_modifiers,
  .repeat_info = keyboard_repeat_info,
};

/* ------------------------------------------------------------------ *
 * Text input                                                         *
 *                                                                    *
 * The compositor is the input-method host, so this side only answers: *
 * report the field when the text input enters, then apply whatever    *
 * the engine turns the keystrokes into.                              *
 * ------------------------------------------------------------------ */

static void
field_insert (const char *text)
{
  size_t len = strlen (text);
  size_t cur = strlen (field);

  if (field_caret < 0 || (size_t) field_caret > cur)
    field_caret = (int) cur;
  if (cur + len >= sizeof field)
    len = sizeof field - cur - 1;
  if (len == 0)
    return;

  memmove (field + field_caret + len, field + field_caret, cur - field_caret + 1);
  memcpy (field + field_caret, text, len);
  field_caret += (int) len;
}

static void
text_input_report (void)
{
  if (!text_input || !ti_enabled)
    return;

  zwp_text_input_v3_set_surrounding_text (text_input, field, field_caret, field_caret);
  zwp_text_input_v3_set_text_change_cause (text_input,
                                           ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
  /* Where the engine should put its candidate window: surface-local logical
   * pixels, so the same space the pointer moves in. */
  zwp_text_input_v3_set_cursor_rectangle (text_input, 20, 30, 200, 24);
  zwp_text_input_v3_commit (text_input);
  ti_commits++;

  fprintf (stderr, "client: reported '%s' caret=%d (commit #%d)\n",
           field, field_caret, ti_commits);
}

static void
text_input_enter (void *data, struct zwp_text_input_v3 *input,
                  struct wl_surface *wl_surface)
{
  fprintf (stderr, "client: text input enter%s\n",
           text_input_password ? " (password)" : "");

  ti_enabled = 1;
  zwp_text_input_v3_enable (input);
  /* hint before purpose, as the request spells them. */
  zwp_text_input_v3_set_content_type (input,
      text_input_password ? ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT
                          : ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
      text_input_password ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD
                          : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
  text_input_report ();
}

static void
text_input_leave (void *data, struct zwp_text_input_v3 *input,
                  struct wl_surface *wl_surface)
{
  ti_enabled = 0;
  preedit[0] = '\0';

  fprintf (stderr, "client: text input leave\n");

  if (mapped)
    draw ();
}

static void
text_input_preedit_string (void *data, struct zwp_text_input_v3 *input,
                           const char *text, int32_t cursor_begin,
                           int32_t cursor_end)
{
  snprintf (preedit, sizeof preedit, "%s", text);
  preedit_begin = cursor_begin;
  preedit_end = cursor_end;
  ti_preedit_events++;

  fprintf (stderr, "client: preedit '%s' caret [%d,%d]\n", text,
           cursor_begin, cursor_end);

  if (mapped)
    draw ();
}

static void
text_input_commit_string (void *data, struct zwp_text_input_v3 *input, const char *text)
{
  ti_committed++;
  preedit[0] = '\0';
  field_insert (text);

  fprintf (stderr, "client: commit_string '%s' -> field '%s' caret=%d\n",
           text, field, field_caret);

  text_input_report ();

  if (mapped)
    draw ();
}

static void
text_input_delete_surrounding (void *data, struct zwp_text_input_v3 *input,
                               uint32_t before_length, uint32_t after_length)
{
  int start = field_caret - (int) before_length;
  size_t cur = strlen (field);

  if (start < 0 || (size_t) start + before_length + after_length > cur)
    {
      fprintf (stderr, "client: delete_surrounding_text (%u,%u) outside the field\n",
               before_length, after_length);
      return;
    }

  memmove (field + start, field + start + before_length + after_length,
           cur - (start + before_length + after_length) + 1);
  field_caret = start;

  fprintf (stderr, "client: delete_surrounding_text before=%u after=%u -> '%s'\n",
           before_length, after_length, field);

  text_input_report ();

  if (mapped)
    draw ();
}

static void
text_input_done (void *data, struct zwp_text_input_v3 *input, uint32_t serial)
{
  /* The serial counts this client's commit requests, which is the only way to
   * match a reply to the state it answers. */
  fprintf (stderr, "client: text input done serial=%u, %d commits sent\n",
           serial, ti_commits);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
  .enter = text_input_enter,
  .leave = text_input_leave,
  .preedit_string = text_input_preedit_string,
  .commit_string = text_input_commit_string,
  .delete_surrounding_text = text_input_delete_surrounding,
  .done = text_input_done,
};

static void
ensure_text_input (void)
{
  if (!use_text_input || !text_input_manager || !seat || text_input)
    return;

  text_input = zwp_text_input_manager_v3_get_text_input (text_input_manager, seat);
  zwp_text_input_v3_add_listener (text_input, &text_input_listener, NULL);
  fprintf (stderr, "client: text input created\n");
}

static void
seat_capabilities (void *data, struct wl_seat *wl_seat, uint32_t caps)
{
  fprintf (stderr, "client: seat %s capabilities=0x%x\n", seat_name, caps);

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer)
    {
      pointer = wl_seat_get_pointer (wl_seat);
      wl_pointer_add_listener (pointer, &pointer_listener, NULL);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer)
    {
      wl_pointer_destroy (pointer);
      pointer = NULL;
    }

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard)
    {
      keyboard = wl_seat_get_keyboard (wl_seat);
      wl_keyboard_add_listener (keyboard, &keyboard_listener, NULL);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard)
    {
      wl_keyboard_destroy (keyboard);
      keyboard = NULL;
    }

  ensure_data_device ();
  ensure_text_input ();
}

static void
ensure_data_device (void)
{
  if (!data_manager || !seat || data_device)
    return;

  data_device = wl_data_device_manager_get_data_device (data_manager, seat);
  wl_data_device_add_listener (data_device, &data_device_listener, NULL);
  fprintf (stderr, "client: %d: data device created\n", getpid());
}

static void
data_offer_mime (void *data, struct wl_data_offer *offer, const char *mime_type)
{
  if (n_selection_mimes < (int) (sizeof selection_mimes / sizeof selection_mimes[0]))
    {
      snprintf (selection_mimes[n_selection_mimes],
                sizeof selection_mimes[0], "%s", mime_type);
      n_selection_mimes++;
      fprintf (stderr, "client: selection offers %s\n", mime_type);
    }
}

static void
data_offer_source_actions (void *data, struct wl_data_offer *offer, uint32_t actions)
{
}

static void
data_offer_action (void *data, struct wl_data_offer *offer, uint32_t action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
  .offer = data_offer_mime,
  .source_actions = data_offer_source_actions,
  .action = data_offer_action,
};

static void
do_paste (void)
{
  char buf[4096];
  size_t len = 0;
  ssize_t n;
  int fds[2];

  if (paste_done || n_selection_mimes == 0 || !selection_offer)
    return;

  paste_done = 1;

  if (pipe (fds) != 0)
    return;

  /* Blocking reads are safe here: the compositor answers from a thread, so it
   * keeps writing while this client is not dispatching. */
  wl_data_offer_receive (selection_offer, selection_mimes[0], fds[1]);

  /* Nothing reaches the compositor until the socket takes it, and the answer
   * only starts after that, so the queue has to drain before our read end
   * loses its last writer. */
  while (wl_display_flush (display) < 0 && errno == EAGAIN)
    {
      struct pollfd writable = { .fd = wl_display_get_fd (display),
                                 .events = POLLOUT };

      if (poll (&writable, 1, 2000) <= 0)
        break;
    }

  close (fds[1]);

  while ((n = read (fds[0], buf + len, sizeof buf - 1 - len)) > 0)
    len += (size_t) n;
  close (fds[0]);

  buf[len] = '\0';
  fprintf (stderr, "client: %d: pasted %zu bytes of %s: '%s'\n",
           getpid(), len, selection_mimes[0], buf);
}

static void
data_device_selection (void *data, struct wl_data_device *device,
                       struct wl_data_offer *offer)
{
  /* The protocol makes the receiver retire the previous selection offer. The
   * mime list for the new one already arrived, right after it was introduced. */
  if (selection_offer && selection_offer != offer)
    wl_data_offer_destroy (selection_offer);

  selection_offer = offer;

  if (!offer)
    {
      fprintf (stderr, "client: selection cleared\n");
      return;
    }

  /* Every new selection gets read once, so a second copy from another client
   * shows up in the log too. */
  paste_done = 0;
  if (use_paste)
    paste_pending = 1;
}

static void
data_device_data_offer (void *data, struct wl_data_device *device,
                        struct wl_data_offer *offer)
{
  /* Both drags and selections are introduced here, so the offer only becomes
   * interesting once selection or enter names it; the listener has to be in
   * place before its mime list arrives. */
  n_selection_mimes = 0;
  wl_data_offer_add_listener (offer, &data_offer_listener, NULL);
}

static const struct wl_data_device_listener data_device_listener = {
  .data_offer = data_device_data_offer,
  .selection = data_device_selection,
};

static void
data_source_send (void *data, struct wl_data_source *source,
                  const char *mime_type, int fd)
{
  const char *payload = copy_text;
  size_t left = strlen (copy_text);
  size_t offset = 0;

  if (strcmp (mime_type, "text/plain;charset=utf-8") != 0)
    payload = NULL;

  fprintf (stderr, "client: %d: asked for %s, answering with %s\n",
           getpid(), mime_type, payload ? "the copy" : "nothing");

  while (payload && offset < left)
    {
      ssize_t n = write (fd, payload + offset, left - offset);

      if (n <= 0)
        break;
      offset += (size_t) n;
    }

  close (fd);
}

static void
data_source_cancelled (void *data, struct wl_data_source *source)
{
  fprintf (stderr, "client: our selection was superseded\n");
  wl_data_source_destroy (source);
}

static const struct wl_data_source_listener data_source_listener = {
  .send = data_source_send,
  .cancelled = data_source_cancelled,
};

static void
do_copy (void)
{
  struct wl_data_source *source;

  source = wl_data_device_manager_create_data_source (data_manager);
  wl_data_source_offer (source, "text/plain;charset=utf-8");
  wl_data_source_add_listener (source, &data_source_listener, source);
  wl_data_device_set_selection (data_device, source, 0);
  fprintf (stderr, "client: %d: took the selection: '%s'\n", getpid(), copy_text);
}

static void
seat_name_listener (void *data, struct wl_seat *wl_seat, const char *name)
{
  snprintf (seat_name, sizeof seat_name, "%s", name);
}

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_capabilities,
  .name = seat_name_listener,
};

static void
dmabuf_format (void *data, struct zwp_linux_dmabuf_v1 *dmabuf_obj, uint32_t format)
{
  advertised_formats++;
}

static void
dmabuf_modifier (void *data, struct zwp_linux_dmabuf_v1 *dmabuf_obj, uint32_t format,
                 uint32_t modifier_hi, uint32_t modifier_lo)
{
  uint64_t modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;

  advertised_modifiers++;

  fprintf (stderr, "client: dmabuf format %c%c%c%c modifier 0x%" PRIx64 "\n",
           format & 0xff, (format >> 8) & 0xff, (format >> 16) & 0xff,
           (format >> 24) & 0xff, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
  .format = dmabuf_format,
  .modifier = dmabuf_modifier,
};

static uint64_t
array_to_device (struct wl_array *device)
{
  uint64_t dev = 0;

  if (device->size >= sizeof dev)
    memcpy (&dev, device->data, sizeof dev);

  return dev;
}

static void
feedback_format_table (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj,
                       int fd, uint32_t size)
{
  void *table;

  /* MAP_PRIVATE, as the protocol requires; closing the fd leaves the map. */
  table = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close (fd);

  if (table == MAP_FAILED)
    {
      fprintf (stderr, "client: cannot map the feedback format table: %s\n",
               strerror (errno));
      return;
    }

  if (feedback.entries)
    munmap ((void *) feedback.entries, feedback.table_size);

  /* A table always starts a fresh set of parameters, on this or another
   * feedback object: drop what the previous one advertised. */
  feedback.n_pairs = 0;
  feedback.n_tranches = 0;

  feedback.entries = table;
  feedback.table_size = size;
  feedback.n_entries = size / sizeof (DmabufEntry);
  fprintf (stderr, "client: feedback format table: %zu pairs\n", feedback.n_entries);
}

static void
feedback_main_device (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj,
                      struct wl_array *device)
{
  feedback.main_device = array_to_device (device);
  fprintf (stderr, "client: feedback main device %u:%u\n",
           major ((dev_t) feedback.main_device), minor ((dev_t) feedback.main_device));
}

static void
feedback_tranche_target_device (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj,
                                struct wl_array *device)
{
  feedback.target_device = array_to_device (device);
}

static void
feedback_tranche_flags (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj,
                        uint32_t flags)
{
  feedback.flags = flags;
}

static void
feedback_tranche_formats (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj,
                          struct wl_array *indices)
{
  const uint16_t *index = indices->data;
  size_t n = indices->size / sizeof (uint16_t);
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (!feedback.entries || index[i] >= feedback.n_entries)
        {
          fprintf (stderr, "client: feedback index %u is not in the table\n", index[i]);
          continue;
        }

      if (feedback.n_pairs < FEEDBACK_MAX_PAIRS)
        {
          feedback.fourcc[feedback.n_pairs] = feedback.entries[index[i]].format;
          feedback.modifier[feedback.n_pairs] = feedback.entries[index[i]].modifier;
          feedback.n_pairs++;
        }
    }

  fprintf (stderr, "client: feedback tranche: %zu indices, flags=0x%x, target=%u:%u\n",
           n, feedback.flags,
           major ((dev_t) feedback.target_device), minor ((dev_t) feedback.target_device));
}

static void
feedback_tranche_done (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj)
{
  feedback.n_tranches++;
  fprintf (stderr, "client: feedback tranche %d done\n", feedback.n_tranches);
}

static void
feedback_done (void *data, struct zwp_linux_dmabuf_feedback_v1 *obj)
{
  fprintf (stderr, "client: feedback done: %d tranches, %d usable pairs\n",
           feedback.n_tranches, feedback.n_pairs);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
  .done = feedback_done,
  .format_table = feedback_format_table,
  .main_device = feedback_main_device,
  .tranche_done = feedback_tranche_done,
  .tranche_target_device = feedback_tranche_target_device,
  .tranche_formats = feedback_tranche_formats,
  .tranche_flags = feedback_tranche_flags,
};

static void
fractional_scale_preferred (void *data, struct wp_fractional_scale_v1 *scale,
                           uint32_t preferred)
{
  /* The event counts in 120ths, so 150 is a scale of 1.25. */
  double s = (double) preferred / 120.0;

  fprintf (stderr, "client: preferred_scale %u -> %.4f (was %.4f)\n",
           preferred, s, client_scale);

  if (s == client_scale)
    return;

  client_scale = s;
  update_device_size ();
  resize_pending = 1;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
  .preferred_scale = fractional_scale_preferred,
};

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
                 const char *interface, uint32_t version)
{
  if (!strcmp (interface, "wl_compositor"))
    compositor = wl_registry_bind (registry, name, &wl_compositor_interface,
                                   version < 4 ? version : 4);
  else if (!strcmp (interface, "wl_shm"))
    shm = wl_registry_bind (registry, name, &wl_shm_interface, 1);
  else if (!strcmp (interface, "wp_viewporter"))
    viewporter = wl_registry_bind (registry, name, &wp_viewporter_interface,
                                   version < 1 ? version : 1);
  else if (!strcmp (interface, "wp_fractional_scale_manager_v1"))
    fractional_manager = wl_registry_bind (registry, name,
                                           &wp_fractional_scale_manager_v1_interface,
                                           version < 1 ? version : 1);
  else if (!strcmp (interface, "zwp_linux_dmabuf_v1"))
    {
      uint32_t bind = version < 5 ? version : 5;

      dmabuf = wl_registry_bind (registry, name, &zwp_linux_dmabuf_v1_interface, bind);
      zwp_linux_dmabuf_v1_add_listener (dmabuf, &dmabuf_listener, NULL);

      /* Version 4 is where the legacy format/modifier events were dropped in
       * favour of per-device feedback. */
      if (bind >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)
        {
          dmabuf_feedback = zwp_linux_dmabuf_v1_get_default_feedback (dmabuf);
          zwp_linux_dmabuf_feedback_v1_add_listener (dmabuf_feedback,
                                                     &feedback_listener, NULL);
        }
    }
  else if (!strcmp (interface, "wl_data_device_manager"))
    {
      /* Version 3 is where data_offer got source_actions/action; 1 is enough
       * for the copy and paste this client does. */
      data_manager = wl_registry_bind (registry, name,
                                       &wl_data_device_manager_interface,
                                       version < 3 ? version : 3);
      ensure_data_device ();
    }
  else if (!strcmp (interface, "zwp_text_input_manager_v3"))
    {
      text_input_manager = wl_registry_bind (registry, name,
                                             &zwp_text_input_manager_v3_interface,
                                             version < 1 ? version : 1);
      ensure_text_input ();
    }
  else if (!strcmp (interface, "wl_seat"))
    {
      seat = wl_registry_bind (registry, name, &wl_seat_interface,
                               version < 5 ? version : 5);
      wl_seat_add_listener (seat, &seat_listener, NULL);
    }
  else if (!strcmp (interface, "xdg_wm_base"))
    {
      wm_base = wl_registry_bind (registry, name, &xdg_wm_base_interface,
                                  version < 3 ? version : 3);
      xdg_wm_base_add_listener (wm_base, &wm_base_listener, NULL);
    }
}

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

int
main (int argc, char *argv[])
{
  struct wl_registry *registry;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--dmabuf"))
        use_dmabuf = 1;
      else if (!strcmp (argv[i], "--copy"))
        use_copy = 1;
      else if (!strcmp (argv[i], "--paste"))
        use_paste = 1;
      else if (!strcmp (argv[i], "--text-input"))
        use_text_input = 1;
      else if (!strcmp (argv[i], "--text-input-password"))
        {
          use_text_input = 1;
          text_input_password = 1;
        }
      else if (!strncmp (argv[i], "--render-node=", 14))
        snprintf (render_node, sizeof render_node, "%s", argv[i] + 14);
      else
        fprintf (stderr, "client: ignoring unknown argument '%s'\n", argv[i]);
    }

  field_caret = (int) strlen (field);

  display = wl_display_connect (NULL);
  if (!display)
    {
      fprintf (stderr, "client: cannot connect to WAYLAND_DISPLAY=%s\n",
               getenv ("WAYLAND_DISPLAY") ? getenv ("WAYLAND_DISPLAY") : "(unset)");
      return 1;
    }

  registry = wl_display_get_registry (display);
  wl_registry_add_listener (registry, &registry_listener, NULL);
  wl_display_roundtrip (display);

  if (!compositor || !shm || !wm_base)
    {
      fprintf (stderr, "client: missing globals compositor=%p shm=%p xdg=%p\n",
               (void *) compositor, (void *) shm, (void *) wm_base);
      return 1;
    }

  if (!seat)
    fprintf (stderr, "client: no wl_seat, input cannot be tested\n");

  if (use_dmabuf)
    {
      if (!dmabuf)
        fprintf (stderr, "client: no zwp_linux_dmabuf_v1, painting shm\n");
      else
        {
          int fd = open (render_node, O_RDWR | O_CLOEXEC);

          if (fd < 0)
            fprintf (stderr, "client: cannot open %s: %s, painting shm\n",
                     render_node, strerror (errno));
          else
            gbm = gbm_create_device (fd); /* owns the fd, success or not */
        }

      if (!gbm)
        use_dmabuf = 0;

      /* A second roundtrip: the events the compositor sends in reply to our
       * binds are queued behind the registry ones, so nothing is parsed yet. */
      wl_display_roundtrip (display);

      /* Feedback lands with those events, and allocating on a device other than
       * the preferred one is the hybrid-GPU bug this protocol exists to catch. */
      if (gbm && feedback.n_pairs > 0)
        {
          struct stat st;

          if (fstat (gbm_device_get_fd (gbm), &st) == 0)
            fprintf (stderr, "client: allocated on %u:%u, feedback prefers %u:%u — %s\n",
                     major (st.st_rdev), minor (st.st_rdev),
                     major ((dev_t) feedback.main_device),
                     minor ((dev_t) feedback.main_device),
                     (uint64_t) st.st_rdev == feedback.main_device ? "match" : "MISMATCH");
        }
      else if (dmabuf && !feedback.n_pairs)
        fprintf (stderr, "client: no dmabuf feedback received\n");
    }

  fprintf (stderr, "client: painting %s buffers, %d dmabuf formats and %d modifiers "
                   "advertised, render node %s\n", use_dmabuf ? "dmabuf" : "shm",
           advertised_formats, advertised_modifiers, render_node);
  fprintf (stderr, "client: viewporter=%p fractional_scale_manager=%p data_manager=%p\n",
           (void *) viewporter, (void *) fractional_manager, (void *) data_manager);
  if (!data_manager && (use_copy || use_paste))
    fprintf (stderr, "client: no wl_data_device_manager, clipboard test impossible\n");
  if (use_text_input && !text_input_manager)
    fprintf (stderr, "client: no zwp_text_input_manager_v3, composition impossible\n");

  /* Either global could have arrived after the other, and both have to be in
   * hand before a text input can be created. */
  ensure_text_input ();

  surface = wl_compositor_create_surface (compositor);
  wl_surface_add_listener (surface, &surface_listener, NULL);

  /* A nested compositor has one destination, so the per-surface answer is the
   * default one — but CEF asks, so make sure the request is served. */
  if (dmabuf &&
      zwp_linux_dmabuf_v1_get_version (dmabuf) >=
        ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION)
    {
      struct zwp_linux_dmabuf_feedback_v1 *per_surface =
        zwp_linux_dmabuf_v1_get_surface_feedback (dmabuf, surface);

      zwp_linux_dmabuf_feedback_v1_add_listener (per_surface, &feedback_listener, NULL);
    }

  if (viewporter)
    viewport = wp_viewporter_get_viewport (viewporter, surface);
  if (fractional_manager)
    {
      fractional_scale =
        wp_fractional_scale_manager_v1_get_fractional_scale (fractional_manager, surface);
      wp_fractional_scale_v1_add_listener (fractional_scale,
                                           &fractional_scale_listener, NULL);
    }
  update_device_size ();

  xdg_surface = xdg_wm_base_get_xdg_surface (wm_base, surface);
  xdg_surface_add_listener (xdg_surface, &xdg_surface_listener, NULL);

  toplevel = xdg_surface_get_toplevel (xdg_surface);
  xdg_toplevel_add_listener (toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_title (toplevel, "shm test client");
  xdg_toplevel_set_app_id (toplevel, "nested-shm-test");

  wl_surface_commit (surface);

  while (wl_display_dispatch (display) >= 0)
    {
      /* Dispatching may have just brought the mime list for a new selection;
       * the ask happens out here, not from inside the listener. */
      if (paste_pending && n_selection_mimes > 0)
        {
          paste_pending = 0;
          do_paste ();
        }
    }

  fprintf (stderr, "client: connection closed (%u frames)\n", frames);
  return 0;
}
