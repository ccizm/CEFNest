/* Minimal client used to validate the nested compositor: it draws a moving
 * pattern with a red border, so a stale, blank or wrongly sized frame is
 * obvious just by looking at the GTK window. With --dmabuf the same picture is
 * uploaded through a GBM buffer and zwp_linux_dmabuf_v1 instead of wl_shm.
 * With --popup it also opens a menu and a submenu over itself, so placement,
 * hit-testing and dismissal can be checked against the rules it chose. With
 * --dnd-requests it sends the three wl_data_device_manager version 3 requests
 * that abort a compositor whose implementation slots are empty — see
 * docs/empty-request-slot.md. With --drag-out it starts a drag from the left
 * button press and offers the mime list a real page's text drag was measured
 * with, so the handover of a drag that leaves the window has a source to read
 * without a browser. With --drag-in it is the other half: the destination of a
 * drag the host carries in, which accepts a type, reads the payload twice the
 * way a page was measured to, and exits non-zero if the bytes are not the ones
 * it was handed. With --second-window it maps a second toplevel and drops the
 * test pattern in both: whichever window the seat has the keyboard in paints
 * red, the other green, so the routing of an input between two surfaces of one
 * client is readable off the frames the compositor imported. With
 * --wrong-object it names, from connections of its own, a
 * zwp_primary_selection_source_v1 and a wl_surface where the protocol wants a
 * wl_data_source, and fails the run if the compositor takes either: an object
 * id is a struct the compositor will go on reading through the wrong fields.
 * With --pool-shrink it cuts its own shm pool file down after the compositor
 * has mapped it and commits a second buffer from the same pool: reading the old
 * mapping is a SIGBUS that takes the whole host, so a compositor that has the
 * pool mapped and the file gone has to be the one that hangs up. With
 * --empty-copy it takes the clipboard and the primary selection and answers every
 * read of them with a closed descriptor and no bytes: copying nothing is a legal
 * selection, and a compositor that builds a string out of the payload has to
 * survive being handed an empty one rather than dereferencing the NULL it got.
 * With --popup-depth it hangs a popup on a popup on a popup, one roundtrip at a
 * time, from a connection of its own, and fails the run unless the compositor
 * refuses within the depth its own tree walks are built for: the nesting is
 * chosen by the client but paid for by the host's C stack. With
 * --drag-fake-serial it asks for a drag citing a serial that never came from a
 * button press, which is the one drag request the protocol requires a grab for:
 * a compositor that takes it holds the pointer away from every surface
 * underneath for a drag nobody started, so the run fails unless the source comes
 * back cancelled with no surface entered. */
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

#include "cursor-shape-client-protocol.h"
#include "fractional-scale-client-protocol.h"
#include "linux-dmabuf-client-protocol.h"
#include "primary-selection-client-protocol.h"
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
static void popups_open (void);
static void popup_reposition (void);
static void second_create (void);
static void second_draw (void);
static void second_focus_changed (void);
static void second_after_release (Buffer *buffer);

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
static struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
static struct wp_cursor_shape_device_v1 *cursor_shape_device;

static struct gbm_device *gbm;
static int use_dmabuf;
static int use_fullscreen, fs_enter_sent, fs_exit_sent;
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

/* The three requests that only exist from wl_data_device_manager version 3,
 * and that killed the whole compositor while their slots were empty:
 * wl_data_source.set_actions, wl_data_offer.set_actions, wl_data_offer.finish.
 * --dnd-requests sends all three. See dnd_requests_probe() for why that needs
 * a second connection. */
static int use_dnd_requests;
static int dnd_done;

/* A drag that leaves the page. --drag-out starts one from the left button press
 * and offers the three mime names measured off a real page's text drag, so the
 * compositor's handover has a source to read without a browser in the loop. The
 * payload is deliberately not the one --copy puts on the selection: a run that
 * mixed the two paths up says so in its own log. */
#define DRAG_OUT_CUSTOM_MIME "chromium/x-web-custom-data"

static int use_drag_out;
static int drag_out_sent;      /* a drag started by the press is being held */
static int drag_out_motions;   /* wl_data_device.motion events as the destination */
static int drag_out_reads;     /* send requests answered on the drag source */
static char drag_out_read_mime[128];
static struct wl_data_source *drag_source;
static const char drag_text[] = "drag-out-payload";

/* A drag the host carries in, which is the other direction: here the compositor
 * is the source and this client has to be the destination a page would be. The
 * three requests that make it one are the measured shape of what a real page
 * does — set_actions and accept on entering, receive to take the bytes, finish
 * to say it is done with them — and the read happens twice, because a page was
 * measured asking for the payload before the pointer let go and again after.
 * Whether the bytes that came back are the ones the host put in is only knowable
 * here, so a mismatch exits non-zero and seat-selftest treats that as its own
 * failure. The payload is deliberately not --drag-out's: a run that mixed the
 * two directions up says so in its own log. */
#define DRAG_IN_MIME    "text/plain"
#define DRAG_IN_PAYLOAD "drag-in-payload"

static int use_drag_in;
static struct wl_data_offer *drag_in_offer;
static char drag_in_accepted_mime[128]; /* the type this destination accepted */
static int drag_in_reads;           /* receives answered by the compositor */
static int drag_in_eager_pending;   /* the page-shaped read, before the drop */
static int drag_in_read_pending;    /* the destination-shaped read, after it */
static int drag_in_failed;          /* bytes came back that were not the ones */

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

/* Cursor identity. --cursor-shape asks wp_cursor_shape_device_v1 for one of the
 * CSS shapes every time the pointer enters, cycling so repeated enters cover
 * several names; --cursor-surface takes the older path and hands the compositor
 * a surface of our own drawing through wl_pointer.set_cursor. Both have to cite
 * the serial of the enter they answer, which is what the compositor checks. */
static int use_cursor_shape;
static int use_cursor_surface;
static uint32_t enter_serial;
static struct wl_surface *cursor_surface;
static struct wl_buffer *cursor_buffer;
static int cursor_asks;

/* Popups (xdg_popup). --popup opens a context-menu-sized surface and a submenu
 * over the client's own toplevel, each placed by positioner rules whose answer
 * a screenshot can be checked against; the menu asks for a seat grab, so it is
 * also how keyboard routing and popup_done dismissal get exercised.
 * --popup-reposition then moves the menu with a second set of rules. */
static int use_popup;
static int use_refused;
static int use_pool_budget;
static int use_wrong_object;
static int wrong_object_done;
static int use_popup_reposition;
static int use_second_window;
static int popups_opened;
static int popup_repositioned;

/* --pool-shrink commits a buffer from a pool whose file the client then cuts
 * down under the compositor's live mapping: the map the compositor took at
 * create_pool points at pages the file no longer has, and the next read of them
 * is a SIGBUS in the whole host, not in this connection. */
static int use_pool_shrink;
static int pool_shrink_done;

/* --damage-scale commits a buffer whose damage was named before the scale that
 * measures it, which is the one ordering of wl_surface.damage and
 * wl_surface.set_buffer_scale a compositor can get wrong in silence. See the
 * probe for the rectangle and the three sample points. */
static int use_damage_scale;

/* Raised before the probe's first commit and never lowered: the ordinary drawing
 * must not slip a frame in between the two, or the stale quarter the gate reads
 * gets painted over. Frame callbacks still arrive, so the loop has to be stopped
 * by a flag rather than by not asking for one. */
static int damage_scale_holding;

/* --empty-copy answers a read of its own selection with nothing at all, on both
 * the clipboard and the primary selection. Copying the empty string is legal, so
 * a compositor that turns the payload into text has to cope with a zero-byte
 * answer: g_bytes_get_data() of an empty GBytes and g_strndup() of a NULL buffer
 * both come back NULL, and a host that dereferences either dies with the page
 * that was only trying to clear a selection. The counters are the other half of
 * the gate: if the compositor never asks for the payload, nothing was read and
 * the mode would pass by proving nothing. */
static int use_empty_copy;
static int empty_copy_done;
static int empty_copy_checked;
static int empty_copy_reads;
static int primary_copy_reads;
static struct zwp_primary_selection_device_manager_v1 *primary_manager;
static struct zwp_primary_selection_device_v1 *primary_device;
static struct zwp_primary_selection_source_v1 *primary_source;

/* --popup-depth hangs a popup on a popup until the compositor says no. The host
 * walks a window's popup tree recursively — once to place each menu against its
 * parent, once to build the stack it draws — so the client that nests decides how
 * deep the C stack has to go. Without a cap, one unprivileged connection can ask
 * for ten thousand frames of recursion and take the whole windowing system down
 * with a SIGSEGV. POPUP_DEPTH_LIMIT mirrors NESTED_POPUP_MAX_DEPTH in
 * src/nested-internal.h. The refusal is the assertion: a build with the cap
 * removed has to go red rather than quietly accept the chain. */
#define POPUP_DEPTH_LIMIT   32
#define POPUP_DEPTH_ATTEMPT 128

static int use_popup_depth;
static int popup_depth_done;

/* --drag-fake-serial asks for a drag with a serial no press ever carried.
 * wl_data_device.start_drag says the origin surface is one "the client must have
 * an active implicit grab that matches" — the one place in the drag protocol that
 * makes a requirement of a serial rather than leaving it decorative. A compositor
 * that does not look starts a drag nobody began, and from that moment the seat
 * keeps every button event away from the surfaces underneath, because a drag is
 * holding the pointer; it keeps them away until some release arrives and is read
 * as a drop. The symptom is a page that stops taking clicks, so this is not a
 * tidiness question.
 *
 * The answer the protocol wants is wl_data_source.cancelled and no drag, because
 * the client owns the source until the compositor says otherwise. That reply is
 * the assertion. A serial invented here looks, to the compositor's records,
 * exactly like the press of a button that has since been let go, so one number
 * grades both halves of the rule. */
#define DRAG_FAKE_MIME   "application/x-cefnest-no-grab"
#define DRAG_FAKE_SERIAL 1u

static int use_drag_fake_serial;
static int drag_fake_done;
static int drag_fake_cancelled;
/* Every wl_data_device.enter this client has been sent, which is how the run
 * tells a refused request from a drag that quietly started. */
static int drag_enters;

/* --drag-no-source drags with no source at all, which is the shape wayland.xml
 * sanctions for a page that starts a drag and hands nothing over: the enter
 * arrives with no wl_data_offer, and the destination therefore has no
 * wl_data_offer.finish with which to retire the drag. See the probe for what that
 * costs — a surface that was the drag's icon stays an icon, and an icon that no
 * drag is carrying is not drawn at all. */
static int use_drag_no_source;
static int drag_ns_sent;
static int drag_ns_enters;
static int drag_ns_offers;
static int drag_ns_drops;
static int drag_ns_configured;
static int drag_ns_checked;

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
/* The 120ths of a wheel detent, which is the channel that keeps a fractional
 * detent, and a count of the gesture ends the compositor told us about. */
static int32_t scroll120_x;
static int32_t scroll120_y;
static int axis_stops;
/* Which axes sent a wl_pointer.axis in the frame being filled, one bit each. */
static unsigned int axis_in_frame;
/* Which wl_pointer.frame we are in. An axis event and the stop that ends its
 * gesture have to land in different ones, and this counter is how the log of a
 * run shows whether they did. */
static int pointer_frames;
static int pointer_inside;
/* Which of our surfaces the pointer is over: the toplevel or one of the popups
 * drawn on top of it. Motion events do not name a surface, so the enter does
 * and this is what routes them. */
static struct wl_surface *pointer_surface;
/* The surface the keyboard is over, which after a seat grab is the popup the
 * client opened rather than the toplevel. */
static struct wl_surface *keyboard_surface;
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
static void do_drag_out (uint32_t serial);
static void dnd_requests_probe (void);
static void wrong_object_probe (void);
static void pool_shrink_probe (void);
static void empty_copy_probe (void);
static void popup_depth_probe (void);
static void damage_scale_probe (void);
static void ns_drag_start (uint32_t serial);
static void ns_check (void);
static void drag_fake_probe (void);
static void ensure_data_device (void);
static void ensure_text_input (void);
static void ensure_cursor_shape (void);

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

  /* --second-window: this picture is the evidence, not a test pattern. Red
   * means the seat put the keyboard in this toplevel, green means it put it in
   * the other one. */
  if (use_second_window)
    {
      uint32_t solid = keyboard_surface == surface ? 0x00c04040u : 0x0040c040u;

      for (y = 0; y < buffer->height; y++)
        {
          uint32_t *row = pixels + (size_t) y * (buffer->stride / 4);

          for (x = 0; x < buffer->width; x++)
            row[x] = solid;
        }
      return;
    }

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

  /* The damage-scale probe leaves its picture standing on purpose: a frame from
   * the ordinary loop after it would overwrite the half the compositor is
   * supposed not to have read. */
  if (damage_scale_holding)
    return;

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
    {
      buffer->mapped = 0;
      second_after_release (buffer);
    }
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

  /* --fullscreen: exercise both edges of the toplevel fullscreen request at
   * real frame cadence — enter at ~0.5 s, leave at ~2.5 s — so the selftest
   * can assert it saw the signal twice and the client kept drawing in
   * between. */
  if (use_fullscreen && mapped)
    {
      if (frames >= 30 && !fs_enter_sent)
        {
          fs_enter_sent = 1;
          fprintf (stderr, "client: requesting toplevel fullscreen\n");
          xdg_toplevel_set_fullscreen (toplevel, NULL);
        }
      else if (frames >= 150 && !fs_exit_sent)
        {
          fs_exit_sent = 1;
          fprintf (stderr, "client: leaving toplevel fullscreen\n");
          xdg_toplevel_unset_fullscreen (toplevel);
        }
    }

  /* Same timing rule as the copy: the seat may only have advertised its
   * capabilities after the first frame. */
  if (use_dnd_requests && !dnd_done && frames > 3)
    {
      dnd_done = 1;
      ensure_data_device ();
      dnd_requests_probe ();
    }

  /* Out here rather than at startup: the globals have to have arrived, and the
   * window has to have painted, or "the compositor survived" would be a claim
   * about a run that never started. */
  if (use_wrong_object && !wrong_object_done && frames > 3)
    {
      wrong_object_done = 1;
      wrong_object_probe ();
    }

  /* Out here for the same reason, and it ends the run: this one has to cut the
   * file under a buffer the compositor is already showing, so the connection
   * that gets the answer is this client's own. */
  if (use_pool_shrink && !pool_shrink_done && frames > 3)
    {
      pool_shrink_done = 1;
      pool_shrink_probe ();
    }

  /* The same rule as the probes above: the window has to be mapped and the seat
   * and the globals have to have arrived before a commit of a new geometry is
   * worth anything. Nothing is graded here — the compositor's own frame is what
   * the selftest reads back. */
  if (use_damage_scale && !damage_scale_holding && frames > 3)
    damage_scale_probe ();

  /* The drop is what makes this worth checking: before it the drag has not yet
   * failed to be retired, and after it the icon surface is free to become a
   * window. One frame later, because this runs from inside the dispatch that
   * delivered the drop and the answer has to go out as its own round. */
  if (use_drag_no_source && drag_ns_drops && !drag_ns_checked)
    {
      drag_ns_checked = 1;
      ns_check ();
    }

  /* Both halves of --empty-copy wait for the same thing: the compositor reads a
   * selection when it is told about one, so the offers have to be sent after the
   * window is up, and the answer comes back on the source's fd some frames
   * later. Checking a count before it could have moved would only grade the
   * speed of the test. */
  if (use_empty_copy && !empty_copy_done && frames > 3)
    {
      empty_copy_done = 1;
      empty_copy_probe ();
    }

  if (use_empty_copy && empty_copy_done && !empty_copy_checked && frames > 40)
    {
      empty_copy_checked = 1;
      if (!empty_copy_reads || !primary_copy_reads)
        {
          fprintf (stderr, "client: %d: empty-copy gate FAILED: the compositor read "
                           "the clipboard %d time(s) and the primary selection %d, "
                           "so no zero-byte payload was ever turned into text\n",
                   __LINE__, empty_copy_reads, primary_copy_reads);
          exit (1);
        }
      fprintf (stderr, "client: the compositor took %d empty clipboard read(s) and "
                       "%d empty primary read(s) and is still serving this "
                       "connection\n", empty_copy_reads, primary_copy_reads);
    }

  /* Same rule as the wrong-object attacks: the globals have to have arrived and
   * the window has to be on screen, and the attacker lives on its own
   * connection so the refusal this probe wants cannot end the painting. */
  if (use_popup_depth && !popup_depth_done && frames > 3)
    {
      popup_depth_done = 1;
      popup_depth_probe ();
    }

  /* Needs a seat and a data device, so it waits for the same settle the other
   * data-device probes do — and, unlike them, for the window to have painted:
   * the drag it sends names this toplevel as its origin, and an origin the
   * compositor does not know about is refused before the serial is ever looked
   * at, which would grade the wrong rule. */
  if (use_drag_fake_serial && !drag_fake_done && mapped && frames > 3)
    {
      drag_fake_done = 1;
      drag_fake_probe ();
    }

  /* Open the popups once the toplevel has been configured and is drawing: the
   * rules are stated against its window geometry, so placing them before the
   * configure would only prove the compositor can guess. */
  if (use_popup && !popups_opened && width >= 400 && frames > 3)
    {
      popups_opened = 1;
      popups_open ();
    }

  if (use_popup_reposition && popups_opened && !popup_repositioned && frames > 30)
    {
      popup_repositioned = 1;
      popup_reposition ();
    }

  if (mapped)
    draw ();
}

static const struct wl_callback_listener frame_listener = {
  .done = frame_done,
};

/* ------------------------------------------------------------------ *
 * --second-window                                                    *
 *                                                                    *
 * A second toplevel, made by this same client: two independent CEF   *
 * windows are two surfaces of one wl_client, so the pair a host has  *
 * to route input between has to come from one connection too. Both   *
 * pictures say where the keyboard is — red in the focused window,    *
 * green in the other — which is what lets a run read the             *
 * compositor's routing decision off the buffers it imported instead  *
 * of trusting the state behind it.                                   *
 * ------------------------------------------------------------------ */

#define SECOND_APP_ID "nested-shm-test-second"
#define FOCUS_RED   0x00c04040u
#define FOCUS_GREEN 0x0040c040u

typedef struct {
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct wp_viewport *viewport;
  Buffer buffers[2];
  int width;
  int height;
  int device_width;
  int device_height;
  int configured;
  int has_focus;
  int repaint_wanted;
} SecondWindow;

static SecondWindow second = {
  .width = DEFAULT_WIDTH,
  .height = DEFAULT_HEIGHT,
};

static void
second_size (void)
{
  second.device_width = (int) (second.width * client_scale + 0.5);
  second.device_height = (int) (second.height * client_scale + 0.5);
  if (second.device_width < 1)
    second.device_width = 1;
  if (second.device_height < 1)
    second.device_height = 1;
}

static void
second_buffers_create (void)
{
  int i;

  for (i = 0; i < 2; i++)
    {
      buffer_destroy (&second.buffers[i]);
      /* Shm always: like the popups, this window tests routing, and an shm
       * buffer is one the compositor copies out — which is what releases it
       * soon enough for a redraw asked for in a burst. */
      buffer_create_shm (&second.buffers[i], second.device_width,
                        second.device_height);
    }
}

static void
second_draw (void)
{
  Buffer *buffer = NULL;
  uint32_t *pixels;
  int x, y, i;

  if (!second.surface || !second.configured)
    return;

  for (i = 0; i < 2; i++)
    if (!second.buffers[i].mapped && second.buffers[i].wl_buffer)
      {
        buffer = &second.buffers[i];
        break;
      }

  /* Both frames are still with the compositor. Its wl_buffer.release retries
   * this, so a focus change that arrives mid-burst is not lost. */
  if (!buffer)
    {
      second.repaint_wanted = 1;
      return;
    }

  second.repaint_wanted = 0;
  pixels = buffer->data;
  for (y = 0; y < buffer->height; y++)
    {
      uint32_t *row = pixels + (size_t) y * (buffer->stride / 4);

      for (x = 0; x < buffer->width; x++)
        row[x] = second.has_focus ? FOCUS_RED : FOCUS_GREEN;
    }

  wl_surface_attach (second.surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage_buffer (second.surface, 0, 0, buffer->width, buffer->height);
  if (second.viewport)
    wp_viewport_set_destination (second.viewport, second.width, second.height);
  wl_surface_commit (second.surface);
  buffer->mapped = 1;
}

static void
second_after_release (Buffer *buffer)
{
  if (buffer != &second.buffers[0] && buffer != &second.buffers[1])
    return;

  if (second.repaint_wanted)
    second_draw ();
}

static void
second_close (void)
{
  int i;

  if (!second.surface)
    return;

  fprintf (stderr, "client: second window torn down\n");

  xdg_toplevel_destroy (second.toplevel);
  xdg_surface_destroy (second.xdg_surface);
  wl_surface_destroy (second.surface);
  if (second.viewport)
    wp_viewport_destroy (second.viewport);
  for (i = 0; i < 2; i++)
    buffer_destroy (&second.buffers[i]);

  memset (&second, 0, sizeof second);
  second.width = DEFAULT_WIDTH;
  second.height = DEFAULT_HEIGHT;

  /* The other window repaints with whatever the seat says next; until then it
   * has to stop claiming the keyboard this one no longer has. */
  if (mapped)
    draw ();
}

static void
second_focus_changed (void)
{
  if (!use_second_window)
    return;

  second.has_focus = second.surface && keyboard_surface == second.surface;
  second_draw ();

  /* The first window paints through the ordinary path, which only runs on a
   * frame; ask it for the new colour now. */
  if (mapped)
    draw ();
}

static void
second_toplevel_configure (void *data, struct xdg_toplevel *obj, int32_t w,
                           int32_t h, struct wl_array *states)
{
  if (w > 0 && h > 0 && (w != second.width || h != second.height))
    {
      second.width = w;
      second.height = h;
      second_size ();
    }

  fprintf (stderr, "client: second toplevel configure %dx%d\n", w, h);
}

static void
second_toplevel_close (void *data, struct xdg_toplevel *obj)
{
  second_close ();
}

static const struct xdg_toplevel_listener second_toplevel_listener = {
  .configure = second_toplevel_configure,
  .close = second_toplevel_close,
};

static void
second_xdg_configure (void *data, struct xdg_surface *obj, uint32_t serial)
{
  xdg_surface_ack_configure (obj, serial);

  /* One rule for the first configure and for a resize: the buffers have to
   * match the size this window is showing at. */
  if (!second.configured ||
      second.buffers[0].width != second.device_width ||
      second.buffers[0].height != second.device_height)
    {
      second.configured = 1;
      second_buffers_create ();
    }

  second_draw ();
}

static const struct xdg_surface_listener second_xdg_surface_listener = {
  .configure = second_xdg_configure,
};

static void
second_create (void)
{
  second.surface = wl_compositor_create_surface (compositor);
  if (viewporter)
    second.viewport = wp_viewporter_get_viewport (viewporter, second.surface);
  second.xdg_surface = xdg_wm_base_get_xdg_surface (wm_base, second.surface);
  xdg_surface_add_listener (second.xdg_surface, &second_xdg_surface_listener, NULL);
  second.toplevel = xdg_surface_get_toplevel (second.xdg_surface);
  xdg_toplevel_add_listener (second.toplevel, &second_toplevel_listener, NULL);
  xdg_toplevel_set_title (second.toplevel, "shm test client second");
  xdg_toplevel_set_app_id (second.toplevel, SECOND_APP_ID);

  second_size ();
  second.has_focus = keyboard_surface == second.surface;

  fprintf (stderr, "client: second window created, app-id %s\n", SECOND_APP_ID);

  wl_surface_commit (second.surface);
}

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
 * cursor identity                                                    *
 * ------------------------------------------------------------------ */

#define N_CURSOR_SHAPES 5

static const uint32_t cursor_shapes[N_CURSOR_SHAPES] = {
  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT,
  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
};

static void
ensure_cursor_shape (void)
{
  if (!use_cursor_shape || !cursor_shape_manager || !pointer || cursor_shape_device)
    return;

  cursor_shape_device =
    wp_cursor_shape_manager_v1_get_pointer (cursor_shape_manager, pointer);
}

/* The surface the older wl_pointer.set_cursor path needs: an arrow of our own,
 * because a compositor that has never seen one is the bug this tests for. */
static void
cursor_surface_create (void)
{
  const int size = 32;
  struct wl_shm_pool *pool;
  uint32_t *pixels;
  int fd, x, y;

  if (!compositor || !shm || cursor_surface)
    {
      use_cursor_surface = 0;
      return;
    }

  cursor_surface = wl_compositor_create_surface (compositor);
  pixels = map_pool (size * size * 4, &fd);
  pool = wl_shm_create_pool (shm, fd, size * size * 4);
  cursor_buffer = wl_shm_pool_create_buffer (pool, 0, size, size, size * 4,
                                             WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);

  /* Opaque blue wedge on nothing else, so the hotspot at its tip is testable. */
  for (y = 0; y < size; y++)
    for (x = 0; x < size; x++)
      pixels[y * size + x] =
        (x <= y && x < 14) ? 0xff2020ffu : 0x00000000u;

  wl_surface_attach (cursor_surface, cursor_buffer, 0, 0);
  wl_surface_damage (cursor_surface, 0, 0, size, size);
}

static void
ask_cursor (void)
{
  if (cursor_shape_device)
    {
      uint32_t shape = cursor_shapes[cursor_asks % N_CURSOR_SHAPES];

      cursor_asks++;
      wp_cursor_shape_device_v1_set_shape (cursor_shape_device, enter_serial, shape);
      fprintf (stderr, "client: cursor shape %u, serial=%u\n", shape, enter_serial);
      return;
    }

  if (!use_cursor_surface)
    return;

  cursor_surface_create ();
  if (!cursor_surface)
    return;

  /* set_cursor first, commit after: the protocol says the cursor changes with
   * that commit, so a compositor that only looks at the buffer it finds right
   * now shows an arrow where this wedge belongs. */
  wl_pointer_set_cursor (pointer, enter_serial, cursor_surface, 2, 2);
  wl_surface_commit (cursor_surface);
  fprintf (stderr, "client: wl_pointer.set_cursor with our own surface, serial=%u\n",
           enter_serial);
}

/* ------------------------------------------------------------------ *
 * popups (xdg_popup)                                                 *
 * ------------------------------------------------------------------ */

#define N_POPUPS 2

/* The menu is placed by the rules the compositor has to read literally: the
 * anchor rectangle's bottom-left corner is the point, and BOTTOM_RIGHT gravity
 * means the child extends down and right from it. So the answer is the point
 * itself — (150, 100+20) = (150, 120). The submenu asks for the top-right of its
 * own rectangle, (200+10, 10) = (210, 10) in menu space, but a nested popup is
 * constrained to the window geometry of whatever it hangs off — here the menu's
 * 200x120 — so SLIDE_X pulls it back to (210 - (210+140-200)) = (60, 10). A
 * submenu that would stick out past the right of its parent is shown inside it,
 * which is what a real toolkit does with a menu too close to the screen edge. */
struct _Popup {
  const char *name;
  struct _Popup *parent;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_popup *popup;
  struct wl_buffer *buffer;
  void *data;
  int stride;
  int width, height;      /* the size the configure reported */
  int32_t x, y;           /* top-left the configure reported, in parent space */
  int configured;         /* a popup configure with a usable size arrived */
  int closed;
  int wants_grab;
  int grabbed;
  int pointer_inside;
  double pointer_x, pointer_y;
  uint32_t base;
  uint32_t edge;
  uint32_t mark;
};

static struct _Popup popups[N_POPUPS];

static const struct xdg_popup_listener popup_listener;
static const struct xdg_surface_listener popup_xdg_surface_listener;

static void
popup_close (struct _Popup *popup)
{
  int i;

  if (!popup->surface)
    return;

  fprintf (stderr, "client: popup %s torn down\n", popup->name);

  /* A submenu cannot outlive the menu it is drawn against. */
  for (i = 0; i < N_POPUPS; i++)
    if (popups[i].parent == popup)
      popup_close (&popups[i]);

  xdg_popup_destroy (popup->popup);
  xdg_surface_destroy (popup->xdg_surface);
  wl_surface_destroy (popup->surface);

  if (popup->buffer)
    wl_buffer_destroy (popup->buffer);
  if (popup->data)
    munmap (popup->data, (size_t) popup->stride * popup->height);

  memset (popup, 0, sizeof *popup);
  popup->closed = 1;
}

static void
popup_rect (struct _Popup *popup, int x, int y, int w, int h, uint32_t color)
{
  int px, py;

  for (py = y; py < y + h; py++)
    {
      uint32_t *row;

      if (py < 0 || py >= popup->height)
        continue;

      row = (uint32_t *) popup->data + (size_t) py * (popup->stride / 4);

      for (px = x; px < x + w; px++)
        if (px >= 0 && px < popup->width)
          row[px] = color;
    }
}

static void
popup_paint (struct _Popup *popup)
{
  int x, y;

  if (!popup->data)
    return;

  for (y = 0; y < popup->height; y++)
    {
      uint32_t *row = (uint32_t *) popup->data + (size_t) y * (popup->stride / 4);

      for (x = 0; x < popup->width; x++)
        row[x] = (x < 3 || y < 3 || x >= popup->width - 3 || y >= popup->height - 3)
                 ? popup->edge : popup->base;
    }

  /* The grab band says which popup asked for the seat, so a screenshot shows
   * where the keyboard is supposed to go without reading the log. */
  if (popup->grabbed)
    popup_rect (popup, 3, 3, popup->width - 6, 8, 0x00ffffffu);

  if (popup->pointer_inside)
    {
      popup_rect (popup, (int) popup->pointer_x - 40, (int) popup->pointer_y - 2,
                  80, 4, popup->mark);
      popup_rect (popup, (int) popup->pointer_x - 2, (int) popup->pointer_y - 40,
                  4, 80, popup->mark);
    }

  /* A held button turns the background green, as on the toplevel. */
  if (button_mask)
    popup_rect (popup, popup->width - 24, popup->height - 24, 16, 16, 0x0000ff00u);
}

static void
popup_draw (struct _Popup *popup)
{
  if (!popup->surface || !popup->buffer)
    return;

  popup_paint (popup);

  wl_surface_attach (popup->surface, popup->buffer, 0, 0);
  wl_surface_damage_buffer (popup->surface, 0, 0, popup->width, popup->height);
  wl_surface_commit (popup->surface);
}

static void
popup_draw_all (void)
{
  int i;

  for (i = 0; i < N_POPUPS; i++)
    if (popups[i].surface)
      popup_draw (&popups[i]);
}

static struct _Popup *
popup_for_surface (struct wl_surface *surface)
{
  int i;

  for (i = 0; i < N_POPUPS; i++)
    if (popups[i].surface && popups[i].surface == surface)
      return &popups[i];

  return NULL;
}

/* The serial a grab may cite is the one from the last pointer enter, so a popup
 * opened before the pointer arrived waits for it rather than naming serial 0. */
static void
popup_issue_grabs (void)
{
  int i;

  if (!seat || !enter_serial)
    return;

  for (i = 0; i < N_POPUPS; i++)
    if (popups[i].popup && popups[i].wants_grab && !popups[i].grabbed)
      {
        popups[i].grabbed = 1;
        xdg_popup_grab (popups[i].popup, seat, enter_serial);
        fprintf (stderr, "client: popup %s grabbed the seat with serial %u\n",
                 popups[i].name, enter_serial);
      }
}

static void
popup_buffer_create (struct _Popup *popup)
{
  struct wl_shm_pool *pool;
  int fd;

  /* Shm only: what this client is testing is placement and routing, not the
   * buffer transport the toplevel already covers. */
  if (popup->buffer)
    wl_buffer_destroy (popup->buffer);
  if (popup->data)
    munmap (popup->data, (size_t) popup->stride * popup->height);

  popup->buffer = NULL;
  popup->data = NULL;
  popup->stride = popup->width * 4;
  popup->data = map_pool (popup->stride * popup->height, &fd);

  pool = wl_shm_create_pool (shm, fd, popup->stride * popup->height);
  popup->buffer = wl_shm_pool_create_buffer (pool, 0, popup->width, popup->height,
                                             popup->stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
}

static void
popup_create (struct _Popup *popup, const char *name, struct _Popup *parent,
              int32_t rect_x, int32_t rect_y, int32_t rect_w, int32_t rect_h,
              int32_t size_w, int32_t size_h, uint32_t anchor, uint32_t gravity,
              int32_t offset_x, int32_t offset_y, uint32_t constraint, int grab)
{
  struct xdg_positioner *positioner;
  struct xdg_surface *parent_xdg = parent ? parent->xdg_surface : xdg_surface;

  positioner = xdg_wm_base_create_positioner (wm_base);
  xdg_positioner_set_size (positioner, size_w, size_h);
  xdg_positioner_set_anchor_rect (positioner, rect_x, rect_y, rect_w, rect_h);
  xdg_positioner_set_anchor (positioner, anchor);
  xdg_positioner_set_gravity (positioner, gravity);
  xdg_positioner_set_offset (positioner, offset_x, offset_y);
  xdg_positioner_set_constraint_adjustment (positioner, constraint);

  popup->name = name;
  popup->parent = parent;
  popup->wants_grab = grab;
  popup->surface = wl_compositor_create_surface (compositor);
  popup->xdg_surface = xdg_wm_base_get_xdg_surface (wm_base, popup->surface);
  xdg_surface_add_listener (popup->xdg_surface, &popup_xdg_surface_listener, popup);
  popup->popup = xdg_surface_get_popup (popup->xdg_surface, parent_xdg, positioner);
  xdg_popup_add_listener (popup->popup, &popup_listener, popup);

  xdg_positioner_destroy (positioner);

  fprintf (stderr, "client: popup %s created over %s, asking for %dx%d\n",
           name, parent ? parent->name : "the toplevel", size_w, size_h);

  /* The first commit is what maps the surface; the grab has to precede it. */
  popup_issue_grabs ();
  wl_surface_commit (popup->surface);
}

static void
popups_open (void)
{
  popups[0].base = 0x004040c0u;  /* blue menu */
  popups[0].edge = 0x00ffffffu;
  popups[0].mark = 0x0000ffffu;
  popups[1].base = 0x0040c040u;  /* green submenu */
  popups[1].edge = 0x00ffff00u;
  popups[1].mark = 0x00ff00ffu;

  popup_create (&popups[0], "menu", NULL,
                150, 100, 30, 20, 200, 120,
                XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
                XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
                0, 0,
                XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y, 1);

  popup_create (&popups[1], "submenu", &popups[0],
                200, 10, 10, 20, 140, 80,
                XDG_POSITIONER_ANCHOR_TOP_RIGHT,
                XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
                0, 0,
                XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y, 0);
}

/* Move the menu with a fresh positioner: this is what a real menu does when the
 * anchor item scrolls or the window resizes, and the repositioned event is the
 * answer it waits for. */
static void
popup_reposition (void)
{
  struct xdg_positioner *positioner;

  if (!popups[0].popup)
    return;

  positioner = xdg_wm_base_create_positioner (wm_base);
  xdg_positioner_set_size (positioner, 200, 120);
  xdg_positioner_set_anchor_rect (positioner, 600, 400, 30, 20);
  xdg_positioner_set_anchor (positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
  xdg_positioner_set_gravity (positioner, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
  xdg_positioner_set_offset (positioner, 4, -8);
  xdg_positioner_set_constraint_adjustment (positioner,
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

  xdg_popup_reposition (popups[0].popup, positioner, 77);
  xdg_positioner_destroy (positioner);

  fprintf (stderr, "client: popup menu repositioned to an anchor at 600,400\n");
}

static void
popup_configure (void *data, struct xdg_popup *obj, int32_t x, int32_t y,
                 int32_t w, int32_t h)
{
  struct _Popup *popup = data;

  popup->x = x;
  popup->y = y;
  popup->width = w;
  popup->height = h;
  popup->configured = 1;

  fprintf (stderr, "client: popup %s configured at %d,%d size %dx%d\n",
           popup->name, x, y, w, h);
}

static void
popup_popup_done (void *data, struct xdg_popup *obj)
{
  struct _Popup *popup = data;

  fprintf (stderr, "client: popup %s dismissed by the compositor\n", popup->name);
  popup_close (popup);

  if (mapped)
    draw ();
}

static void
popup_told_repositioned (void *data, struct xdg_popup *obj, uint32_t token)
{
  struct _Popup *popup = data;

  fprintf (stderr, "client: popup %s repositioned, token=%u, now at %d,%d %dx%d\n",
           popup->name, token, popup->x, popup->y, popup->width, popup->height);
}

static const struct xdg_popup_listener popup_listener = {
  .configure = popup_configure,
  .popup_done = popup_popup_done,
  .repositioned = popup_told_repositioned,
};

static void
popup_xdg_configure (void *data, struct xdg_surface *obj, uint32_t serial)
{
  struct _Popup *popup = data;

  xdg_surface_ack_configure (obj, serial);

  if (!popup->configured || popup->width <= 0 || popup->height <= 0)
    return;

  popup_buffer_create (popup);
  popup_draw (popup);
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
  .configure = popup_xdg_configure,
};

/* ------------------------------------------------------------------ *
 * wl_seat                                                            *
 * ------------------------------------------------------------------ */

static char seat_name[64] = "(none)";

static void
pointer_enter (void *data, struct wl_pointer *wl_pointer, uint32_t serial,
               struct wl_surface *wl_surface, wl_fixed_t sx, wl_fixed_t sy)
{
  struct _Popup *popup = popup_for_surface (wl_surface);

  enter_serial = serial;
  pointer_surface = wl_surface;

  /* The grab may cite no serial but this one, and a popup opened before the
   * pointer arrived can only ask now. */
  popup_issue_grabs ();

  if (popup)
    {
      popup->pointer_inside = 1;
      popup->pointer_x = wl_fixed_to_double (sx);
      popup->pointer_y = wl_fixed_to_double (sy);

      fprintf (stderr, "client: pointer enter popup %s serial=%u at %.1f,%.1f\n",
               popup->name, serial, popup->pointer_x, popup->pointer_y);
      popup_draw (popup);
      return;
    }

  pointer_inside = 1;
  pointer_x = wl_fixed_to_double (sx);
  pointer_y = wl_fixed_to_double (sy);

  fprintf (stderr, "client: pointer enter serial=%u at %.1f,%.1f\n",
           serial, pointer_x, pointer_y);

  ask_cursor ();
}

static void
pointer_leave (void *data, struct wl_pointer *wl_pointer, uint32_t serial,
               struct wl_surface *wl_surface)
{
  struct _Popup *popup = popup_for_surface (wl_surface);

  if (popup)
    {
      popup->pointer_inside = 0;
      fprintf (stderr, "client: pointer leave popup %s serial=%u\n",
               popup->name, serial);
      return;
    }

  pointer_inside = 0;

  fprintf (stderr, "client: pointer leave serial=%u\n", serial);
}

static void
pointer_motion (void *data, struct wl_pointer *wl_pointer, uint32_t time,
                wl_fixed_t sx, wl_fixed_t sy)
{
  struct _Popup *popup = popup_for_surface (pointer_surface);

  if (popup)
    {
      popup->pointer_x = wl_fixed_to_double (sx);
      popup->pointer_y = wl_fixed_to_double (sy);

      if (++motion_events % 60 == 1)
        fprintf (stderr, "client: motion #%d in popup %s at %.1f,%.1f\n",
                 motion_events, popup->name, popup->pointer_x, popup->pointer_y);
      return;
    }

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

  /* A page starts its drag from inside the handler for the press that grabs it,
   * and the serial it cites is this one, so the request goes out here rather
   * than on the next frame. */
  if (state == WL_POINTER_BUTTON_STATE_PRESSED && use_drag_out && !drag_out_sent)
    {
      ensure_data_device ();
      if (data_device)
        {
          drag_out_sent = 1;
          do_drag_out (serial);
        }
      else
        fprintf (stderr, "client: no data device, cannot drag\n");
    }
  else if (state != WL_POINTER_BUTTON_STATE_PRESSED && use_drag_out)
    {
      /* Re-arm: a run that hands a drag over still has a pointer, and the next
       * press should start a drag the way a page's does. */
      drag_out_sent = 0;
    }

  /* A page that drags without preparing a payload cites the same press, so this
   * is the same moment as the drag above — and the same serial, which is what
   * lets the compositor see a real grab behind a drag that owns nothing. */
  if (state == WL_POINTER_BUTTON_STATE_PRESSED && use_drag_no_source && !drag_ns_sent)
    {
      ensure_data_device ();
      if (!data_device || !compositor || !shm || !wm_base)
        {
          fprintf (stderr, "client: %d: drag-no-source gate FAILED: no data device, "
                           "compositor, shm or xdg wm base, so the sourceless drag "
                           "could not be asked for\n", __LINE__);
          exit (1);
        }

      drag_ns_sent = 1;
      ns_drag_start (serial);
    }

  /* Every surface shows the held button, so a screenshot keeps the evidence of
   * which one the press went to. */
  popup_draw_all ();
}

static void
pointer_axis (void *data, struct wl_pointer *wl_pointer, uint32_t time,
              uint32_t axis, wl_fixed_t value)
{
  double v = wl_fixed_to_double (value);

  axis_in_frame |= 1u << axis;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
    scroll_y += v;
  else
    scroll_x += v;

  fprintf (stderr, "client: axis %u %+.2f (total %+.1f) in frame #%d\n", axis, v,
           axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? scroll_y : scroll_x,
           pointer_frames + 1);
}

static void
pointer_frame (void *data, struct wl_pointer *wl_pointer)
{
  pointer_frames++;
  axis_in_frame = 0;

  if (mapped)
    draw ();

  popup_draw_all ();
}

static void
pointer_axis_source (void *data, struct wl_pointer *wl_pointer, uint32_t axis_source)
{
  static const char *const names[] = { "wheel", "finger", "continuous", "wheel_tilt" };
  const char *name = axis_source < sizeof (names) / sizeof (names[0])
    ? names[axis_source] : "?";

  fprintf (stderr, "client: axis source %s, frame #%d\n", name, pointer_frames + 1);
}

static void
pointer_axis_stop (void *data, struct wl_pointer *wl_pointer, uint32_t time,
                   uint32_t axis)
{
  axis_stops++;

  /* This one is an assertion, not a note in a log: a stop says the sequence has
   * ended, and Chromium settles the sequence when it reads one, so a delta in
   * the same frame never becomes a wheel event and the page scrolls nothing.
   * Exiting non-zero is what makes `meson test` hear about it. */
  if (axis_in_frame & (1u << axis))
    {
      fprintf (stderr,
               "client: axis %u stop came in frame #%d, the same frame as the axis "
               "event it stops\n", axis, pointer_frames + 1);
      exit (1);
    }

  fprintf (stderr, "client: axis %u stop (#%d) in frame #%d\n", axis, axis_stops,
           pointer_frames + 1);
}

static void
pointer_axis_discrete (void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                       int32_t discrete)
{
  fprintf (stderr, "client: axis %d discrete %+d\n", axis, discrete);
}

static void
pointer_axis_value120 (void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                       int32_t value120)
{
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
    scroll120_y += value120;
  else
    scroll120_x += value120;

  fprintf (stderr, "client: axis %u value120 %+d (total %+d) in frame #%d\n", axis,
           value120,
           axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? scroll120_y : scroll120_x,
           pointer_frames + 1);
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
  .axis_value120 = pointer_axis_value120,
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
  struct _Popup *popup = popup_for_surface (wl_surface);

  keyboard_surface = wl_surface;

  fprintf (stderr, "client: keyboard enter on %s, serial=%u, %zu keys held\n",
           popup ? popup->name : (wl_surface == second.surface ? "the second window" : "the toplevel"),
           serial, keys->size / sizeof (uint32_t));

  second_focus_changed ();
}

static void
keyboard_leave (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                struct wl_surface *wl_surface)
{
  fprintf (stderr, "client: keyboard leave %s serial=%u\n",
           popup_for_surface (wl_surface) ? "of the popup" : "of the toplevel",
           serial);

  if (wl_surface == keyboard_surface)
    keyboard_surface = NULL;

  second_focus_changed ();
}

static void
keyboard_key (void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
              uint32_t time, uint32_t key, uint32_t state)
{
  struct _Popup *focus = popup_for_surface (keyboard_surface);
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

  fprintf (stderr, "client: key %s evdev=%u keysym=%s on %s mods=0x%x\n",
           state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up", key, name,
           focus ? focus->name :
             (keyboard_surface == second.surface ? "the second window" : "the toplevel"),
           xkb_state ? xkb_state_serialize_mods (xkb_state, XKB_STATE_MODS_DEPRESSED) : 0u);

  /* --second-window: the key is the trigger and the proof. Only the window the
   * seat sent it to can act on it, so a press that closes the second toplevel
   * also says which surface held the keyboard at that moment. */
  if (use_second_window && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      second.surface && keyboard_surface == second.surface)
    {
      fprintf (stderr, "client: the second window holds the keyboard, closing it\n");
      second_close ();
      return;
    }

  if (mapped)
    draw ();

  popup_draw_all ();
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
  ensure_cursor_shape ();
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
      /* Both a selection and a drag's offer answer this, so the line cannot
       * claim which one it belongs to. */
      fprintf (stderr, "client: an offer lists %s\n", mime_type);
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

/* This client is its own destination: the surface the drag started on is the
 * only one the nested compositor has to drag over. For --drag-out it is a
 * passive one — no accept, no receive — which is the shape the in-page loop has
 * to work with anyway, and it means a run that reaches a drop can say so here.
 * For --drag-in it is an active one, because the drag has no destination unless
 * this client agrees to be one. */
static void
data_device_enter (void *data, struct wl_data_device *device, uint32_t serial,
                   struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                   struct wl_data_offer *offer)
{
  drag_enters++;

  if (use_drag_no_source)
    {
      drag_ns_enters++;
      if (offer)
        drag_ns_offers++;
    }

  fprintf (stderr, "client: %d: dragged onto a surface at %.1f,%.1f serial=%u\n",
           getpid(), wl_fixed_to_double (x), wl_fixed_to_double (y), serial);

  if (!use_drag_in || !offer)
    return;

  /* The mime list arrived with the offer, before this event, so the type to
   * take is already known. A page takes the short one of the two text names, and
   * asking for the one it was listed is the only shape that can be checked. */
  drag_in_offer = offer;
  drag_in_accepted_mime[0] = '\0';
  drag_in_reads = 0;
  drag_in_eager_pending = 0;
  drag_in_read_pending = 0;
  drag_in_failed = 0;

  if (wl_data_offer_get_version (offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
    wl_data_offer_set_actions (offer,
                               WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                               WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                               WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

  for (int i = 0; i < n_selection_mimes; i++)
    if (!strcmp (selection_mimes[i], DRAG_IN_MIME))
      {
        snprintf (drag_in_accepted_mime, sizeof drag_in_accepted_mime, "%s",
                  DRAG_IN_MIME);
        break;
      }

  if (!drag_in_accepted_mime[0] && n_selection_mimes > 0)
    snprintf (drag_in_accepted_mime, sizeof drag_in_accepted_mime, "%s",
              selection_mimes[0]);

  if (!drag_in_accepted_mime[0])
    {
      fprintf (stderr, "client: %d: the drag arrived with no type to take\n",
               getpid ());
      drag_in_failed = 1;
      return;
    }

  /* The serial this enter carries is what the accept has to cite. */
  wl_data_offer_accept (offer, serial, drag_in_accepted_mime);
  fprintf (stderr, "client: %d: the drag is taken as %s, %d type(s) listed\n",
           getpid(), drag_in_accepted_mime, n_selection_mimes);

  /* First of the two reads: the payload asked for while the pointer is still
   * held over the destination, which is what a page was measured doing. */
  drag_in_eager_pending = 1;
}

static void
data_device_motion (void *data, struct wl_data_device *device, uint32_t time,
                    wl_fixed_t x, wl_fixed_t y)
{
  drag_out_motions++;
  fprintf (stderr, "client: %d: the drag moved to %.1f,%.1f\n", getpid(),
           wl_fixed_to_double (x), wl_fixed_to_double (y));
}

static void
data_device_leave (void *data, struct wl_data_device *device)
{
  fprintf (stderr, "client: %d: the drag left the surface\n", getpid());

  if (!use_drag_in || !drag_in_offer)
    return;

  /* A leave that is not followed by a drop is the host taking its own drag back
   * somewhere else. Nothing was owed and nothing was read, so the offer goes the
   * way the protocol says it does at exactly this point. */
  drag_in_eager_pending = 0;
  drag_in_read_pending = 0;
  fprintf (stderr, "client: %d: the drag was given back after %d read(s)\n",
           getpid(), drag_in_reads);
  wl_data_offer_destroy (drag_in_offer);
  drag_in_offer = NULL;
}

static void
data_device_drop (void *data, struct wl_data_device *device)
{
  fprintf (stderr, "client: %d: the drag was dropped here\n", getpid());

  if (use_drag_no_source)
    drag_ns_drops++;

  if (!use_drag_in)
    return;

  if (!drag_in_offer)
    {
      fprintf (stderr, "client: %d: the drag-in gate FAILED: dropped with "
                       "nothing offered to take\n", getpid ());
      drag_in_failed = 1;
      return;
    }

  /* Second of the two reads, and the one a destination that waits for the
   * pointer to let go does. The finish that follows it is what ends the drag. */
  drag_in_read_pending = 1;
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
  .enter = data_device_enter,
  .motion = data_device_motion,
  .leave = data_device_leave,
  .drop = data_device_drop,
};

static void
data_source_send (void *data, struct wl_data_source *source,
                  const char *mime_type, int fd)
{
  const char *payload = copy_text;
  size_t left = strlen (copy_text);
  size_t offset = 0;
  int is_drag = source == drag_source;

  if (is_drag)
    {
      /* Whatever the compositor names, which for a handover is a type this
       * source was never asked to accept by any destination. A page that keeps
       * its payload to itself in that case is indistinguishable from a
       * compositor that never asked, so answer. */
      payload = drag_text;
      left = strlen (drag_text);
      drag_out_reads++;
      snprintf (drag_out_read_mime, sizeof drag_out_read_mime, "%s", mime_type);
    }
  else if (strcmp (mime_type, "text/plain;charset=utf-8") != 0)
    payload = NULL;
  else if (use_empty_copy)
    {
      payload = NULL;
      empty_copy_reads++;
    }

  fprintf (stderr, "client: %d: %s asked for %s, answering with %s\n",
           getpid(), is_drag ? "the drag" : "the selection", mime_type,
           payload ? (is_drag ? drag_text : "the copy") : "nothing");

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
  if (source == drag_source)
    {
      drag_source = NULL;
      fprintf (stderr, "client: %d: the drag was cancelled after %d motion(s) "
                       "and %d read(s) of %s\n", getpid(), drag_out_motions,
               drag_out_reads,
               drag_out_read_mime[0] ? drag_out_read_mime : "(nothing asked)");
      wl_data_source_destroy (source);
      return;
    }

  fprintf (stderr, "client: our selection was superseded\n");
  wl_data_source_destroy (source);
}

/* Every event a version 3 source can be sent. libwayland aborts the connection
 * on an event whose slot the client left NULL, so binding 3 means answering all
 * six — including target, which is the one this compositor sends when it reads a
 * drag's payload for the host. */
static void
data_source_target (void *data, struct wl_data_source *source, const char *mime_type)
{
  fprintf (stderr, "client: %d: %s: a reader wants %s\n", getpid(),
           source == drag_source ? "drag" : "selection",
           mime_type ? mime_type : "(nothing)");
}

static void
data_source_drop_performed (void *data, struct wl_data_source *source)
{
  fprintf (stderr, "client: %d: %s: the pointer let go\n", getpid(),
           source == drag_source ? "drag" : "selection");
}

static void
data_source_dnd_finished (void *data, struct wl_data_source *source)
{
  fprintf (stderr, "client: %d: %s: the destination is done with it\n", getpid(),
           source == drag_source ? "drag" : "selection");
}

static void
data_source_action (void *data, struct wl_data_source *source, uint32_t action)
{
  fprintf (stderr, "client: %d: %s: action 0x%x chosen\n", getpid(),
           source == drag_source ? "drag" : "selection", action);
}

static const struct wl_data_source_listener data_source_listener = {
  .target = data_source_target,
  .send = data_source_send,
  .cancelled = data_source_cancelled,
  .dnd_drop_performed = data_source_drop_performed,
  .dnd_finished = data_source_dnd_finished,
  .action = data_source_action,
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

/* The mime list and the action are the ones measured off a real page's text
 * drag: three names for one string, copy only. Nothing here is a drop target's
 * doing — no accept, no receive — because the point of the run is what the
 * compositor does when the destination is not one of its clients at all. */
static void
do_drag_out (uint32_t serial)
{
  struct wl_data_source *source;

  source = wl_data_device_manager_create_data_source (data_manager);
  wl_data_source_offer (source, "text/plain;charset=utf-8");
  wl_data_source_offer (source, "text/plain");
  wl_data_source_offer (source, DRAG_OUT_CUSTOM_MIME);
  wl_data_source_add_listener (source, &data_source_listener, source);
  wl_data_source_set_actions (source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  drag_source = source;
  drag_out_motions = 0;
  drag_out_reads = 0;
  drag_out_read_mime[0] = '\0';

  wl_data_device_start_drag (data_device, source, surface, NULL, serial);
  fprintf (stderr, "client: %d: started a drag of 3 types, serial=%u, payload '%s'\n",
           getpid (), serial, drag_text);
}

/* Taking the payload the compositor is carrying in. The shape is do_paste's —
 * make a pipe, hand its write end over, drain the socket so the request really
 * reaches the compositor, then read until the last writer goes — with one
 * difference that matters: this is a drag, so the offer has to be finished once
 * the bytes have been looked at, and a mismatch has to end the run. */
static void
do_drag_in_read (const char *when)
{
  char buf[4096];
  size_t len = 0;
  ssize_t n;
  int fds[2];

  if (!drag_in_offer || !drag_in_accepted_mime[0])
    return;

  if (pipe (fds) != 0)
    {
      drag_in_failed = 1;
      return;
    }

  wl_data_offer_receive (drag_in_offer, drag_in_accepted_mime, fds[1]);

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
  drag_in_reads++;

  if (len != strlen (DRAG_IN_PAYLOAD) || memcmp (buf, DRAG_IN_PAYLOAD, len) != 0)
    {
      fprintf (stderr, "client: %d: the drag-in gate FAILED: %s it read %zu "
                       "bytes of %s: '%s', wanted '%s'\n", getpid(), when, len,
                       drag_in_accepted_mime, buf, DRAG_IN_PAYLOAD);
      drag_in_failed = 1;
      return;
    }

  fprintf (stderr, "client: %d: %s it read %zu bytes of %s: '%s'\n",
           getpid(), when, len, drag_in_accepted_mime, buf);
}

/* Both ends of a destination's side of a drag: say the bytes were right and
 * hand the offer back, or leave the run to fail. Exiting here is the report —
 * seat-selftest owns the compositor and treats a client that leaves non-zero as
 * the compositor getting something wrong. */
static void
do_drag_in_conclude (void)
{
  if (!drag_in_offer)
    return;

  if (drag_in_failed)
    {
      fprintf (stderr, "client: %d: the drag-in gate FAILED: the host's payload "
                       "did not come back\n", getpid ());
      exit (1);
    }

  if (drag_in_reads < 1)
    {
      fprintf (stderr, "client: %d: the drag-in gate FAILED: nothing was read "
                       "off the offer\n", getpid ());
      drag_in_failed = 1;
      exit (1);
    }

  /* A destination that drops and stays silent is a destination that kept the
   * payload: the protocol's only way of saying that is this version 3 request,
   * and it is also what retires the drag on the compositor's side. */
  if (wl_data_offer_get_version (drag_in_offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
    wl_data_offer_finish (drag_in_offer);

  fprintf (stderr, "client: %d: drag-in gate ok, %d read(s) of %s and the "
                   "offer is finished\n", getpid(), drag_in_reads,
           drag_in_accepted_mime);

  wl_data_offer_destroy (drag_in_offer);
  drag_in_offer = NULL;
}

/* The compositor does not hand a client back its own selection, so one
 * connection cannot both put up an offer and be the target of it. A second
 * connection is a second wl_client as far as the compositor is concerned, and
 * that is enough: the first takes the selection, the second receives the offer
 * and answers it. Everything here stays inside the nested compositor because
 * the mime below is not one the clipboard bridge copies outward — the host
 * clipboard is left alone. */
#define DND_PROBE_MIME "application/x-cefnest-dnd-probe"

static struct wl_display *dnd_display;
static struct wl_data_device_manager *dnd_manager;
static struct wl_seat *dnd_seat;
static struct wl_data_device *dnd_device;
static struct wl_data_offer *dnd_offer;

static void
dnd_offer_mime (void *data, struct wl_data_offer *offer, const char *mime_type)
{
  fprintf (stderr, "client: the second connection was offered %s\n", mime_type);
}

static void
dnd_offer_actions (void *data, struct wl_data_offer *offer, uint32_t actions)
{
  fprintf (stderr, "client: that offer supports actions 0x%x\n", actions);
}

static const struct wl_data_offer_listener dnd_offer_listener = {
  .offer = dnd_offer_mime,
  .source_actions = dnd_offer_actions,
  .action = dnd_offer_actions,
};

static void
dnd_device_offer (void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
  dnd_offer = offer;
  wl_data_offer_add_listener (offer, &dnd_offer_listener, NULL);
}

static void
dnd_device_selection (void *data, struct wl_data_device *device,
                      struct wl_data_offer *offer)
{
  fprintf (stderr, "client: the second connection's selection is offer id %u\n",
           offer ? wl_proxy_get_id ((struct wl_proxy *) offer) : 0);
}

static void
dnd_device_leave (void *data, struct wl_data_device *device)
{
}

static const struct wl_data_device_listener dnd_device_listener = {
  .data_offer = dnd_device_offer,
  .selection = dnd_device_selection,
  .leave = dnd_device_leave,
};

static void
dnd_registry_global (void *data, struct wl_registry *registry, uint32_t name,
                     const char *interface, uint32_t version)
{
  if (!strcmp (interface, "wl_data_device_manager"))
    dnd_manager = wl_registry_bind (registry, name, &wl_data_device_manager_interface,
                                    version < 3 ? version : 3);
  else if (!strcmp (interface, "wl_seat"))
    dnd_seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
}

static void
dnd_registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener dnd_registry_listener = {
  .global = dnd_registry_global,
  .global_remove = dnd_registry_global_remove,
};

static void
dnd_requests_probe (void)
{
  struct wl_data_source *source;
  struct wl_registry *registry;
  int spins;

  if (!data_device)
    {
      fprintf (stderr, "client: no data device, the v3 requests cannot be sent\n");
      return;
    }

  dnd_display = wl_display_connect (NULL);
  if (!dnd_display)
    {
      fprintf (stderr, "client: cannot open the second connection for the probe\n");
      return;
    }

  registry = wl_display_get_registry (dnd_display);
  wl_registry_add_listener (registry, &dnd_registry_listener, NULL);
  wl_display_roundtrip (dnd_display);

  if (!dnd_manager || !dnd_seat)
    {
      fprintf (stderr, "client: second connection got manager=%d seat=%d, probe abandoned\n",
               !!dnd_manager, !!dnd_seat);
      wl_display_disconnect (dnd_display);
      dnd_display = NULL;
      return;
    }

  /* Created before the selection, or there is nothing to advertise to it. */
  dnd_device = wl_data_device_manager_get_data_device (dnd_manager, dnd_seat);
  wl_data_device_add_listener (dnd_device, &dnd_device_listener, NULL);
  wl_display_roundtrip (dnd_display);

  /* Request 2 of wl_data_source. Chromium sends this before it even tries to
   * start a drag, which is why an empty slot here took the whole desktop down. */
  source = wl_data_device_manager_create_data_source (data_manager);
  wl_data_source_offer (source, DND_PROBE_MIME);
  wl_data_source_add_listener (source, &data_source_listener, source);
  wl_data_source_set_actions (source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
  wl_data_device_set_selection (data_device, source, 1);
  fprintf (stderr, "client: sent wl_data_source.set_actions and took the selection\n");
  wl_display_roundtrip (display);

  for (spins = 0; !dnd_offer && spins < 10; spins++)
    wl_display_roundtrip (dnd_display);

  if (!dnd_offer)
    fprintf (stderr, "client: no offer reached the second connection, so the two "
                     "wl_data_offer requests were not sent\n");
  else
    {
      /* Requests 4 and 3 of wl_data_offer. finish() belongs to a drag that
       * never happens here — sending it at all is the point, the slot has to
       * hold whatever the compositor thinks of the gesture. */
      wl_data_offer_set_actions (dnd_offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                 WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
      wl_data_offer_finish (dnd_offer);
      fprintf (stderr, "client: sent wl_data_offer.set_actions and .finish on id %u\n",
               wl_proxy_get_id ((struct wl_proxy *) dnd_offer));
    }

  /* A compositor that did not like any of this answers with an error, and the
   * error only shows up once both connections have been flushed. */
  wl_display_roundtrip (dnd_display);
  wl_display_roundtrip (display);
  fprintf (stderr, "client: still connected on both halves (main error=%d second error=%d)\n",
           wl_display_get_error (display), wl_display_get_error (dnd_display));
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
  else if (!strcmp (interface, "zwp_primary_selection_device_manager_v1"))
    {
      /* Only --empty-copy reaches for this: the primary selection is the second
       * place a compositor turns a payload into text, and the first one that did
       * it with a NULL string killed the host outright. */
      primary_manager =
        wl_registry_bind (registry, name,
                          &zwp_primary_selection_device_manager_v1_interface,
                          version < 1 ? version : 1);
    }
  else if (!strcmp (interface, "zwp_text_input_manager_v3"))
    {
      text_input_manager = wl_registry_bind (registry, name,
                                             &zwp_text_input_manager_v3_interface,
                                             version < 1 ? version : 1);
      ensure_text_input ();
    }
  else if (!strcmp (interface, "wp_cursor_shape_manager_v1"))
    {
      cursor_shape_manager = wl_registry_bind (registry, name,
                                               &wp_cursor_shape_manager_v1_interface,
                                               version < 1 ? version : 1);
      ensure_cursor_shape ();
    }
  else if (!strcmp (interface, "wl_seat"))
    {
      /* Pointer and keyboard inherit this version, and 8 is what lets the seat
       * deliver wl_pointer.axis_value120 — a listener slot this client has to
       * fill before asking for it, because libwayland-client aborts on an event
       * whose slot is NULL just like the server does on a request. */
      seat = wl_registry_bind (registry, name, &wl_seat_interface,
                               version < 8 ? version : 8);
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

/* ------------------------------------------------------------------ *
 * resource limits (requests meant to be refused)                     *
 * ------------------------------------------------------------------ *
 *
 * A run with --refused sends nothing the compositor should accept: imports of
 * a buffer whose fourcc no one exports, and a pool bigger than any frame
 * needs. What is being checked is the compositor's side of the answer — it has
 * to stay up, and it has to give back the plane descriptors it was handed even
 * when it could not use them. The oversized pool comes last because the
 * compositor answers that one by killing this connection.
 *
 * --pool-budget asks for a pile of pools that are individually legal, which is
 * a different question: how much of the host one connection may hold in total.
 * It kills the connection too, so it is a mode of its own. */

#define PROBE_IMPORTS 40
#define PROBE_POOL_SIZE (300 * 1024 * 1024)

static int refused_imports;

static void
probe_params_created (void *data, struct zwp_linux_buffer_params_v1 *params,
                      struct wl_buffer *buffer)
{
  fprintf (stderr, "client: the compositor imported a buffer it should have refused\n");
  wl_buffer_destroy (buffer);
  zwp_linux_buffer_params_v1_destroy (params);
}

static void
probe_params_failed (void *data, struct zwp_linux_buffer_params_v1 *params)
{
  refused_imports++;
  zwp_linux_buffer_params_v1_destroy (params);
}

static const struct zwp_linux_buffer_params_v1_listener probe_params_listener = {
  .created = probe_params_created,
  .failed = probe_params_failed,
};

static void
probe_refused_imports (void)
{
  struct gbm_device *device;
  struct gbm_bo *bo;
  int fd, i, asked = 0;

  if (!dmabuf)
    {
      fprintf (stderr, "client: no dmabuf, cannot probe the import path\n");
      return;
    }

  fd = open (render_node, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      fprintf (stderr, "client: cannot open %s: %s\n", render_node, strerror (errno));
      return;
    }
  device = gbm_create_device (fd); /* owns the fd, success or not */
  if (!device)
    return;

  bo = gbm_bo_create (device, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR);
  if (!bo)
    {
      fprintf (stderr, "client: no bo to probe with\n");
      gbm_device_destroy (device);
      return;
    }

  for (i = 0; i < PROBE_IMPORTS; i++)
    {
      struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params (dmabuf);
      int plane_fd = gbm_bo_get_fd (bo);

      if (plane_fd < 0)
        break;

      /* add() hands the descriptor over: libwayland closes this process's copy
       * once the request has gone out, so it is not ours to close. */
      zwp_linux_buffer_params_v1_add (params, plane_fd, 0, 0, 64 * 4, 0, 0);
      zwp_linux_buffer_params_v1_add_listener (params, &probe_params_listener, NULL);
      /* Not a fourcc any driver exports, so the import can only fail. */
      zwp_linux_buffer_params_v1_create (params, 64, 64, 0x12345678, 0);
      asked++;
      if (wl_display_roundtrip (display) < 0)
        {
          fprintf (stderr, "client: the compositor hung up on import %d of %d, so "
                           "the rest cannot be asked\n", i, PROBE_IMPORTS);
          break;
        }
    }

  fprintf (stderr, "client: %d of %d impossible imports were refused\n",
           refused_imports, PROBE_IMPORTS);

  /* Every import that was asked has to come back refused, and all of them have
   * to have been asked: an impossible format the compositor accepts is the bug
   * this mode exists to catch, and a run that prints the two numbers without
   * judging them is a green line that says nothing. */
  if (refused_imports != asked || asked != PROBE_IMPORTS)
    {
      fprintf (stderr, "client: %d: the refused-import gate FAILED (%d refused of "
                       "%d asked, %d expected)\n", __LINE__, refused_imports,
                       asked, PROBE_IMPORTS);
      exit (1);
    }

  gbm_bo_destroy (bo);
  gbm_device_destroy (device);
}

static void
probe_oversized_pool (void)
{
  int fd = memfd_create ("shm-client-huge", MFD_CLOEXEC);

  if (fd < 0 || ftruncate (fd, PROBE_POOL_SIZE) < 0)
    {
      fprintf (stderr, "client: cannot make an oversized pool: %s\n", strerror (errno));
      if (fd >= 0)
        close (fd);
      return;
    }

  /* Nothing is written to it: the request alone is what the compositor has to
   * answer, and a protocol error is the answer that ends this connection. */
  wl_shm_create_pool (shm, fd, PROBE_POOL_SIZE);
  if (wl_display_roundtrip (display) < 0)
    fprintf (stderr, "client: the compositor refused the %d MB pool and dropped us\n",
             PROBE_POOL_SIZE / (1024 * 1024));
  else
    {
      fprintf (stderr, "client: %d: the oversized-pool gate FAILED: the compositor "
                       "accepted a %d MB pool, over the limit it advertises\n",
               __LINE__, PROBE_POOL_SIZE / (1024 * 1024));
      exit (1);
    }
}

/* The per-connection budget, which is the other half of the cap: sixteen pools
 * of 100 MB are each legal on their own, and a host that only ever measures one
 * of them lets a client hold 1.6 GB of its memory.
 *
 * The pools are kept alive rather than created and destroyed, because handing
 * bytes back is the thing this probe is not testing, and a cycle of create/destroy
 * would be refused by nothing. */
#define PROBE_BUDGET_POOL_SIZE (100 * 1024 * 1024)
#define PROBE_BUDGET_POOLS 16

static void
probe_pool_budget (void)
{
  int asked = 0;
  int refused = 0;
  int granted, i;

  for (i = 0; i < PROBE_BUDGET_POOLS; i++)
    {
      int fd = memfd_create ("shm-client-budget", MFD_CLOEXEC);

      if (fd < 0 || ftruncate (fd, PROBE_BUDGET_POOL_SIZE) < 0)
        {
          fprintf (stderr, "client: cannot make budget pool %d: %s\n",
                   i, strerror (errno));
          if (fd >= 0)
            close (fd);
          break;
        }

      /* The pool this returns is never destroyed: holding the bytes is the test. */
      wl_shm_create_pool (shm, fd, PROBE_BUDGET_POOL_SIZE);
      asked++;
      if (wl_display_roundtrip (display) < 0)
        {
          refused = 1;
          break;
        }
    }

  granted = asked - (refused ? 1 : 0);

  if (refused)
    fprintf (stderr, "client: the compositor took %d pools of %d MB and refused "
                     "the next one, so %d MB is what one connection may hold\n",
             granted, PROBE_BUDGET_POOL_SIZE / (1024 * 1024),
             granted * PROBE_BUDGET_POOL_SIZE / (1024 * 1024));
  else
    fprintf (stderr, "client: the compositor took all %d pools it was sent "
                     "(%d MB) and refused none\n",
             asked, asked * PROBE_BUDGET_POOL_SIZE / (1024 * 1024));

  /* A budget that refuses the first pool is as wrong as one that refuses no
   * pool: a client has to be able to paint at least one frame. */
  if (!refused || granted < 1)
    {
      fprintf (stderr, "client: %d: the shm budget gate FAILED: the compositor "
                       "granted %d of the %d pools it was sent\n",
               __LINE__, granted, asked);
      exit (1);
    }
}

/* -------------------------------------------------------- --pool-shrink */

/* The fd handed to create_pool stays the client's file, and nothing in the
 * protocol takes it away from it: a client that cuts the file down afterwards
 * leaves the compositor holding a mapping over pages that no longer have
 * anything behind them. The frame that reads them is a SIGBUS in whoever maps
 * the surface, which is the host — not an error this connection can be told
 * about first.
 *
 * Two buffers from one pool are what make the read happen. The first commits
 * while the file is whole, so the compositor maps the pool and paints from it;
 * the second commits after the file goes out from under that map. The client
 * stops reading its own mapping before the cut, because a client that faults in
 * the hole it just made would be the corpse the gate reports.
 *
 * The pool object is destroyed in the first round and kept alive in the second.
 * That is the difference the compositor has to handle rather than a detail: a
 * buffer keeps its pool referenced, so the ordinary client that destroys the
 * pool as soon as it has its buffers leaves a mapping with no object to post an
 * error against — dropping the map and keeping the connection is all it may do.
 * With the object still there, the answer a client that broke its own pool earns
 * is a protocol error, and that ends the connection. */
#define PROBE_SHRINK_KEEP 4096

/* Whether the connection died of the commit that followed the cut. */
static int
shrink_a_pool (int keep_pool)
{
  struct wl_shm_pool *pool;
  struct wl_buffer *first, *second;
  size_t bytes;
  int fd, stride;
  void *map;

  stride = device_width * 4;
  bytes = (size_t) stride * device_height * 2;

  fd = memfd_create ("shm-client-shrink", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, (off_t) bytes) < 0)
    {
      fprintf (stderr, "client: cannot make a pool to cut down: %s\n", strerror (errno));
      if (fd >= 0)
        close (fd);
      exit (1);
    }

  map = mmap (NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    {
      fprintf (stderr, "client: cannot map the pool to cut down: %s\n", strerror (errno));
      close (fd);
      exit (1);
    }
  memset (map, 0x42, bytes);

  pool = wl_shm_create_pool (shm, fd, (int32_t) bytes);
  first = wl_shm_pool_create_buffer (pool, 0, device_width, device_height, stride,
                                     WL_SHM_FORMAT_XRGB8888);
  second = wl_shm_pool_create_buffer (pool, stride * device_height, device_width,
                                      device_height, stride, WL_SHM_FORMAT_XRGB8888);
  if (!keep_pool)
    wl_shm_pool_destroy (pool);
  /* This descriptor is not closed at all, and not because the compositor needs
   * it — it has its own copy. It is the hole the ftruncate below is cut with. */

  wl_surface_attach (surface, first, 0, 0);
  wl_surface_damage_buffer (surface, 0, 0, device_width, device_height);
  wl_surface_commit (surface);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: the compositor died or hung up while the pool was "
                       "still whole, so this run proves nothing\n");
      exit (1);
    }

  munmap (map, bytes);
  if (ftruncate (fd, PROBE_SHRINK_KEEP) < 0)
    {
      fprintf (stderr, "client: cannot cut the pool down: %s\n", strerror (errno));
      exit (1);
    }

  wl_surface_attach (surface, second, 0, 0);
  wl_surface_damage_buffer (surface, 0, 0, device_width, device_height);
  wl_surface_commit (surface);

  return wl_display_roundtrip (display) < 0;
}

static void
pool_shrink_probe (void)
{
  int refused;

  refused = shrink_a_pool (0);
  fprintf (stderr, "client: a pool cut to %d bytes after its object was destroyed: %s\n",
           PROBE_SHRINK_KEEP,
           refused ? "the compositor hung up on a pool it can no longer name"
                   : "the compositor kept the connection and read the file itself");
  if (refused)
    {
      fprintf (stderr, "client: %d: pool-shrink gate FAILED: a destroyed pool is "
                       "the normal shape, and a host that cannot post against it "
                       "still must not die with it\n", __LINE__);
      exit (1);
    }

  refused = shrink_a_pool (1);
  fprintf (stderr, "client: a pool cut to %d bytes with its object still alive: %s\n",
           PROBE_SHRINK_KEEP,
           refused ? "the compositor refused it and hung up"
                   : "the COMPOSITOR WENT ON READING IT");
  if (!refused)
    {
      fprintf (stderr, "client: %d: pool-shrink gate FAILED\n", __LINE__);
      exit (1);
    }

  /* The refusal is what ends the run: this connection cannot outlive it, and the
   * host being awake enough to finish its script and save its screenshot is the
   * other half of what the selftest step looks at. */
  exit (0);
}

/* ---------------------------------------------------------- --damage-scale */

/* wl_surface.damage is measured in surface coordinates and
 * wl_surface.set_buffer_scale is what says how big those coordinates are — but
 * both are double-buffered, so the scale a damage request was written under is
 * not necessarily the one the next commit applies. weston keeps the rectangle
 * verbatim and multiplies it at commit by the scale that has just become
 * current; mutter does the same. A compositor that converts on arrival instead
 * reads only part of the new buffer and keeps showing the previous frame where
 * its shadow copy never reached.
 *
 * The two commits below are chosen so all three ways of getting this wrong give
 * a different picture. The buffer is 200x200 twice, so the shadow is reused and
 * the partial copy is really taken; the first frame is solid red with the whole
 * surface damaged. The second is solid green with 50x50 *surface* pixels
 * damaged and the scale set to 2 in the same round:
 *
 *   - converted at commit (correct): buffer (0,0,100,100) turns green, so
 *     (25,25) and (75,75) are green and (150,150) is still red;
 *   - converted on arrival with the old scale 1: only buffer (0,0,50,50) turns
 *     green, so (75,75) is red;
 *   - no partial copy at all, every frame read whole: (150,150) is green.
 *
 * That last point is why the gate samples outside the damaged rectangle as well
 * as inside it: a full re-read would pass a gate that only looked inside. */
#define PROBE_DS_SIZE   200
#define PROBE_DS_DAMAGE  50  /* surface pixels: 100 buffer pixels once scale 2 lands */
#define PROBE_DS_RED   0x00ff0000u
#define PROBE_DS_GREEN 0x0000ff00u

static void
damage_scale_fill (unsigned char *map, size_t offset, uint32_t color)
{
  int stride = PROBE_DS_SIZE * 4;
  int x, y;

  for (y = 0; y < PROBE_DS_SIZE; y++)
    {
      uint32_t *row = (uint32_t *) (map + offset + (size_t) y * stride);

      for (x = 0; x < PROBE_DS_SIZE; x++)
        row[x] = color;
    }
}

static void
damage_scale_probe (void)
{
  struct wl_shm_pool *pool;
  struct wl_buffer *red, *green;
  size_t bytes;
  int fd, stride;
  void *map;

  damage_scale_holding = 1;

  stride = PROBE_DS_SIZE * 4;
  bytes = (size_t) stride * PROBE_DS_SIZE * 2;

  fd = memfd_create ("shm-client-damage-scale", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, (off_t) bytes) < 0)
    {
      fprintf (stderr, "client: cannot make a pool to damage: %s\n", strerror (errno));
      if (fd >= 0)
        close (fd);
      exit (1);
    }

  map = mmap (NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    {
      fprintf (stderr, "client: cannot map the pool to damage: %s\n", strerror (errno));
      close (fd);
      exit (1);
    }

  damage_scale_fill (map, 0, PROBE_DS_RED);
  damage_scale_fill (map, (size_t) stride * PROBE_DS_SIZE, PROBE_DS_GREEN);

  pool = wl_shm_create_pool (shm, fd, (int32_t) bytes);
  red = wl_shm_pool_create_buffer (pool, 0, PROBE_DS_SIZE, PROBE_DS_SIZE, stride,
                                   WL_SHM_FORMAT_XRGB8888);
  green = wl_shm_pool_create_buffer (pool, (int32_t) stride * PROBE_DS_SIZE,
                                     PROBE_DS_SIZE, PROBE_DS_SIZE, stride,
                                     WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);

  /* Frame one: whole surface damaged at scale 1, so the shadow the next commit
   * partly overwrites is red from edge to edge. */
  wl_surface_set_buffer_scale (surface, 1);
  wl_surface_attach (surface, red, 0, 0);
  wl_surface_damage (surface, 0, 0, PROBE_DS_SIZE, PROBE_DS_SIZE);
  wl_surface_commit (surface);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: damage-scale gate FAILED: the compositor hung "
                       "up on a plain scaled commit\n", __LINE__);
      exit (1);
    }

  /* Frame two, and the order is the whole test: the rectangle is named while the
   * surface still thinks it is 200 units wide, then the scale says it was 100. */
  wl_surface_damage (surface, 0, 0, PROBE_DS_DAMAGE, PROBE_DS_DAMAGE);
  wl_surface_set_buffer_scale (surface, 2);
  wl_surface_attach (surface, green, 0, 0);
  wl_surface_commit (surface);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: damage-scale gate FAILED: the compositor hung "
                       "up on damage followed by set_buffer_scale in one commit\n",
               __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: damage-scale probe: 200x200 red, then 50x50 surface "
                   "pixels of a 200x200 green buffer at scale 2 — holding that "
                   "picture for the compositor to be read against\n");
}

/* -------------------------------------------------------- --wrong-object */

/* A request that names an object hands the compositor whatever struct sits
 * under that id. Naming the wrong kind of object is not a complaint a
 * compositor can shrug at: it decides which struct the compositor reads
 * through which fields, and which destroy path frees it while the pointer is
 * still remembered. A zwp_primary_selection_source_v1 is built by the same
 * code as a wl_data_source but torn down by the other branch, which drops the
 * primary offers and frees the struct — so a compositor that took it for a
 * data source goes on reading memory it has already let go of.
 *
 * Each attack gets its own connection, because the answer the client has to
 * see is a protocol error, and that ends the connection that provoked it. The
 * main connection never sends anything wrong: it keeps painting, and the last
 * probe checks it is still being served. */

struct WrongConnection {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_seat *seat;
  struct wl_data_device_manager *manager;
  struct wl_data_device *device;
  struct zwp_primary_selection_device_manager_v1 *primary_manager;
};

static void
wrong_registry_global (void *data, struct wl_registry *registry, uint32_t name,
                       const char *interface, uint32_t version)
{
  struct WrongConnection *conn = data;

  if (!strcmp (interface, "wl_data_device_manager"))
    conn->manager = wl_registry_bind (registry, name,
                                      &wl_data_device_manager_interface,
                                      version < 3 ? version : 3);
  else if (!strcmp (interface, "wl_seat"))
    conn->seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
  else if (!strcmp (interface, "wl_compositor"))
    conn->compositor = wl_registry_bind (registry, name,
                                         &wl_compositor_interface, 1);
  else if (!strcmp (interface, "zwp_primary_selection_device_manager_v1"))
    conn->primary_manager =
      wl_registry_bind (registry, name,
                        &zwp_primary_selection_device_manager_v1_interface,
                        version < 1 ? version : 1);
}

static void
wrong_registry_global_remove (void *data, struct wl_registry *registry,
                              uint32_t name)
{
}

static const struct wl_registry_listener wrong_registry_listener = {
  .global = wrong_registry_global,
  .global_remove = wrong_registry_global_remove,
};

static int
wrong_connection_open (struct WrongConnection *conn)
{
  memset (conn, 0, sizeof *conn);

  conn->display = wl_display_connect (NULL);
  if (!conn->display)
    {
      fprintf (stderr, "client: no second connection for the wrong-object probe\n");
      return 0;
    }

  conn->registry = wl_display_get_registry (conn->display);
  wl_registry_add_listener (conn->registry, &wrong_registry_listener, conn);
  wl_display_roundtrip (conn->display);

  if (!conn->manager || !conn->seat || !conn->compositor || !conn->primary_manager)
    {
      fprintf (stderr, "client: wrong-object probe globals manager=%d seat=%d "
                       "compositor=%d primary=%d\n", !!conn->manager, !!conn->seat,
                       !!conn->compositor, !!conn->primary_manager);
      wl_display_disconnect (conn->display);
      conn->display = NULL;
      return 0;
    }

  /* Created before the attack, so the compositor has a device of this
   * connection to answer on. */
  conn->device = wl_data_device_manager_get_data_device (conn->manager, conn->seat);
  return 1;
}

static void
wrong_connection_close (struct WrongConnection *conn)
{
  /* The proxies of a connection the compositor hung up on are not worth
   * destroying: the display owns what is left of them. */
  if (conn->display)
    wl_display_disconnect (conn->display);
  conn->display = NULL;
}

/* The frame no client wrapper will send for you. Handing
 * wl_data_device_set_selection() a zwp_primary_selection_source_v1 is refused
 * inside libwayland — measured, on this very probe: "invalid object (8), type
 * (zwp_primary_selection_source_v1), message set_selection(?ou)" — so the
 * request never reaches the compositor and a gate built on the wrapper would be
 * grading the client library. The wire is written here instead, one whole
 * message after a flush, which is what a fuzzer or a determined client does:
 * the compositor is then the only one left who can say no.
 *
 * set_selection is opcode 1 of wl_data_device and takes two arguments, the
 * source id and the serial; a wayland frame is size, sender id, opcode and then
 * the arguments, all in native order. */
static void
wrong_selection_of (struct WrongConnection *conn, uint32_t source_id,
                    const char *what)
{
  struct { uint32_t size, id, opcode, source, serial; } message = {
    sizeof message,
    wl_proxy_get_id ((struct wl_proxy *) conn->device),
    1,
    source_id,
    1
  };
  ssize_t written;
  int refused;

  wl_display_flush (conn->display);
  written = write (wl_display_get_fd (conn->display), &message, sizeof message);
  if (written != (ssize_t) sizeof message)
    {
      fprintf (stderr, "client: %d: cannot send the raw frame: %s\n",
               __LINE__, strerror (errno));
      exit (1);
    }

  refused = wl_display_roundtrip (conn->display) < 0;

  fprintf (stderr, "client: named a %s where a wl_data_source was expected: %s\n",
           what, refused ? "the compositor refused it and hung up"
                         : "the COMPOSITOR ACCEPTED IT");

  if (!refused)
    {
      fprintf (stderr, "client: %d: wrong-object gate FAILED on a %s\n",
               __LINE__, what);
      exit (1);
    }

  wrong_connection_close (conn);
}

static void
wrong_object_probe (void)
{
  struct WrongConnection conn;
  struct zwp_primary_selection_source_v1 *primary;
  struct wl_surface *surface;
  struct wl_data_source *source;

  if (!wrong_connection_open (&conn))
    exit (1);
  primary = zwp_primary_selection_device_manager_v1_create_source (conn.primary_manager);
  wrong_selection_of (&conn,
                      wl_proxy_get_id ((struct wl_proxy *) primary),
                      "zwp_primary_selection_source_v1");

  if (!wrong_connection_open (&conn))
    exit (1);
  surface = wl_compositor_create_surface (conn.compositor);
  wrong_selection_of (&conn,
                      wl_proxy_get_id ((struct wl_proxy *) surface),
                      "wl_surface");

  /* Both attackers are gone; the client that has been painting the whole time
   * is still a client. Its selection has to be taken, not hung up on. */
  ensure_data_device ();
  source = data_manager ?
    wl_data_device_manager_create_data_source (data_manager) : NULL;
  if (!source || !data_device)
    {
      fprintf (stderr, "client: no data device to prove the compositor survived\n");
      exit (1);
    }

  wl_data_source_offer (source, "text/plain");
  wl_data_source_add_listener (source, &data_source_listener, source);
  wl_data_device_set_selection (data_device, source, 1);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: wrong-object gate FAILED: the compositor hung "
                       "up on the client that sent nothing wrong\n", __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: the compositor refused both wrong objects and is still "
                   "serving the client that painted them\n");

  /* No exit here, unlike the modes that answer on their own connection: both
   * attackers were second connections, so this client still has a window the
   * compositor is drawing. Going back to the frame loop makes the rest of the
   * script grade that — a run whose picture vanishes after the attacks is the
   * bug this probe exists to catch. */
}

/* ---------------------------------------------------------- --empty-copy */

/* The primary-selection half. The compositor reads this source the moment it is
 * told about it, exactly as it reads the clipboard one, and the answer here is a
 * descriptor closed without a byte written to it. */
static void
primary_source_send (void *data, struct zwp_primary_selection_source_v1 *source,
                     const char *mime_type, int32_t fd)
{
  primary_copy_reads++;

  fprintf (stderr, "client: %d: the primary selection asked for %s, answering "
                   "with nothing\n", getpid (), mime_type);
  close (fd);
}

static void
primary_source_cancelled (void *data, struct zwp_primary_selection_source_v1 *source)
{
  zwp_primary_selection_source_v1_destroy (source);
  primary_source = NULL;
}

static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
  .send = primary_source_send,
  .cancelled = primary_source_cancelled,
};

static void
empty_copy_probe (void)
{
  struct wl_data_source *source;

  ensure_data_device ();
  if (!data_device || !data_manager)
    {
      fprintf (stderr, "client: %d: no data device, the empty copy cannot be sent\n",
               __LINE__);
      exit (1);
    }
  /* Both halves have to be askable or the run is not grading what it says it is:
   * a probe that quietly skips the part it cannot send is a green that proves
   * nothing, which is worse than a red. */
  if (!primary_manager || !seat)
    {
      fprintf (stderr, "client: %d: no primary selection to copy into "
                       "(manager=%d seat=%d)\n", __LINE__,
               !!primary_manager, !!seat);
      exit (1);
    }

  source = wl_data_device_manager_create_data_source (data_manager);
  wl_data_source_offer (source, "text/plain;charset=utf-8");
  wl_data_source_add_listener (source, &data_source_listener, source);
  wl_data_device_set_selection (data_device, source, 0);

  primary_device =
    zwp_primary_selection_device_manager_v1_get_device (primary_manager, seat);
  primary_source =
    zwp_primary_selection_device_manager_v1_create_source (primary_manager);
  zwp_primary_selection_source_v1_offer (primary_source, "text/plain;charset=utf-8");
  zwp_primary_selection_source_v1_add_listener (primary_source,
                                                &primary_source_listener,
                                                primary_source);
  zwp_primary_selection_device_v1_set_selection (primary_device, primary_source, 0);

  fprintf (stderr, "client: %d: took an empty clipboard and primary selection\n",
           getpid ());

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: empty-copy gate FAILED: the compositor hung "
                       "up on a payload that was empty\n", __LINE__);
      exit (1);
    }
}

/* --------------------------------------------------------- --popup-depth */

/* An xdg_popup becomes a link on its parent's child list, and the host then
 * walks that tree with recursion — to place a menu against the window above it,
 * and to build the stack it draws. The depth of the recursion is therefore chosen
 * by the client and paid for out of the host's C stack, which is the shape of bug
 * where one unprivileged connection can take the whole windowing system down.
 *
 * The chain is built one popup at a time, each followed by a roundtrip, because
 * the request that hits the cap kills the connection and a batch of them would
 * not say where the cap actually sat. Like the wrong-object attacks, the attacker
 * lives on a connection of its own: the refusal is the wanted answer, and it
 * must not end the client that is painting. */

struct DepthConnection {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct xdg_wm_base *wm_base;
};

static void
depth_registry_global (void *data, struct wl_registry *registry, uint32_t name,
                       const char *interface, uint32_t version)
{
  struct DepthConnection *conn = data;

  if (!strcmp (interface, "wl_compositor"))
    conn->compositor = wl_registry_bind (registry, name,
                                         &wl_compositor_interface, 1);
  else if (!strcmp (interface, "xdg_wm_base"))
    {
      conn->wm_base = wl_registry_bind (registry, name, &xdg_wm_base_interface,
                                        version < 3 ? version : 3);
      xdg_wm_base_add_listener (conn->wm_base, &wm_base_listener, NULL);
    }
}

static void
depth_registry_global_remove (void *data, struct wl_registry *registry,
                              uint32_t name)
{
}

static const struct wl_registry_listener depth_registry_listener = {
  .global = depth_registry_global,
  .global_remove = depth_registry_global_remove,
};

/* The root is configured like any toplevel, and an unacked configure is a
 * complaint the compositor is entitled to make about a probe that has not done
 * anything wrong yet. */
static void
depth_configure (void *data, struct xdg_surface *xdg, uint32_t serial)
{
  xdg_surface_ack_configure (xdg, serial);
}

static const struct xdg_surface_listener depth_surface_listener = {
  .configure = depth_configure,
};

static void
popup_depth_probe (void)
{
  struct DepthConnection conn;
  struct wl_surface *root_surface;
  struct xdg_surface *parent;
  int refused_at = 0;
  int attempt;

  memset (&conn, 0, sizeof conn);
  conn.display = wl_display_connect (NULL);
  if (!conn.display)
    {
      fprintf (stderr, "client: %d: no second connection for the popup-depth "
                       "probe\n", __LINE__);
      exit (1);
    }

  conn.registry = wl_display_get_registry (conn.display);
  wl_registry_add_listener (conn.registry, &depth_registry_listener, &conn);
  wl_display_roundtrip (conn.display);

  if (!conn.compositor || !conn.wm_base)
    {
      fprintf (stderr, "client: %d: popup-depth probe globals compositor=%d "
                       "xdg_wm_base=%d\n", __LINE__, !!conn.compositor,
                       !!conn.wm_base);
      exit (1);
    }

  root_surface = wl_compositor_create_surface (conn.compositor);
  parent = xdg_wm_base_get_xdg_surface (conn.wm_base, root_surface);
  xdg_surface_add_listener (parent, &depth_surface_listener, NULL);
  xdg_surface_get_toplevel (parent);
  wl_surface_commit (root_surface);

  if (wl_display_roundtrip (conn.display) < 0)
    {
      fprintf (stderr, "client: %d: popup-depth gate FAILED: the compositor hung "
                       "up on the plain toplevel the chain hangs off\n", __LINE__);
      exit (1);
    }

  for (attempt = 1; attempt <= POPUP_DEPTH_ATTEMPT; attempt++)
    {
      struct wl_surface *surface =
        wl_compositor_create_surface (conn.compositor);
      struct xdg_surface *xdg =
        xdg_wm_base_get_xdg_surface (conn.wm_base, surface);
      struct xdg_positioner *positioner =
        xdg_wm_base_create_positioner (conn.wm_base);
      int refused;

      /* A positioner has to state a size and an anchor rectangle or the refusal
       * that comes back is the one about the positioner, not the one about the
       * depth — and the anchor moves so no two levels place identically. */
      xdg_positioner_set_size (positioner, 40, 20);
      xdg_positioner_set_anchor_rect (positioner, attempt, attempt, 8, 8);
      xdg_positioner_set_gravity (positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
      xdg_surface_get_popup (xdg, parent, positioner);
      xdg_positioner_destroy (positioner);

      refused = wl_display_roundtrip (conn.display) < 0;
      if (refused)
        {
          refused_at = attempt;
          break;
        }

      parent = xdg;
    }

  if (!refused_at)
    {
      fprintf (stderr, "client: %d: popup-depth gate FAILED: the compositor took "
                       "%d nested popups, so nothing bounds the depth its tree "
                       "walks recurse through\n", __LINE__, POPUP_DEPTH_ATTEMPT);
      exit (1);
    }

  /* Asked for exactly, not merely "early enough": a chain refused at popup 3
   * would be a refusal for some other reason, and the run would be grading a
   * different bug than the one it claims to. The 33rd popup is the first whose
   * parent already carries the full cap of links. */
  if (refused_at != POPUP_DEPTH_LIMIT + 1)
    {
      fprintf (stderr, "client: %d: popup-depth gate FAILED: refused at popup %d, "
                       "but the cap is %d links, so the first refusal should be "
                       "popup %d\n", __LINE__, refused_at, POPUP_DEPTH_LIMIT,
               POPUP_DEPTH_LIMIT + 1);
      exit (1);
    }

  fprintf (stderr, "client: %d: the chain was refused at popup %d; the cap is "
                   "%d links\n", __LINE__, refused_at, POPUP_DEPTH_LIMIT);

  wl_display_disconnect (conn.display);

  /* The attacker and its chain are gone; the client that never sent anything
   * wrong is still a client, and going back to the frame loop lets the rest of
   * the script grade that its picture is still being drawn. */
  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: popup-depth gate FAILED: the compositor hung "
                       "up on the client that painted through the attack\n",
               __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: %d: the compositor refused the deep chain and is "
                   "still serving the client that painted it\n", __LINE__);
}

/* --------------------------------------------------- --drag-fake-serial */

/* Every slot a version 3 source can be answered with, because a compositor that
 * took this drag would start sending them and libwayland ends the connection over
 * an event whose slot the client left NULL — which would hide the answer the gate
 * is actually grading. Only .cancelled is the one wanted. */
static void
drag_fake_target (void *data, struct wl_data_source *source, const char *mime_type)
{
  fprintf (stderr, "client: %d: a reader wants %s of the drag with no grab\n",
           __LINE__, mime_type ? mime_type : "(nothing)");
}

static void
drag_fake_send (void *data, struct wl_data_source *source,
               const char *mime_type, int fd)
{
  fprintf (stderr, "client: %d: the drag with no grab was read for %s\n",
           __LINE__, mime_type);
  close (fd);
}

static void
drag_fake_source_cancelled (void *data, struct wl_data_source *source)
{
  drag_fake_cancelled = 1;
  wl_data_source_destroy (source);
}

static void
drag_fake_drop_performed (void *data, struct wl_data_source *source)
{
  fprintf (stderr, "client: %d: the drag with no grab was let go\n", __LINE__);
}

static void
drag_fake_dnd_finished (void *data, struct wl_data_source *source)
{
  fprintf (stderr, "client: %d: the drag with no grab was finished\n", __LINE__);
}

static void
drag_fake_action (void *data, struct wl_data_source *source, uint32_t action)
{
  fprintf (stderr, "client: %d: the drag with no grab got action 0x%x\n",
           __LINE__, action);
}

static const struct wl_data_source_listener drag_fake_listener = {
  .target = drag_fake_target,
  .send = drag_fake_send,
  .cancelled = drag_fake_source_cancelled,
  .dnd_drop_performed = drag_fake_drop_performed,
  .dnd_finished = drag_fake_dnd_finished,
  .action = drag_fake_action,
};

static void
drag_fake_probe (void)
{
  struct wl_data_source *source;
  int enters_before = drag_enters;

  ensure_data_device ();
  if (!data_device || !data_manager)
    {
      fprintf (stderr, "client: %d: no data device, the drag with no grab cannot "
                       "be sent\n", __LINE__);
      exit (1);
    }

  source = wl_data_device_manager_create_data_source (data_manager);
  wl_data_source_offer (source, DRAG_FAKE_MIME);
  wl_data_source_add_listener (source, &drag_fake_listener, source);

  wl_data_device_start_drag (data_device, source, surface, NULL,
                             DRAG_FAKE_SERIAL);
  fprintf (stderr, "client: %d: asked for a drag over a painted surface citing "
                   "serial %u, which no press ever carried\n", __LINE__,
           DRAG_FAKE_SERIAL);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: drag-fake gate FAILED: the compositor hung up "
                       "on a drag with no press behind it — the answer the "
                       "protocol wants is the source cancelled, not the client "
                       "dropped\n", __LINE__);
      exit (1);
    }

  if (!drag_fake_cancelled)
    {
      fprintf (stderr, "client: %d: drag-fake gate FAILED: nothing cancelled a "
                       "start_drag that cited serial %u, which the compositor "
                       "never gave out in a button press%s\n", __LINE__,
               DRAG_FAKE_SERIAL,
               drag_enters != enters_before ?
                 ", and the drag it started then entered this surface" : "");
      exit (1);
    }

  /* Two answers to one request is as wrong as none: the cancelled says there is
   * no drag, and an enter is a drag moving over something. */
  if (drag_enters != enters_before)
    {
      fprintf (stderr, "client: %d: drag-fake gate FAILED: the source was "
                       "cancelled and a drag arrived anyway, so the seat is now "
                       "holding a pointer for a drag it refused\n", __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: %d: the drag with no grab was refused with cancelled "
                   "and no surface was entered\n", __LINE__);
}

/* ------------------------------------------------------ --drag-no-source */

/* A page can start a drag and hand nothing over: wayland.xml spells out the
 * shape — "If source is NULL, enter, leave and motion events are sent only to
 * the client that initiated the drag" — and the compositor sends
 * wl_data_device.enter with no offer at all. That is legal, but it costs the
 * drag its ending: the request that normally retires a drop is
 * wl_data_offer.finish, and there is no offer to make it on. Everything else
 * that keeps a drag alive goes through the offer too, so if the drop does not
 * retire it, nothing ever will.
 *
 * The consequence is a picture, not a leak. The surface the client named as the
 * drag's icon is marked as one, and only the death of the drag unmarks it — and
 * an icon is painted while a drag carries it and never again afterwards. So a
 * client that drags once with no payload and then puts a window on that same
 * surface has a window that is mapped, configured and full of pixels and is
 * never drawn.
 *
 * The half graded here is the wire: the enter has to arrive with no offer (a
 * sourceless drag that got one is a different bug), a drop has to arrive (a drag
 * cancelled on the way leaves nothing to conclude, and the gate would be grading
 * a run that never happened), and the connection has to survive both. The half
 * graded by seat-selftest is that the ex-icon surface, once it is a toplevel, is
 * in the stack the host is told to draw. */
#define NS_SIZE      120
#define NS_MAGENTA 0x00ff00ffu
#define NS_APP_ID    "cefnest-drag-no-source"

static struct wl_surface *ns_surface;
static struct wl_buffer *ns_buffer;
static struct xdg_surface *ns_xdg;
static struct xdg_toplevel *ns_toplevel;

static void
ns_xdg_configure (void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
  xdg_surface_ack_configure (xdg_surface, serial);

  /* A toplevel is only shown once it has content under the new role, so the
   * buffer the surface carried as an icon goes on being attached. */
  wl_surface_attach (ns_surface, ns_buffer, 0, 0);
  wl_surface_damage_buffer (ns_surface, 0, 0, NS_SIZE, NS_SIZE);
  wl_surface_commit (ns_surface);
  drag_ns_configured++;
}

static const struct xdg_surface_listener ns_xdg_listener = {
  .configure = ns_xdg_configure,
};

static void
ns_drag_start (uint32_t serial)
{
  struct wl_shm_pool *pool;
  uint32_t *pixels;
  int fd, i;

  ns_surface = wl_compositor_create_surface (compositor);

  pixels = map_pool (NS_SIZE * NS_SIZE * 4, &fd);
  pool = wl_shm_create_pool (shm, fd, NS_SIZE * NS_SIZE * 4);
  ns_buffer = wl_shm_pool_create_buffer (pool, 0, NS_SIZE, NS_SIZE, NS_SIZE * 4,
                                         WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);

  for (i = 0; i < NS_SIZE * NS_SIZE; i++)
    pixels[i] = NS_MAGENTA;

  wl_surface_attach (ns_surface, ns_buffer, 0, 0);
  wl_surface_damage_buffer (ns_surface, 0, 0, NS_SIZE, NS_SIZE);
  wl_surface_commit (ns_surface);

  /* No source, and this new surface as the icon: the shape the protocol says
   * needs no offer, asked for with the serial of the press this handler is
   * running in, so the seat's grab rule is satisfied and the drag is real. */
  wl_data_device_start_drag (data_device, NULL, surface, ns_surface, serial);

  if (wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: the compositor hung "
                       "up on a sourceless start_drag\n", __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: %d: started a drag with no source, icon is a surface of "
                   "our own, serial %u\n", __LINE__, serial);
}

static void
ns_check (void)
{
  if (!drag_ns_enters)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: nothing ever entered "
                       "the surface this drag started on, so the drop that was "
                       "supposed to conclude it was never offered to anybody\n",
               __LINE__);
      exit (1);
    }

  if (drag_ns_offers)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: the compositor "
                       "introduced a wl_data_offer for a drag that had no source, so "
                       "this run graded a payload that does not exist\n", __LINE__);
      exit (1);
    }

  if (!drag_ns_drops)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: the pointer let go "
                       "over this client's own surface and no wl_data_device.drop "
                       "arrived\n", __LINE__);
      exit (1);
    }

  /* The drop is done with. Make the icon into a window: a surface that was only
   * ever an icon has no role, so this is the first xdg object it gets. */
  ns_xdg = xdg_wm_base_get_xdg_surface (wm_base, ns_surface);
  xdg_surface_add_listener (ns_xdg, &ns_xdg_listener, NULL);
  ns_toplevel = xdg_surface_get_toplevel (ns_xdg);
  xdg_toplevel_set_title (ns_toplevel, "drag icon become a window");
  xdg_toplevel_set_app_id (ns_toplevel, NS_APP_ID);
  wl_surface_commit (ns_surface);

  /* Twice: the first roundtrip brings the configure, whose listener answers with
   * a commit; the second lets that commit become a frame the compositor has
   * imported. */
  if (wl_display_roundtrip (display) < 0 ||
      wl_display_roundtrip (display) < 0)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: the compositor hung "
                       "up on the ex-icon surface being given a toplevel role after "
                       "the drop\n", __LINE__);
      exit (1);
    }

  if (!drag_ns_configured)
    {
      fprintf (stderr, "client: %d: drag-no-source gate FAILED: the toplevel built on "
                       "the drag's icon was never configured, so the selftest has no "
                       "window to look for\n", __LINE__);
      exit (1);
    }

  fprintf (stderr, "client: %d: the sourceless drag entered with no offer, dropped, "
                   "and its icon became a configured toplevel (%d)\n",
           __LINE__, drag_ns_configured);
}

int
main (int argc, char *argv[])
{
  struct wl_registry *registry;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--dmabuf"))
        use_dmabuf = 1;
      else if (!strcmp (argv[i], "--fullscreen"))
        use_fullscreen = 1;
      else if (!strcmp (argv[i], "--copy"))
        use_copy = 1;
      else if (!strcmp (argv[i], "--dnd-requests"))
        use_dnd_requests = 1;
      else if (!strcmp (argv[i], "--drag-out"))
        use_drag_out = 1;
      else if (!strcmp (argv[i], "--drag-in"))
        use_drag_in = 1;
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
      else if (!strcmp (argv[i], "--cursor-shape"))
        use_cursor_shape = 1;
      else if (!strcmp (argv[i], "--cursor-surface"))
        use_cursor_surface = 1;
      else if (!strcmp (argv[i], "--popup"))
        use_popup = 1;
      else if (!strcmp (argv[i], "--popup-reposition"))
        {
          use_popup = 1;
          use_popup_reposition = 1;
        }
      else if (!strcmp (argv[i], "--refused"))
        use_refused = 1;
      else if (!strcmp (argv[i], "--pool-budget"))
        use_pool_budget = 1;
      else if (!strcmp (argv[i], "--wrong-object"))
        use_wrong_object = 1;
      else if (!strcmp (argv[i], "--pool-shrink"))
        use_pool_shrink = 1;
      else if (!strcmp (argv[i], "--empty-copy"))
        use_empty_copy = 1;
      else if (!strcmp (argv[i], "--popup-depth"))
        use_popup_depth = 1;
      else if (!strcmp (argv[i], "--damage-scale"))
        use_damage_scale = 1;
      else if (!strcmp (argv[i], "--drag-no-source"))
        use_drag_no_source = 1;
      else if (!strcmp (argv[i], "--drag-fake-serial"))
        use_drag_fake_serial = 1;
      else if (!strcmp (argv[i], "--second-window"))
        use_second_window = 1;
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

  /* exit(0) rather than a return, and both probes above judge their own answer:
   * reaching here means the compositor refused what it had to refuse, and the
   * oversized pool took this connection with it. Painting cannot follow, and a
   * walk off the end of main would report an unjudged run as a pass. */
  if (use_refused)
    {
      probe_refused_imports ();
      probe_oversized_pool ();
      exit (0);
    }

  /* Its own mode because the answer to the pool that goes over the limit ends
   * this connection, and nothing after it could be asked. */
  if (use_pool_budget)
    {
      probe_pool_budget ();
      exit (0);
    }

  fprintf (stderr, "client: painting %s buffers, %d dmabuf formats and %d modifiers "
                   "advertised, render node %s\n", use_dmabuf ? "dmabuf" : "shm",
           advertised_formats, advertised_modifiers, render_node);
  fprintf (stderr, "client: viewporter=%p fractional_scale_manager=%p data_manager=%p\n",
           (void *) viewporter, (void *) fractional_manager, (void *) data_manager);
  if (!data_manager && (use_copy || use_paste))
    fprintf (stderr, "client: no wl_data_device_manager, clipboard test impossible\n");
  if (use_dnd_requests && !data_manager)
    fprintf (stderr, "client: no wl_data_device_manager, the v3 requests do not exist\n");
  if (use_drag_out && !data_manager)
    fprintf (stderr, "client: no wl_data_device_manager, no drag can be started\n");
  if (use_drag_in && !data_manager)
    fprintf (stderr, "client: no wl_data_device_manager, no drag can be taken\n");
  if (use_text_input && !text_input_manager)
    fprintf (stderr, "client: no zwp_text_input_manager_v3, composition impossible\n");

  /* Either global could have arrived after the other, and both have to be in
   * hand before a text input can be created. */
  ensure_text_input ();
  ensure_cursor_shape ();

  if (use_cursor_shape && !cursor_shape_manager)
    fprintf (stderr, "client: no wp_cursor_shape_manager_v1, no shapes to ask for\n");

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

  /* After the first toplevel commits: this way the window the run starts from
   * is the one the compositor already knows about when the second arrives. */
  if (use_second_window)
    second_create ();

  while (wl_display_dispatch (display) >= 0)
    {
      /* Dispatching may have just brought the mime list for a new selection;
       * the ask happens out here, not from inside the listener. */
      if (paste_pending && n_selection_mimes > 0)
        {
          paste_pending = 0;
          do_paste ();
        }

      /* Same reason for the two halves of a drag taken in from the host: the
       * read blocks, so it goes out here where nothing is being answered. */
      if (drag_in_eager_pending)
        {
          drag_in_eager_pending = 0;
          do_drag_in_read ("entering the surface, before the pointer let go,");
        }
      if (drag_in_read_pending)
        {
          drag_in_read_pending = 0;
          do_drag_in_read ("after the pointer let go,");
          do_drag_in_conclude ();
        }
    }

  /* The loop only ends when the socket did: a protocol error from the
   * compositor, or the process behind it going away. Either is a complaint about
   * the compositor, not a pass, and every mode that ends its own connection on
   * purpose says so where it breaks it (see --refused and --pool-shrink). */
  fprintf (stderr, "client: %d: the compositor hung up after %u frames\n",
           __LINE__, frames);
  return 1;
}
