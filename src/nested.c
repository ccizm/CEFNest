#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xkbcommon/xkbcommon.h>

#include "fractional-scale-server-protocol.h"
#include "linux-dmabuf-server-protocol.h"
#include "text-input-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

/* One seat, one pointer, one keyboard: the embedding widget is the only device
 * the nested compositor knows about. */
#define NESTED_SEAT_NAME       "seat0"
#define NESTED_SEAT_VERSION    5
#define NESTED_POINTER_VERSION 5
#define NESTED_KEYBOARD_VERSION 4

/* Version 4 adds zwp_linux_dmabuf_feedback_v1, which is how a client learns
 * which device to allocate on — the only thing a nested compositor can answer
 * is "the render node I import on", but CEF asks, so advertise it. Version 5
 * adds nothing we need, and binding the newest keeps us honest about the
 * deprecated format/modifier events, which stay for old clients. */
#define NESTED_DMABUF_VERSION   5
#define NESTED_DMABUF_MAX_PLANES 4

/* ------------------------------------------------------------------ *
 * Types                                                              *
 * ------------------------------------------------------------------ */

typedef struct _Pool Pool;
typedef struct _Buffer Buffer;
typedef struct _BufferUser BufferUser;
typedef struct _FrameCallback FrameCallback;
typedef struct _OutputResource OutputResource;
typedef struct _SeatClient SeatClient;
typedef struct _Params Params;
typedef struct _Viewport Viewport;
typedef struct _FractionalScale FractionalScale;
typedef struct _DataSource DataSource;
typedef struct _DataOffer DataOffer;
typedef struct _DataDevice DataDevice;
typedef struct _ClipboardTransfer ClipboardTransfer;
typedef struct _TextInputState TextInputState;
typedef struct _TextInput TextInput;

struct _Pool {
  int fd;
  size_t size;
  guint refcount;
};

/* A (fourcc, modifier) pair advertised through zwp_linux_dmabuf_v1. */
typedef struct {
  uint32_t fourcc;
  uint64_t modifier;
} DmabufFormat;

struct _Buffer {
  Pool *pool;                 /* shm only */
  struct wl_resource *resource;
  struct wl_list users;       /* BufferUser.link */
  int32_t offset;             /* shm only */
  int32_t width;
  int32_t height;
  int32_t stride;             /* shm only */
  uint32_t format;            /* shm only */

  /* dmabuf planes. The fds arrive owned by us; once a texture has been built
   * they belong to that texture's destroy notify, because the texture may well
   * outlive the wl_buffer the client attached. */
  gboolean is_dmabuf;
  gboolean fds_transferred;
  int fd[NESTED_DMABUF_MAX_PLANES];
  uint32_t plane_offset[NESTED_DMABUF_MAX_PLANES];
  uint32_t plane_stride[NESTED_DMABUF_MAX_PLANES];
  int n_planes;
  uint32_t fourcc;
  uint64_t modifier;
  GdkTexture *texture;        /* dmabuf only: buffer contents are immutable */
};

/* A reference slot held by a surface. Keeping the list node inside the
 * surface (rather than a refcount on the buffer) lets destroying a client's
 * wl_buffer clear every pointer to it without touching freed surfaces. */
struct _BufferUser {
  Buffer *buffer;
  NestedSurface *surface;
  struct wl_list link;
};

struct _FrameCallback {
  struct wl_resource *resource;
  NestedSurface *surface;
  struct wl_list link;
};

struct _OutputResource {
  NestedCompositor *nested;
  struct wl_resource *resource;
  struct wl_list link;
};

/* One wl_seat resource per bound client, plus the pointer and keyboard objects
 * it asked for. There is a single focus surface, but each client may only be
 * told about it when the surface is its own: naming another client's object is
 * a protocol error. *_inside records the enter we actually sent, so motion and
 * keys reach exactly the clients that can act on them. */
struct _SeatClient {
  NestedCompositor *nested;
  struct wl_resource *seat;
  struct wl_resource *pointer;
  struct wl_resource *keyboard;
  gboolean pointer_inside;
  gboolean keyboard_inside;
  struct wl_list link;
};

/* zwp_linux_buffer_params_v1: the planes collected by add() until create() or
 * create_immed() turns them into a wl_buffer. */
struct _Params {
  NestedCompositor *nested;
  struct wl_resource *resource;
  int fd[NESTED_DMABUF_MAX_PLANES];
  uint32_t offset[NESTED_DMABUF_MAX_PLANES];
  uint32_t stride[NESTED_DMABUF_MAX_PLANES];
  uint64_t modifier[NESTED_DMABUF_MAX_PLANES];
  gboolean has_plane[NESTED_DMABUF_MAX_PLANES];
  gboolean used;
};

/* wp_viewport: cropping and scaling state lives on the surface, the object
 * itself is only the request channel, so it needs no fields beyond the two
 * pointers that tie a request back to its surface. */
struct _Viewport {
  NestedSurface *surface;
  struct wl_resource *resource;
};

/* wp_fractional_scale_v1: one object per surface a client asked about, so a
 * scale change is pushed to exactly the surfaces that care. */
struct _FractionalScale {
  NestedSurface *surface;
  struct wl_resource *resource;
  struct wl_list link;
};

struct _NestedSurface {
  GObject parent_instance;

  NestedCompositor *nested;
  struct wl_list nested_link;
  gboolean in_list;

  struct wl_resource *wl_surface;
  struct wl_resource *xdg_surface;
  struct wl_resource *toplevel;
  struct wl_resource *popup;
  struct wl_resource *viewport;
  struct wl_resource *fractional;
  struct wl_list fractional_links;    /* FractionalScale.link */

  struct wl_list frame_callbacks;
  BufferUser pending_user;
  gboolean pending_attach;

  /* Crop and scale state, in "natural" coordinates: buffer pixels divided by
   * the buffer scale, which is what wp_viewport.set_source uses. The pending
   * copy becomes current on wl_surface.commit, like the buffer. */
  struct { double x, y, width, height; } pending_src, src;
  struct { int width, height; } pending_dst, dst;

  GdkTexture *texture;
  int width;                 /* surface-local size, after crop and scale */
  int height;
  int natural_width;         /* buffer size divided by scale */
  int natural_height;
  int geom_width;
  int geom_height;
  int32_t scale;             /* wl_surface.set_buffer_scale */

  char *app_id;
  char *title;

  uint32_t configure_serial;
  unsigned int is_toplevel : 1;
  unsigned int is_popup : 1;
  unsigned int mapped : 1;
};

struct _NestedCompositor {
  GObject parent_instance;

  struct wl_display *display;
  struct wl_event_loop *loop;
  GSource *source;
  gulong source_id;
  guint frame_timer_id;
  char *socket_name;

  struct wl_global *global_compositor;
  struct wl_global *global_shm;
  struct wl_global *global_output;
  struct wl_global *global_seat;
  struct wl_global *global_xdg_wm_base;
  struct wl_global *global_dmabuf;
  struct wl_global *global_viewporter;
  struct wl_global *global_fractional;
  struct wl_global *global_data;
  struct wl_global *global_text_input;

  struct wl_list outputs;       /* OutputResource.link */
  struct wl_list surfaces;      /* NestedSurface.nested_link */
  struct wl_list seat_clients;  /* SeatClient.link */
  struct wl_list text_inputs;   /* TextInput.link */

  /* Format list for zwp_linux_dmabuf_v1, probed from the render node. Empty
   * means no dmabuf global at all: clients keep using wl_shm. */
  struct gbm_device *gbm;
  GArray *dmabuf_formats;       /* DmabufFormat */

  /* Feedback shares one immutable memfd holding the format table, in the
   * 16-byte (format, padding, modifier) layout the protocol mandates, and the
   * dev_t of the node we import on. */
  int dmabuf_table_fd;
  size_t dmabuf_table_size;
  uint64_t dmabuf_device;

  /* Clipboard bridge. selection_source is a client that owns the selection and
   * can be asked for its contents; clipboard_bytes is what the GTK clipboard
   * holds, buffered so a client can be served without blocking the loop. */
  struct wl_list data_devices;      /* DataDevice.link */
  DataSource *selection_source;
  ClipboardTransfer *clipboard_transfer;
  GdkClipboard *clipboard;          /* the embedding widget's, may be NULL */
  gulong clipboard_handler;         /* notify::content */
  GBytes *clipboard_bytes;
  char *clipboard_mime;
  guint clipboard_echo;             /* ignores the notify our own write causes */

  int width;                  /* configured size, in widget logical pixels */
  int height;
  double scale;               /* device pixels per logical pixel */
  uint32_t serial;
  uint64_t frame_count;

  NestedSurface *primary;

  /* Injected input state. The focus follows "primary"; xkb only tracks the
   * modifier bits, so the keymap is compiled but never used to resolve
   * keyvals — the client does that with the keymap we hand it. */
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  char *keymap_text;
  size_t keymap_size;
  GArray *pressed_keys;      /* uint32_t evdev codes */
  NestedSurface *pointer_focus;
  NestedSurface *keyboard_focus;
  /* The one text input the widget should compose for: entered, and enabled by
   * its client. NULL means keys go to the client as plain wl_keyboard.key. */
  TextInput *active_input;
  double pointer_x;
  double pointer_y;
  unsigned int pointer_wanted : 1;
  unsigned int keyboard_wanted : 1;
};

enum {
  PROP_0,
  PROP_SURFACE,
  N_PROPS,
};

enum {
  SIGNAL_FRAME,
  SIGNAL_TEXT_INPUT_STATE,
  N_SIGNALS,
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ *
 * Interface implementations, declared up front so the object cast      *
 * helpers can validate them.                                         *
 * ------------------------------------------------------------------ */

static const struct wl_compositor_interface compositor_impl;
static const struct wl_region_interface region_impl;
static const struct wl_surface_interface surface_impl;
static const struct wl_shm_interface shm_impl;
static const struct wl_shm_pool_interface shm_pool_impl;
static const struct wl_buffer_interface buffer_impl;
static const struct wl_output_interface output_impl;
static const struct wl_seat_interface seat_impl;
static const struct wl_pointer_interface pointer_impl;
static const struct wl_keyboard_interface keyboard_impl;
static const struct xdg_wm_base_interface xdg_wm_base_impl;
static const struct xdg_positioner_interface xdg_positioner_impl;
static const struct xdg_surface_interface xdg_surface_impl;
static const struct xdg_toplevel_interface xdg_toplevel_impl;
static const struct xdg_popup_interface xdg_popup_impl;
static const struct wp_viewporter_interface viewporter_impl;
static const struct wp_viewport_interface viewport_impl;
static const struct wp_fractional_scale_manager_v1_interface fractional_scale_manager_impl;
static const struct wp_fractional_scale_v1_interface fractional_scale_impl;

/* ------------------------------------------------------------------ *
 * Small helpers                                                      *
 * ------------------------------------------------------------------ */

static void  nested_flush (NestedCompositor *self);
static void  nested_update_primary (NestedCompositor *self);
static void  nested_start_frame_clock (NestedCompositor *self);
static void  xdg_surface_configure (NestedSurface *self);
static void  surface_resource_destroyed (struct wl_resource *resource);
static void  pool_resource_destroyed (struct wl_resource *resource);
static void  seat_retarget (NestedCompositor *self);
static void  seat_clear_focus (NestedCompositor *self, NestedSurface *surface);
static void  surface_reset_viewport (NestedSurface *self);
static gboolean surface_apply_viewport (NestedSurface *self,
                                        struct wl_resource *resource,
                                        int natural_width, int natural_height);
static void  surface_update_local_size (NestedSurface *self);
static void  compositor_send_scale (NestedCompositor *self);
static void  text_input_retarget (NestedCompositor *self, NestedSurface *surface);
static void  text_input_forget_surface (NestedCompositor *self,
                                        NestedSurface *surface);

static uint32_t
nested_next_serial (NestedCompositor *self)
{
  return ++self->serial;
}

static uint32_t
nested_now_msec (void)
{
  return (uint32_t) (g_get_monotonic_time () / 1000);
}

static void
post_wrong_object (struct wl_resource *where, struct wl_resource *what, const char *iface)
{
  wl_resource_post_error (where, WL_DISPLAY_ERROR_INVALID_OBJECT,
                          "expected a %s, got id %u",
                          iface, what ? wl_resource_get_id (what) : 0u);
}

static NestedSurface *
surface_from_resource (struct wl_resource *where, struct wl_resource *resource)
{
  if (!wl_resource_instance_of (resource, &wl_surface_interface, &surface_impl))
    {
      post_wrong_object (where, resource, "wl_surface");
      return NULL;
    }
  return wl_resource_get_user_data (resource);
}

static Buffer *
buffer_from_resource (struct wl_resource *where, struct wl_resource *resource)
{
  if (!wl_resource_instance_of (resource, &wl_buffer_interface, &buffer_impl))
    {
      post_wrong_object (where, resource, "wl_buffer");
      return NULL;
    }
  return wl_resource_get_user_data (resource);
}

static Pool *
pool_ref (Pool *pool)
{
  pool->refcount++;
  return pool;
}

static void
pool_unref (Pool *pool)
{
  if (--pool->refcount > 0)
    return;
  close (pool->fd);
  g_free (pool);
}

/* ------------------------------------------------------------------ *
 * NestedSurface                                                      *
 * ------------------------------------------------------------------ */

G_DEFINE_TYPE (NestedSurface, nested_surface, G_TYPE_OBJECT)

static void
nested_surface_init (NestedSurface *self)
{
  self->scale = 1;
  wl_list_init (&self->nested_link);
  wl_list_init (&self->frame_callbacks);
  wl_list_init (&self->pending_user.link);
  wl_list_init (&self->fractional_links);
  self->pending_user.surface = self;

  /* Negative means unset: no crop, no destination size, so the surface keeps
   * the size of the buffer it attaches. */
  self->pending_src.width = -1.0;
  self->pending_src.height = -1.0;
  self->src.width = -1.0;
  self->src.height = -1.0;
  self->pending_dst.width = -1;
  self->pending_dst.height = -1;
  self->dst.width = -1;
  self->dst.height = -1;
}

static void
nested_surface_dispose (GObject *object)
{
  NestedSurface *self = NESTED_SURFACE (object);

  if (self->pending_user.buffer)
    {
      wl_list_remove (&self->pending_user.link);
      wl_list_init (&self->pending_user.link);
      self->pending_user.buffer = NULL;
    }

  if (self->in_list)
    {
      wl_list_remove (&self->nested_link);
      wl_list_init (&self->nested_link);
      self->in_list = FALSE;
    }

  G_OBJECT_CLASS (nested_surface_parent_class)->dispose (object);
}

static void
nested_surface_finalize (GObject *object)
{
  NestedSurface *self = NESTED_SURFACE (object);

  g_clear_object (&self->texture);
  g_clear_pointer (&self->app_id, g_free);
  g_clear_pointer (&self->title, g_free);

  G_OBJECT_CLASS (nested_surface_parent_class)->finalize (object);
}

static void
nested_surface_class_init (NestedSurfaceClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = nested_surface_dispose;
  G_OBJECT_CLASS (klass)->finalize = nested_surface_finalize;
}

GdkTexture *
nested_surface_get_texture (NestedSurface *self)
{
  g_return_val_if_fail (NESTED_IS_SURFACE (self), NULL);
  return self->texture;
}

void
nested_surface_get_size (NestedSurface *self, int *width, int *height)
{
  g_return_if_fail (NESTED_IS_SURFACE (self));
  if (width)
    *width = self->width;
  if (height)
    *height = self->height;
}

void
nested_surface_get_paint_rect (NestedSurface *self, graphene_rect_t *src,
                               graphene_rect_t *dst)
{
  g_return_if_fail (NESTED_IS_SURFACE (self));

  if (src)
    {
      if (self->src.width > 0.0 && self->texture)
        {
          /* wp_viewport names its source rectangle in natural coordinates,
           * below the buffer scale, so the texture has to be scaled back up. */
          graphene_rect_init (src, self->src.x * self->scale,
                              self->src.y * self->scale,
                              self->src.width * self->scale,
                              self->src.height * self->scale);
        }
      else if (self->texture)
        graphene_rect_init (src, 0, 0, gdk_texture_get_width (self->texture),
                            gdk_texture_get_height (self->texture));
      else
        graphene_rect_init (src, 0, 0, 0, 0);
    }

  if (dst)
    graphene_rect_init (dst, 0, 0, self->width, self->height);
}

const char *
nested_surface_get_app_id (NestedSurface *self)
{
  g_return_val_if_fail (NESTED_IS_SURFACE (self), NULL);
  return self->app_id;
}

const char *
nested_surface_get_title (NestedSurface *self)
{
  g_return_val_if_fail (NESTED_IS_SURFACE (self), NULL);
  return self->title;
}

gboolean
nested_surface_is_mapped (NestedSurface *self)
{
  g_return_val_if_fail (NESTED_IS_SURFACE (self), FALSE);
  return self->mapped;
}

static NestedSurface *
nested_surface_new (NestedCompositor *nested, struct wl_resource *resource)
{
  NestedSurface *self;

  self = g_object_new (NESTED_TYPE_SURFACE, NULL);
  self->nested = nested;
  self->wl_surface = resource;
  wl_list_insert (&nested->surfaces, &self->nested_link);
  self->in_list = TRUE;

  /* The surface resource owns the object; xdg resources take further refs. */
  wl_resource_set_implementation (resource, &surface_impl, g_object_ref (self),
                                  surface_resource_destroyed);
  return self;
}

static void
surface_list_remove (NestedSurface *self)
{
  if (!self->in_list)
    return;
  wl_list_remove (&self->nested_link);
  wl_list_init (&self->nested_link);
  self->in_list = FALSE;
}

static void
surface_set_pending (NestedSurface *self, Buffer *buffer)
{
  if (self->pending_user.buffer)
    wl_list_remove (&self->pending_user.link);

  self->pending_user.buffer = buffer;

  if (buffer)
    wl_list_insert (&buffer->users, &self->pending_user.link);
  else
    wl_list_init (&self->pending_user.link);
}

static void
surface_clear_content (NestedSurface *self)
{
  gboolean was_primary;

  if (!self->texture && !self->mapped)
    return;

  was_primary = self->nested && self == self->nested->primary;

  g_clear_object (&self->texture);
  self->width = 0;
  self->height = 0;
  self->natural_width = 0;
  self->natural_height = 0;
  self->mapped = FALSE;

  if (was_primary)
    nested_update_primary (self->nested);
}

/* ------------------------------------------------------------------ *
 * Frame callbacks                                                    *
 * ------------------------------------------------------------------ */

static void
frame_callback_destroyed (struct wl_resource *resource)
{
  FrameCallback *cb = wl_resource_get_user_data (resource);

  wl_list_remove (&cb->link);
  wl_list_init (&cb->link);
  g_object_unref (cb->surface);
  g_free (cb);
}

static void
surface_frame (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  struct wl_resource *cb_resource;
  FrameCallback *cb;

  cb_resource = wl_resource_create (client, &wl_callback_interface,
                                    wl_resource_get_version (resource), id);
  cb = g_new0 (FrameCallback, 1);
  cb->resource = cb_resource;
  cb->surface = g_object_ref (self);
  wl_list_insert (&self->frame_callbacks, &cb->link);

  /* wl_callback carries no requests, so a NULL implementation is enough. */
  wl_resource_set_implementation (cb_resource, NULL, cb, frame_callback_destroyed);

  /* A client commits first and asks for the callback afterwards, so a frame
   * request is the only hint that there is something to tick for. Without this
   * a surface that never resizes sits on its single commit forever. */
  if (self->nested)
    nested_start_frame_clock (self->nested);
}

static void
surface_fire_frame_callbacks (NestedSurface *self, uint32_t msec)
{
  FrameCallback *cb, *tmp;

  wl_list_for_each_safe (cb, tmp, &self->frame_callbacks, link)
    {
      wl_callback_send_done (cb->resource, msec);
      wl_resource_destroy (cb->resource);
    }
}

static gboolean
surface_has_pending_frames (NestedSurface *self)
{
  return !wl_list_empty (&self->frame_callbacks);
}

/* ------------------------------------------------------------------ *
 * Buffer readback                                                    *
 * ------------------------------------------------------------------ */

static gboolean
read_pool (Pool *pool, off_t offset, size_t length, guint8 *dst)
{
  size_t done = 0;

  while (done < length)
    {
      ssize_t n = pread (pool->fd, dst + done, length - done, offset + done);

      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          g_warning ("nested: reading shm pool failed: %s", g_strerror (errno));
          return FALSE;
        }
      if (n == 0)
        break; /* the file is shorter than the pool: the tail stays zeroed */
      done += (size_t) n;
    }

  return TRUE;
}

static GdkTexture *
buffer_create_shm_texture (Buffer *buffer)
{
  size_t length = (size_t) buffer->stride * buffer->height;
  GdkMemoryFormat format;
  guint8 *pixels;
  GBytes *bytes;

  if (buffer->format != WL_SHM_FORMAT_ARGB8888 &&
      buffer->format != WL_SHM_FORMAT_XRGB8888)
    return NULL;

  format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
  pixels = g_malloc0 (length);

  if (!read_pool (buffer->pool, buffer->offset, length, pixels))
    {
      g_free (pixels);
      return NULL;
    }

  if (buffer->format == WL_SHM_FORMAT_XRGB8888)
    {
      uint32_t *word = (uint32_t *) pixels;
      size_t count = length / sizeof (uint32_t);
      size_t i;

      for (i = 0; i < count; i++)
        word[i] |= 0xff000000u;
    }

  bytes = g_bytes_new_take (pixels, length);
  return gdk_memory_texture_new (buffer->width, buffer->height, format, bytes,
                                 buffer->stride);
}

static void
dmabuf_fds_close (gpointer data)
{
  int *fds = data;
  int i;

  for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
    if (fds[i] >= 0)
      close (fds[i]);

  g_free (fds);
}

static GdkTexture *
buffer_create_dmabuf_texture (Buffer *buffer)
{
  GdkDmabufTextureBuilder *builder;
  GdkDisplay *display;
  GdkTexture *texture;
  GError *error = NULL;
  char name[5] = { 0 };
  int *fds;
  int i;

  for (i = 0; i < 4; i++)
    name[i] = (buffer->fourcc >> (8 * i)) & 0xff;
  name[4] = 0;
  if (!g_ascii_isprint (name[0]) || !g_ascii_isprint (name[1]) ||
      !g_ascii_isprint (name[2]) || !g_ascii_isprint (name[3]))
    memcpy (name, "????", 5);

  /* GDK imports through the display's EGL, so there has to be one. */
  display = gdk_display_get_default ();
  if (!display)
    {
      g_warning ("nested: a dmabuf buffer arrived but GDK is not initialized");
      return NULL;
    }

  builder = gdk_dmabuf_texture_builder_new ();
  gdk_dmabuf_texture_builder_set_display (builder, display);
  gdk_dmabuf_texture_builder_set_width (builder, buffer->width);
  gdk_dmabuf_texture_builder_set_height (builder, buffer->height);
  gdk_dmabuf_texture_builder_set_fourcc (builder, buffer->fourcc);
  gdk_dmabuf_texture_builder_set_modifier (builder, buffer->modifier);
  gdk_dmabuf_texture_builder_set_n_planes (builder, buffer->n_planes);
  gdk_dmabuf_texture_builder_set_premultiplied (builder,
      buffer->fourcc == DRM_FORMAT_ARGB8888 ||
      buffer->fourcc == DRM_FORMAT_ABGR8888);

  for (i = 0; i < buffer->n_planes; i++)
    {
      gdk_dmabuf_texture_builder_set_fd (builder, i, buffer->fd[i]);
      gdk_dmabuf_texture_builder_set_stride (builder, i, buffer->plane_stride[i]);
      gdk_dmabuf_texture_builder_set_offset (builder, i, buffer->plane_offset[i]);
    }

  fds = g_new (int, NESTED_DMABUF_MAX_PLANES);
  memcpy (fds, buffer->fd, sizeof buffer->fd);

  texture = gdk_dmabuf_texture_builder_build (builder, dmabuf_fds_close, fds, &error);
  g_object_unref (builder);

  if (!texture)
    {
      g_warning ("nested: cannot import a %dx%d dmabuf (%s, modifier 0x%" PRIx64
                 ", %d plane%s): %s", buffer->width, buffer->height, name,
                 buffer->modifier, buffer->n_planes,
                 buffer->n_planes == 1 ? "" : "s",
                 error ? error->message : "unknown reason");
      g_clear_error (&error);

      /* build() failed, so the notify never ran and the fds are still ours. */
      g_free (fds);
      return NULL;
    }

  buffer->fds_transferred = TRUE;

  g_debug ("nested: imported dmabuf %dx%d fourcc=%s modifier=0x%" PRIx64
           " stride=%u offset=%u planes=%d", buffer->width, buffer->height, name,
           buffer->modifier, buffer->plane_stride[0], buffer->plane_offset[0],
           buffer->n_planes);

  return texture;
}

/* The texture for a client buffer: dmabuf buffers are imported once and cached
 * (their contents are immutable), shm buffers are read out on every commit. */
static GdkTexture *
buffer_get_texture (Buffer *buffer)
{
  if (!buffer->is_dmabuf)
    return buffer_create_shm_texture (buffer);

  if (!buffer->texture)
    buffer->texture = buffer_create_dmabuf_texture (buffer);

  return buffer->texture ? g_object_ref (buffer->texture) : NULL;
}

/* ------------------------------------------------------------------ *
 * Crop and scale                                                     *
 *                                                                    *
 * wp_viewport decouples the surface size from the buffer size, which  *
 * is how a client renders one screenful of page at a device scale of  *
 * 1.25 and still tells the compositor its surface is 1463x785.        *
 * ------------------------------------------------------------------ */

static void
surface_reset_viewport (NestedSurface *self)
{
  self->pending_src.x = 0.0;
  self->pending_src.y = 0.0;
  self->pending_src.width = -1.0;
  self->pending_src.height = -1.0;
  self->pending_dst.width = -1;
  self->pending_dst.height = -1;
}

/* The surface-local size is the destination if there is one, otherwise the
 * source rectangle, otherwise the buffer divided by its scale. */
static void
surface_update_local_size (NestedSurface *self)
{
  if (!self->texture)
    {
      self->width = 0;
      self->height = 0;
      return;
    }

  if (self->dst.width > 0 && self->dst.height > 0)
    {
      self->width = self->dst.width;
      self->height = self->dst.height;
    }
  else if (self->src.width > 0.0)
    {
      self->width = (int) self->src.width;
      self->height = (int) self->src.height;
    }
  else
    {
      self->width = self->natural_width;
      self->height = self->natural_height;
    }
}

static gboolean
is_whole (double value)
{
  return value == (double) (long long) value;
}

/* Make the pending crop current and say whether the surface state is usable.
 * Protocol errors kill the client, so the caller only has to stop applying
 * anything after one. */
static gboolean
surface_apply_viewport (NestedSurface *self, struct wl_resource *resource,
                        int natural_width, int natural_height)
{
  gboolean has_src = self->pending_src.width > 0.0;
  gboolean has_dst = self->pending_dst.width > 0;

  self->src = self->pending_src;
  self->dst = self->pending_dst;

  /* Cropping without scaling has to land on whole pixels, or the surface size
   * would not describe the content it holds. */
  if (has_src && !has_dst &&
      (!is_whole (self->src.width) || !is_whole (self->src.height)))
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_SIZE,
                              "source rectangle %gx%g needs a destination size",
                              self->src.width, self->src.height);
      return FALSE;
    }

  if (has_src && self->pending_attach && self->pending_user.buffer &&
      (self->src.x + self->src.width > natural_width ||
       self->src.y + self->src.height > natural_height))
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                              "source rectangle %g,%g %gx%g does not fit %dx%d",
                              self->src.x, self->src.y,
                              self->src.width, self->src.height,
                              natural_width, natural_height);
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------ *
 * wl_surface                                                         *
 * ------------------------------------------------------------------ */

static void
surface_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
surface_attach (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  Buffer *buffer = NULL;

  if (buffer_resource)
    {
      buffer = buffer_from_resource (resource, buffer_resource);
      if (!buffer)
        return;
    }

  surface_set_pending (self, buffer);
  self->pending_attach = TRUE;
  (void) x;
  (void) y;
}

static void
surface_damage (struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
  /* Full-buffer repaint: damage is not tracked. */
}

static void
surface_set_region (struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *region)
{
}

static void
surface_set_buffer_transform (struct wl_client *client, struct wl_resource *resource,
                              int32_t transform)
{
  if (transform != WL_OUTPUT_TRANSFORM_NORMAL)
    wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                            "buffer transform %d is not supported", transform);
}

static void
surface_set_buffer_scale (struct wl_client *client, struct wl_resource *resource,
                          int32_t scale)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  if (scale <= 0)
    {
      wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_SCALE,
                              "buffer scale must be positive, not %d", scale);
      return;
    }

  self->scale = scale;
}

static void
surface_offset (struct wl_client *client, struct wl_resource *resource,
               int32_t x, int32_t y)
{
}

static void
surface_commit (struct wl_client *client, struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  NestedCompositor *nested = self->nested;
  Buffer *buffer = self->pending_user.buffer;
  gboolean repaint = FALSE;
  int natural_width, natural_height;
  int previous_width, previous_height;

  /* Buffer scale is applied before crop and scale, so the source rectangle the
   * client named refers to the buffer that is about to become current. */
  if (!self->pending_attach)
    {
      natural_width = self->natural_width;
      natural_height = self->natural_height;
    }
  else if (buffer)
    {
      natural_width = buffer->width / self->scale;
      natural_height = buffer->height / self->scale;
    }
  else
    {
      natural_width = 0;
      natural_height = 0;
    }

  if (!surface_apply_viewport (self, resource, natural_width, natural_height))
    return;

  previous_width = self->width;
  previous_height = self->height;

  /* Only wl_surface.attach changes the content: a state-only commit must
   * keep showing the buffer that is already current. */
  if (self->pending_attach)
    {
      self->pending_attach = FALSE;
      surface_set_pending (self, NULL);

      if (buffer)
        {
          GdkTexture *texture;

          if (self->scale > 1 &&
              (buffer->width % self->scale || buffer->height % self->scale))
            {
              wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_SIZE,
                                      "buffer %dx%d is not a multiple of scale %d",
                                      buffer->width, buffer->height, self->scale);
              return;
            }

          texture = buffer_get_texture (buffer);
          if (!texture)
            {
              g_warning ("nested: could not use a %dx%d buffer, dropping the frame",
                         buffer->width, buffer->height);
              wl_buffer_send_release (buffer->resource);
              return;
            }

          g_set_object (&self->texture, texture);
          g_object_unref (texture);
          self->natural_width = natural_width;
          self->natural_height = natural_height;

          wl_buffer_send_release (buffer->resource);
          repaint = TRUE;
        }
      else
        surface_clear_content (self);
    }

  surface_update_local_size (self);

  /* Cropping or rescaling changes what has to be drawn even when the buffer
   * stays the same. */
  if (!repaint && self->texture &&
      (self->width != previous_width || self->height != previous_height))
    repaint = TRUE;

  if (!self->mapped && self->xdg_surface && self->texture)
    {
      self->mapped = TRUE;
      if (self->is_popup)
        xdg_surface_configure (self);
      if (nested)
        nested_update_primary (nested);
    }

  if (!nested)
    return;

  if (self == nested->primary)
    {
      if (repaint)
        {
          nested->frame_count++;
          g_signal_emit (nested, signals[SIGNAL_FRAME], 0);
        }
    }
  else
    nested_update_primary (nested);

  nested_start_frame_clock (nested);
}

static const struct wl_surface_interface surface_impl = {
  .destroy = surface_destroy,
  .attach = surface_attach,
  .damage = surface_damage,
  .frame = surface_frame,
  .set_opaque_region = surface_set_region,
  .set_input_region = surface_set_region,
  .commit = surface_commit,
  .set_buffer_transform = surface_set_buffer_transform,
  .set_buffer_scale = surface_set_buffer_scale,
  .damage_buffer = surface_damage,
  .offset = surface_offset,
};

static void
surface_resource_destroyed (struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  NestedCompositor *nested = self->nested;

  self->wl_surface = NULL;
  surface_set_pending (self, NULL);
  self->mapped = FALSE;

  seat_clear_focus (nested, self);
  text_input_forget_surface (nested, self);
  surface_list_remove (self);

  if (nested && self == nested->primary)
    nested_update_primary (nested);

  g_object_unref (self);
}

/* ------------------------------------------------------------------ *
 * wl_compositor, wl_region                                           *
 * ------------------------------------------------------------------ */

static void
compositor_create_surface (struct wl_client *client, struct wl_resource *resource,
                           uint32_t id)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *surface_resource;

  surface_resource = wl_resource_create (client, &wl_surface_interface,
                                         wl_resource_get_version (resource), id);
  nested_surface_new (self, surface_resource);
}

static void
region_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
region_add (struct wl_client *client, struct wl_resource *resource,
            int32_t x, int32_t y, int32_t width, int32_t height)
{
}

static const struct wl_region_interface region_impl = {
  .destroy = region_destroy,
  .add = region_add,
  .subtract = region_add,
};

static void
compositor_create_region (struct wl_client *client, struct wl_resource *resource,
                          uint32_t id)
{
  struct wl_resource *region_resource;

  region_resource = wl_resource_create (client, &wl_region_interface,
                                        wl_resource_get_version (resource), id);
  wl_resource_set_implementation (region_resource, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
  .create_surface = compositor_create_surface,
  .create_region = compositor_create_region,
};

static void
compositor_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation (resource, &compositor_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * wl_shm                                                             *
 * ------------------------------------------------------------------ */

static void
shm_create_pool (struct wl_client *client, struct wl_resource *resource,
                 uint32_t id, int32_t fd, int32_t size)
{
  struct wl_resource *pool_resource;
  struct stat info;
  Pool *pool;

  if (size <= 0)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "invalid pool size %d", size);
      close (fd);
      return;
    }

  if (fstat (fd, &info) != 0)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "cannot stat the pool fd: %s", g_strerror (errno));
      close (fd);
      return;
    }

  if (info.st_size < size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "fd holds %lld bytes, pool wants %d",
                              (long long) info.st_size, size);
      close (fd);
      return;
    }

  pool = g_new0 (Pool, 1);
  pool->fd = fd;
  pool->size = (size_t) size;
  pool->refcount = 1;

  pool_resource = wl_resource_create (client, &wl_shm_pool_interface,
                                      wl_resource_get_version (resource), id);
  wl_resource_set_implementation (pool_resource, &shm_pool_impl, pool,
                                  pool_resource_destroyed);
}

static void
shm_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_shm_interface shm_impl = {
  .create_pool = shm_create_pool,
  .release = shm_release,
};

static void
shm_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_shm_interface, version, id);
  wl_resource_set_implementation (resource, &shm_impl, data, NULL);

  wl_shm_send_format (resource, WL_SHM_FORMAT_ARGB8888);
  wl_shm_send_format (resource, WL_SHM_FORMAT_XRGB8888);
}

/* ------------------------------------------------------------------ *
 * wl_shm_pool, wl_buffer                                             *
 * ------------------------------------------------------------------ */

static void
buffer_resource_destroyed (struct wl_resource *resource)
{
  Buffer *buffer = wl_resource_get_user_data (resource);
  BufferUser *user, *tmp;
  int i;

  wl_list_for_each_safe (user, tmp, &buffer->users, link)
    {
      wl_list_remove (&user->link);
      wl_list_init (&user->link);
      user->buffer = NULL;
    }

  if (buffer->is_dmabuf)
    {
      /* Dropping our reference is enough: if a texture still holds the planes,
       * its destroy notify closes the fds when it goes away. */
      g_clear_object (&buffer->texture);
      if (!buffer->fds_transferred)
        {
          for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
            if (buffer->fd[i] >= 0)
              close (buffer->fd[i]);
        }
      g_free (buffer);
      return;
    }

  pool_unref (buffer->pool);
  g_free (buffer);
}

static void
shm_pool_create_buffer (struct wl_client *client, struct wl_resource *resource,
                        uint32_t id, int32_t offset, int32_t width, int32_t height,
                        int32_t stride, uint32_t format)
{
  struct wl_resource *buffer_resource;
  Pool *pool = wl_resource_get_user_data (resource);
  Buffer *buffer;
  int64_t needed;

  if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FORMAT,
                              "format 0x%x is not supported", format);
      return;
    }

  if (width <= 0 || height <= 0 || stride < width * 4 || offset < 0)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
                              "invalid %dx%d buffer with stride %d offset %d",
                              width, height, stride, offset);
      return;
    }

  needed = (int64_t) offset + (int64_t) stride * height;
  if (needed > (int64_t) pool->size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
                              "buffer needs %" PRId64 " bytes, pool has %zu",
                              needed - offset, pool->size);
      return;
    }

  buffer = g_new0 (Buffer, 1);
  buffer->pool = pool_ref (pool);
  buffer->offset = offset;
  buffer->width = width;
  buffer->height = height;
  buffer->stride = stride;
  buffer->format = format;
  wl_list_init (&buffer->users);

  buffer_resource = wl_resource_create (client, &wl_buffer_interface, 1, id);
  buffer->resource = buffer_resource;
  wl_resource_set_implementation (buffer_resource, &buffer_impl, buffer,
                                  buffer_resource_destroyed);
}

static void
pool_resource_destroyed (struct wl_resource *resource)
{
  pool_unref (wl_resource_get_user_data (resource));
}

static void
shm_pool_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
shm_pool_resize (struct wl_client *client, struct wl_resource *resource, int32_t size)
{
  Pool *pool = wl_resource_get_user_data (resource);
  struct stat info;

  if ((int64_t) size <= (int64_t) pool->size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "pool can only grow, not shrink to %d", size);
      return;
    }

  if (fstat (pool->fd, &info) != 0 || info.st_size < size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "fd does not hold %d bytes", size);
      return;
    }

  pool->size = (size_t) size;
}

static const struct wl_shm_pool_interface shm_pool_impl = {
  .create_buffer = shm_pool_create_buffer,
  .destroy = shm_pool_destroy,
  .resize = shm_pool_resize,
};

static void
buffer_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_buffer_interface buffer_impl = {
  .destroy = buffer_destroy,
};

/* ------------------------------------------------------------------ *
 * zwp_linux_dmabuf_v1                                                *
 * ------------------------------------------------------------------ */

static const struct zwp_linux_buffer_params_v1_interface params_impl;
static void params_resource_destroyed (struct wl_resource *resource);

static struct gbm_device *
gbm_open_render_node (void)
{
  struct gbm_device *device = NULL;
  int index;

  /* gbm_create_device() owns the fd on success and closes it on failure, so
   * there is nothing to clean up between attempts. */
  for (index = 128; index < 136 && !device; index++)
    {
      char path[64];
      int fd;

      g_snprintf (path, sizeof path, "/dev/dri/renderD%d", index);
      fd = open (path, O_RDWR | O_CLOEXEC);
      if (fd < 0)
        continue;

      device = gbm_create_device (fd);
      if (device)
        g_debug ("nested: probed dmabuf formats on %s", path);
    }

  return device;
}

/* Feedback hands every client the same immutable table, so it is built once:
 * a packed array of 16-byte (format, padding, modifier) records in a memfd. */
static void
dmabuf_build_format_table (NestedCompositor *self)
{
  struct {
    uint32_t fourcc;
    uint32_t pad;
    uint64_t modifier;
  } *table;
  size_t size = (size_t) self->dmabuf_formats->len * sizeof (*table);
  void *mapped;
  struct stat st;
  guint i;
  int fd;

  fd = memfd_create ("nested-dmabuf-formats", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, size) != 0)
    {
      if (fd >= 0)
        close (fd);
      g_warning ("nested: cannot create the dmabuf format table: %s", g_strerror (errno));
      return;
    }

  mapped = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED)
    {
      close (fd);
      g_warning ("nested: cannot map the dmabuf format table: %s", g_strerror (errno));
      return;
    }

  table = mapped;
  for (i = 0; i < self->dmabuf_formats->len; i++)
    {
      DmabufFormat *entry = &g_array_index (self->dmabuf_formats, DmabufFormat, i);

      table[i].fourcc = entry->fourcc;
      table[i].pad = 0;
      table[i].modifier = entry->modifier;
    }
  munmap (mapped, size);

  self->dmabuf_table_fd = fd;
  self->dmabuf_table_size = size;

  if (fstat (gbm_device_get_fd (self->gbm), &st) == 0)
    self->dmabuf_device = (uint64_t) st.st_rdev;
}

static void
nested_setup_dmabuf (NestedCompositor *self)
{
  static const uint32_t fourccs[] = {
    DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888,
  };
  /* Implicit (INVALID) is what a plain gbm_bo_create() yields; LINEAR is the
   * one modifier every driver really supports. Tiled modifiers would only
   * matter for scan-out, which a nested compositor does not do. */
  static const uint64_t modifiers[] = { DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR };
  size_t f, m;

  self->gbm = gbm_open_render_node ();
  if (!self->gbm)
    {
      g_debug ("nested: no render node, so no zwp_linux_dmabuf_v1 global");
      return;
    }

  for (f = 0; f < G_N_ELEMENTS (fourccs); f++)
    {
      if (!gbm_device_is_format_supported (self->gbm, fourccs[f],
                                           GBM_BO_USE_RENDERING | GBM_BO_USE_WRITE))
        continue;

      for (m = 0; m < G_N_ELEMENTS (modifiers); m++)
        {
          DmabufFormat entry = { fourccs[f], modifiers[m] };

          g_array_append_val (self->dmabuf_formats, entry);
        }
    }

  if (self->dmabuf_formats->len == 0)
    g_debug ("nested: the render node supports no 8888 format for writing");
  else
    dmabuf_build_format_table (self);
}

static void
dmabuf_send_formats (struct wl_resource *resource, NestedCompositor *self)
{
  uint32_t version = wl_resource_get_version (resource);
  guint i;

  for (i = 0; i < self->dmabuf_formats->len; i++)
    {
      DmabufFormat *entry = &g_array_index (self->dmabuf_formats, DmabufFormat, i);

      zwp_linux_dmabuf_v1_send_format (resource, entry->fourcc);
    }

  if (version < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
    return;

  for (i = 0; i < self->dmabuf_formats->len; i++)
    {
      DmabufFormat *entry = &g_array_index (self->dmabuf_formats, DmabufFormat, i);

      zwp_linux_dmabuf_v1_send_modifier (resource, entry->fourcc,
                                         entry->modifier >> 32,
                                         entry->modifier & 0xffffffffu);
    }
}

static void
dmabuf_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
dmabuf_create_params (struct wl_client *client, struct wl_resource *resource,
                      uint32_t id)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *params_resource;
  Params *params;
  int i;

  params = g_new0 (Params, 1);
  params->nested = self;
  for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
    params->fd[i] = -1;

  params_resource = wl_resource_create (client, &zwp_linux_buffer_params_v1_interface,
                                        wl_resource_get_version (resource), id);
  params->resource = params_resource;
  wl_resource_set_implementation (params_resource, &params_impl, params,
                                  params_resource_destroyed);
}

/* ------------------------------------------------------------------ *
 * zwp_linux_dmabuf_feedback_v1                                       *
 * ------------------------------------------------------------------ */

static void
feedback_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
  .destroy = feedback_destroy,
};

static void
dmabuf_send_feedback (struct wl_resource *resource, NestedCompositor *self)
{
  struct wl_array device;
  struct wl_array indices;
  guint i;

  /* A nested compositor has one consumer, the render node it imports on, so
   * there is a single tranche with that device and no scanout flag. */
  if (self->dmabuf_table_fd < 0)
    {
      zwp_linux_dmabuf_feedback_v1_send_done (resource);
      return;
    }

  zwp_linux_dmabuf_feedback_v1_send_format_table (resource, self->dmabuf_table_fd,
                                                  self->dmabuf_table_size);

  device.data = &self->dmabuf_device;
  device.size = sizeof self->dmabuf_device;
  device.alloc = 0;

  indices.data = g_newa (uint16_t, self->dmabuf_formats->len);
  indices.size = self->dmabuf_formats->len * sizeof (uint16_t);
  indices.alloc = 0;
  for (i = 0; i < self->dmabuf_formats->len; i++)
    ((uint16_t *) indices.data)[i] = (uint16_t) i;

  zwp_linux_dmabuf_feedback_v1_send_main_device (resource, &device);
  zwp_linux_dmabuf_feedback_v1_send_tranche_target_device (resource, &device);
  zwp_linux_dmabuf_feedback_v1_send_tranche_flags (resource, 0);
  zwp_linux_dmabuf_feedback_v1_send_tranche_formats (resource, &indices);
  zwp_linux_dmabuf_feedback_v1_send_tranche_done (resource);
  zwp_linux_dmabuf_feedback_v1_send_done (resource);
}

static void
dmabuf_create_feedback (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *feedback;

  feedback = wl_resource_create (client, &zwp_linux_dmabuf_feedback_v1_interface,
                                 wl_resource_get_version (resource), id);
  if (!feedback)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (feedback, &feedback_impl, self, NULL);
  dmabuf_send_feedback (feedback, self);
}

static void
dmabuf_get_default_feedback (struct wl_client *client, struct wl_resource *resource,
                             uint32_t id)
{
  dmabuf_create_feedback (client, resource, id);
}

static void
dmabuf_get_surface_feedback (struct wl_client *client, struct wl_resource *resource,
                             uint32_t id, G_GNUC_UNUSED struct wl_resource *surface)
{
  /* Only one widget is ever scanned out, so per-surface feedback cannot
   * differ from the default. */
  dmabuf_create_feedback (client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
  .destroy = dmabuf_destroy,
  .create_params = dmabuf_create_params,
  .get_default_feedback = dmabuf_get_default_feedback,
  .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void
dmabuf_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  NestedCompositor *self = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_linux_dmabuf_v1_interface, version, id);
  wl_resource_set_implementation (resource, &dmabuf_impl, self, NULL);

  dmabuf_send_formats (resource, self);
}

/* ------------------------------------------------------------------ *
 * zwp_linux_buffer_params_v1                                         *
 * ------------------------------------------------------------------ */

static void
params_resource_destroyed (struct wl_resource *resource)
{
  Params *params = wl_resource_get_user_data (resource);
  int i;

  /* Plane fds handed to a wl_buffer are not ours any more. */
  for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
    if (params->has_plane[i])
      close (params->fd[i]);

  g_free (params);
}

static void
params_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
params_add (struct wl_client *client, struct wl_resource *resource,
            int32_t fd, uint32_t plane_idx, uint32_t offset, uint32_t stride,
            uint32_t modifier_hi, uint32_t modifier_lo)
{
  Params *params = wl_resource_get_user_data (resource);

  if (plane_idx >= NESTED_DMABUF_MAX_PLANES)
    {
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                              "plane index %u is out of range", plane_idx);
      close (fd);
      return;
    }

  if (params->has_plane[plane_idx])
    {
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                              "plane %u was added twice", plane_idx);
      close (fd);
      return;
    }

  params->fd[plane_idx] = fd;
  params->offset[plane_idx] = offset;
  params->stride[plane_idx] = stride;
  params->modifier[plane_idx] = ((uint64_t) modifier_hi << 32) | modifier_lo;
  params->has_plane[plane_idx] = TRUE;
}

/* Imports the collected planes as a wl_buffer. On failure an error or the
 * 'failed' event has been sent and NULL is returned. */
static void
params_create_buffer (struct wl_client *client, struct wl_resource *resource,
                      uint32_t buffer_id, int32_t width, int32_t height,
                      uint32_t fourcc, uint32_t flags, gboolean immediate)
{
  Params *params = wl_resource_get_user_data (resource);
  struct wl_resource *buffer_resource;
  Buffer *buffer;
  int i;

  if (params->used)
    {
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "this params object already created a buffer");
      return;
    }

  if (!params->has_plane[0])
    {
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                              "plane 0 was never added");
      return;
    }

  for (i = 1; i < NESTED_DMABUF_MAX_PLANES; i++)
    if (params->has_plane[i] && !params->has_plane[i - 1])
      {
        wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                                "plane %u added without plane %d", i, i - 1);
        return;
      }

  if (width <= 0 || height <= 0)
    {
      wl_resource_post_error (resource,
                              ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                              "invalid buffer size %dx%d", width, height);
      return;
    }

  if (flags != 0)
    {
      /* Flipping or de-interlacing on upload is not implemented; showing the
       * frame the wrong way round would be worse than refusing it. */
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "buffer flags 0x%x are not supported", flags);
      return;
    }

  for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
    if (params->has_plane[i] && params->modifier[i] != params->modifier[0])
      {
        wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                                "planes do not share a modifier");
        return;
      }

  buffer = g_new0 (Buffer, 1);
  buffer->is_dmabuf = TRUE;
  buffer->width = width;
  buffer->height = height;
  buffer->fourcc = fourcc;
  buffer->modifier = params->modifier[0];
  for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
    buffer->fd[i] = -1;

  for (i = 0; i < NESTED_DMABUF_MAX_PLANES && params->has_plane[i]; i++)
    {
      buffer->fd[i] = params->fd[i];
      buffer->plane_offset[i] = params->offset[i];
      buffer->plane_stride[i] = params->stride[i];
      buffer->n_planes = i + 1;
      params->has_plane[i] = FALSE; /* the buffer owns the fd from here on */
    }

  buffer->stride = (int32_t) buffer->plane_stride[0];
  wl_list_init (&buffer->users);

  /* An id of 0 makes libwayland pick one when the 'created' event binds it. */
  buffer_resource = wl_resource_create (client, &wl_buffer_interface, 1,
                                        immediate ? buffer_id : 0);
  buffer->resource = buffer_resource;
  wl_resource_set_implementation (buffer_resource, &buffer_impl, buffer,
                                  buffer_resource_destroyed);
  params->used = TRUE;

  /* Import now rather than at the first commit: the client can only fall back
   * to wl_shm if it learns about a failure while creating the buffer. */
  buffer->texture = buffer_create_dmabuf_texture (buffer);
  if (!buffer->texture)
    {
      zwp_linux_buffer_params_v1_send_failed (resource);
      return;
    }

  if (!immediate)
    zwp_linux_buffer_params_v1_send_created (resource, buffer_resource);
}

static void
params_create (struct wl_client *client, struct wl_resource *resource,
               int32_t width, int32_t height, uint32_t fourcc, uint32_t flags)
{
  params_create_buffer (client, resource, 0, width, height, fourcc, flags, FALSE);
}

static void
params_create_immed (struct wl_client *client, struct wl_resource *resource,
                     uint32_t buffer_id, int32_t width, int32_t height,
                     uint32_t fourcc, uint32_t flags)
{
  params_create_buffer (client, resource, buffer_id, width, height, fourcc, flags, TRUE);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
  .destroy = params_destroy,
  .add = params_add,
  .create = params_create,
  .create_immed = params_create_immed,
};

/* ------------------------------------------------------------------ *
 * wl_output                                                          *
 * ------------------------------------------------------------------ */

static void
output_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_output_interface output_impl = {
  .release = output_release,
};

static void
output_resource_destroyed (struct wl_resource *resource)
{
  OutputResource *output = wl_resource_get_user_data (resource);

  wl_list_remove (&output->link);
  wl_list_init (&output->link);
  g_free (output);
}

static void
output_send_state (OutputResource *output)
{
  struct wl_resource *resource = output->resource;
  NestedCompositor *self = output->nested;

  if (wl_resource_get_version (resource) < WL_OUTPUT_MODE_SINCE_VERSION)
    return;

  wl_output_send_mode (resource, WL_OUTPUT_MODE_CURRENT, self->width, self->height,
                       60000);
  if (wl_resource_get_version (resource) >= WL_OUTPUT_DONE_SINCE_VERSION)
    wl_output_send_done (resource);
}

static void
output_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;
  NestedCompositor *self = data;
  OutputResource *output;

  resource = wl_resource_create (client, &wl_output_interface, version, id);
  output = g_new0 (OutputResource, 1);
  output->nested = self;
  output->resource = resource;
  wl_list_insert (&self->outputs, &output->link);

  wl_resource_set_implementation (resource, &output_impl, output,
                                  output_resource_destroyed);

  wl_output_send_geometry (resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_NONE,
                           "nested", "widget", WL_OUTPUT_TRANSFORM_NORMAL);
  if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
    wl_output_send_scale (resource, 1);

  output_send_state (output);
}

static struct wl_resource *
find_output_for_client (NestedCompositor *self, struct wl_client *client)
{
  OutputResource *output;

  wl_list_for_each (output, &self->outputs, link)
    if (wl_resource_get_client (output->resource) == client)
      return output->resource;

  return NULL;
}

/* ------------------------------------------------------------------ *
 * wl_seat, wl_pointer, wl_keyboard                                   *
 * ------------------------------------------------------------------ */

/* xkb numbers keys from the evdev codes the Wayland protocol uses. */
#define XKB_EVDEV_OFFSET 8

static void
seat_client_destroyed (struct wl_resource *resource)
{
  SeatClient *sc = wl_resource_get_user_data (resource);

  /* Children of the seat outlive it otherwise, and would leave sc dangling. */
  if (sc->pointer)
    wl_resource_destroy (sc->pointer);
  if (sc->keyboard)
    wl_resource_destroy (sc->keyboard);

  wl_list_remove (&sc->link);
  wl_list_init (&sc->link);
  g_free (sc);
}

static void
pointer_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
pointer_set_cursor (struct wl_client *client, struct wl_resource *resource,
                    uint32_t serial, struct wl_resource *surface,
                    int32_t hotspot_x, int32_t hotspot_y)
{
  /* The embedding widget draws the real cursor; a nested client's cursor
   * surface is only ever a request to hide ours, which GTK does on its own. */
}

static const struct wl_pointer_interface pointer_impl = {
  .set_cursor = pointer_set_cursor,
  .release = pointer_release,
};

static void
pointer_resource_destroyed (struct wl_resource *resource)
{
  SeatClient *sc = wl_resource_get_user_data (resource);

  if (sc->pointer == resource)
    sc->pointer = NULL;
  sc->pointer_inside = FALSE;
}

static void
keyboard_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_keyboard_interface keyboard_impl = {
  .release = keyboard_release,
};

static void
keyboard_resource_destroyed (struct wl_resource *resource)
{
  SeatClient *sc = wl_resource_get_user_data (resource);

  if (sc->keyboard == resource)
    sc->keyboard = NULL;
  sc->keyboard_inside = FALSE;
}

static gboolean
keyboard_send_keymap (NestedCompositor *self, struct wl_resource *keyboard)
{
  size_t left = self->keymap_size;
  const char *p = self->keymap_text;
  int fd;

  if (!p)
    return FALSE;

  fd = memfd_create ("nested-keymap", MFD_CLOEXEC);
  if (fd < 0)
    {
      g_warning ("nested: cannot create a keymap file: %s", g_strerror (errno));
      return FALSE;
    }

  if (ftruncate (fd, self->keymap_size) < 0)
    {
      g_warning ("nested: cannot size the keymap file: %s", g_strerror (errno));
      close (fd);
      return FALSE;
    }

  while (left > 0)
    {
      ssize_t written = write (fd, p, left);

      if (written < 0)
        {
          if (errno == EINTR)
            continue;
          g_warning ("nested: cannot write the keymap: %s", g_strerror (errno));
          close (fd);
          return FALSE;
        }
      p += written;
      left -= (size_t) written;
    }

  /* libwayland dups the descriptor while queueing, so the client closes the
   * copy it receives and we close ours. */
  wl_keyboard_send_keymap (keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                           (uint32_t) self->keymap_size);
  close (fd);

  return TRUE;
}

static void
keyboard_send_modifiers (NestedCompositor *self, struct wl_resource *keyboard)
{
  uint32_t depressed = 0;
  uint32_t latched = 0;
  uint32_t locked = 0;
  uint32_t group = 0;

  if (self->xkb_state)
    {
      depressed = xkb_state_serialize_mods (self->xkb_state, XKB_STATE_MODS_DEPRESSED);
      latched = xkb_state_serialize_mods (self->xkb_state, XKB_STATE_MODS_LATCHED);
      locked = xkb_state_serialize_mods (self->xkb_state, XKB_STATE_MODS_LOCKED);
      group = xkb_state_serialize_layout (self->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    }

  wl_keyboard_send_modifiers (keyboard, nested_next_serial (self), depressed,
                              latched, locked, group);
}

static gboolean
seat_surface_is_mine (struct wl_resource *seat_resource, NestedSurface *surface)
{
  return surface->wl_surface != NULL &&
         wl_resource_get_client (surface->wl_surface) ==
           wl_resource_get_client (seat_resource);
}

static void
pointer_send_enter (NestedCompositor *self, struct wl_resource *pointer,
                    NestedSurface *surface)
{
  if (!seat_surface_is_mine (pointer, surface))
    return;

  wl_pointer_send_enter (pointer, nested_next_serial (self), surface->wl_surface,
                         wl_fixed_from_double (self->pointer_x),
                         wl_fixed_from_double (self->pointer_y));

  if (wl_resource_get_version (pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
    wl_pointer_send_frame (pointer);

  ((SeatClient *) wl_resource_get_user_data (pointer))->pointer_inside = TRUE;
}

static void
pointer_send_leave (NestedCompositor *self, struct wl_resource *pointer,
                    NestedSurface *surface)
{
  SeatClient *sc = wl_resource_get_user_data (pointer);

  if (!sc->pointer_inside || !seat_surface_is_mine (pointer, surface))
    return;

  wl_pointer_send_leave (pointer, nested_next_serial (self), surface->wl_surface);

  if (wl_resource_get_version (pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
    wl_pointer_send_frame (pointer);

  sc->pointer_inside = FALSE;
}

static void
keyboard_send_enter (NestedCompositor *self, struct wl_resource *keyboard,
                     NestedSurface *surface)
{
  struct wl_array keys;
  uint32_t i;

  if (!seat_surface_is_mine (keyboard, surface))
    return;

  wl_array_init (&keys);
  for (i = 0; i < self->pressed_keys->len; i++)
    {
      uint32_t *code = wl_array_add (&keys, sizeof (*code));

      if (code)
        *code = g_array_index (self->pressed_keys, uint32_t, i);
    }

  wl_keyboard_send_enter (keyboard, nested_next_serial (self),
                          surface->wl_surface, &keys);
  wl_array_release (&keys);

  keyboard_send_modifiers (self, keyboard);

  if (wl_resource_get_version (keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
    wl_keyboard_send_repeat_info (keyboard, 25, 250);

  ((SeatClient *) wl_resource_get_user_data (keyboard))->keyboard_inside = TRUE;
}

static void
keyboard_send_leave (NestedCompositor *self, struct wl_resource *keyboard,
                     NestedSurface *surface)
{
  SeatClient *sc = wl_resource_get_user_data (keyboard);

  if (!sc->keyboard_inside || !seat_surface_is_mine (keyboard, surface))
    return;

  wl_keyboard_send_leave (keyboard, nested_next_serial (self), surface->wl_surface);
  sc->keyboard_inside = FALSE;
}

static void
seat_retarget (NestedCompositor *self)
{
  NestedSurface *target = self->primary;
  SeatClient *sc;

  if (target && (!target->mapped || !target->wl_surface))
    target = NULL;

  if (self->pointer_wanted && target != self->pointer_focus)
    {
      if (self->pointer_focus)
        {
          NestedSurface *old = self->pointer_focus;

          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->pointer)
              pointer_send_leave (self, sc->pointer, old);
          g_clear_object (&self->pointer_focus);
        }
      if (target)
        {
          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->pointer)
              pointer_send_enter (self, sc->pointer, target);
          g_set_object (&self->pointer_focus, target);
        }
    }
  else if (!self->pointer_wanted && self->pointer_focus)
    {
      NestedSurface *old = self->pointer_focus;

      wl_list_for_each (sc, &self->seat_clients, link)
        if (sc->pointer)
          pointer_send_leave (self, sc->pointer, old);
      g_clear_object (&self->pointer_focus);
    }

  if (self->keyboard_wanted && target != self->keyboard_focus)
    {
      if (self->keyboard_focus)
        {
          NestedSurface *old = self->keyboard_focus;

          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->keyboard)
              keyboard_send_leave (self, sc->keyboard, old);
          g_clear_object (&self->keyboard_focus);
        }
      if (target)
        {
          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->keyboard)
              keyboard_send_enter (self, sc->keyboard, target);
          g_set_object (&self->keyboard_focus, target);
        }
    }
  else if (!self->keyboard_wanted && self->keyboard_focus)
    {
      NestedSurface *old = self->keyboard_focus;

      wl_list_for_each (sc, &self->seat_clients, link)
        if (sc->keyboard)
          keyboard_send_leave (self, sc->keyboard, old);
      g_clear_object (&self->keyboard_focus);
    }

  /* Text input follows the keyboard: the client that owns the focused surface
   * is the one whose caret the widget composes into. */
  text_input_retarget (self, self->keyboard_focus);

  nested_flush (self);
}

static void
seat_clear_focus (NestedCompositor *self, NestedSurface *surface)
{
  SeatClient *sc;

  if (!self)
    return;

  /* The surface is already gone from the protocol: drop the refs without
   * trying to send a leave for it. */
  if (self->pointer_focus == surface)
    {
      /* No leave can be sent for a destroyed surface, so forget that any
       * client is entered; a later enter starts the pair again. */
      wl_list_for_each (sc, &self->seat_clients, link)
        sc->pointer_inside = FALSE;
      g_clear_object (&self->pointer_focus);
    }
  if (self->keyboard_focus == surface)
    {
      wl_list_for_each (sc, &self->seat_clients, link)
        sc->keyboard_inside = FALSE;
      g_clear_object (&self->keyboard_focus);
    }
}

static void
seat_get_pointer (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  SeatClient *sc = wl_resource_get_user_data (resource);
  NestedCompositor *self = sc->nested;
  struct wl_resource *pointer;

  if (sc->pointer)
    wl_resource_destroy (sc->pointer);

  pointer = wl_resource_create (client, &wl_pointer_interface,
                                MIN (wl_resource_get_version (resource),
                                     NESTED_POINTER_VERSION), id);
  sc->pointer = pointer;
  wl_resource_set_implementation (pointer, &pointer_impl, sc,
                                  pointer_resource_destroyed);

  if (self->pointer_focus)
    pointer_send_enter (self, pointer, self->pointer_focus);
}

static void
seat_get_keyboard (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  SeatClient *sc = wl_resource_get_user_data (resource);
  NestedCompositor *self = sc->nested;
  struct wl_resource *keyboard;

  if (sc->keyboard)
    wl_resource_destroy (sc->keyboard);

  keyboard = wl_resource_create (client, &wl_keyboard_interface,
                                 MIN (wl_resource_get_version (resource),
                                      NESTED_KEYBOARD_VERSION), id);
  sc->keyboard = keyboard;
  wl_resource_set_implementation (keyboard, &keyboard_impl, sc,
                                  keyboard_resource_destroyed);

  if (!keyboard_send_keymap (self, keyboard))
    {
      wl_resource_destroy (keyboard);
      sc->keyboard = NULL;
      return;
    }

  if (self->keyboard_focus)
    keyboard_send_enter (self, keyboard, self->keyboard_focus);
}

static void
seat_get_touch (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  wl_resource_post_error (resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
                          "the nested seat has no touch capability");
}

static void
seat_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_seat_interface seat_impl = {
  .get_pointer = seat_get_pointer,
  .get_keyboard = seat_get_keyboard,
  .get_touch = seat_get_touch,
  .release = seat_destroy,
};

/* Compile the session keymap so clients can turn the evdev codes we send into
 * keyvals, and keep an xkb state around purely to report modifier bits. */
static void
nested_setup_keymap (NestedCompositor *self)
{
  self->xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!self->xkb_context)
    {
      g_warning ("nested: no xkb context, advertising a pointer-only seat");
      return;
    }

  self->xkb_keymap = xkb_keymap_new_from_names (self->xkb_context, NULL,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!self->xkb_keymap)
    {
      g_warning ("nested: no default xkb keymap, advertising a pointer-only seat");
      return;
    }

  self->keymap_text = xkb_keymap_get_as_string (self->xkb_keymap,
                                                XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!self->keymap_text)
    {
      g_warning ("nested: cannot serialize the keymap, advertising a pointer-only seat");
      return;
    }
  self->keymap_size = strlen (self->keymap_text);

  self->xkb_state = xkb_state_new (self->xkb_keymap);
}

static void
seat_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  NestedCompositor *self = data;
  struct wl_resource *resource;
  SeatClient *sc;

  resource = wl_resource_create (client, &wl_seat_interface, version, id);
  sc = g_new0 (SeatClient, 1);
  sc->nested = self;
  sc->seat = resource;
  wl_list_insert (&self->seat_clients, &sc->link);

  wl_resource_set_implementation (resource, &seat_impl, sc, seat_client_destroyed);

  if (self->keymap_text)
    wl_seat_send_capabilities (resource,
                               WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
  else
    wl_seat_send_capabilities (resource, WL_SEAT_CAPABILITY_POINTER);

  if (version >= WL_SEAT_NAME_SINCE_VERSION)
    wl_seat_send_name (resource, NESTED_SEAT_NAME);
}

void
nested_compositor_set_pointer_active (NestedCompositor *self, gboolean active)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (self->pointer_wanted == (active ? 1u : 0u))
    return;

  self->pointer_wanted = active ? 1u : 0u;
  seat_retarget (self);
}

void
nested_compositor_set_keyboard_active (NestedCompositor *self, gboolean active)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (self->keyboard_wanted == (active ? 1u : 0u))
    return;

  self->keyboard_wanted = active ? 1u : 0u;
  seat_retarget (self);
}

void
nested_compositor_notify_pointer_motion (NestedCompositor *self, double x, double y)
{
  SeatClient *sc;
  uint32_t time = nested_now_msec ();

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  self->pointer_x = x;
  self->pointer_y = y;

  if (!self->pointer_focus)
    return;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      if (!sc->pointer || !sc->pointer_inside)
        continue;
      wl_pointer_send_motion (sc->pointer, time, wl_fixed_from_double (x),
                              wl_fixed_from_double (y));
      if (wl_resource_get_version (sc->pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame (sc->pointer);
    }

  nested_flush (self);
}

void
nested_compositor_notify_pointer_button (NestedCompositor *self, uint32_t button,
                                         gboolean pressed)
{
  SeatClient *sc;
  uint32_t serial = nested_next_serial (self);
  uint32_t time = nested_now_msec ();

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (!self->pointer_focus)
    return;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      if (!sc->pointer || !sc->pointer_inside)
        continue;
      wl_pointer_send_button (sc->pointer, serial, time, button,
                              pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                      : WL_POINTER_BUTTON_STATE_RELEASED);
      if (wl_resource_get_version (sc->pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame (sc->pointer);
    }

  nested_flush (self);
}

void
nested_compositor_notify_pointer_axis (NestedCompositor *self, uint32_t axis,
                                       double delta, int32_t steps)
{
  SeatClient *sc;
  uint32_t time = nested_now_msec ();

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (axis == WL_POINTER_AXIS_VERTICAL_SCROLL ||
                    axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL);

  if (!self->pointer_focus)
    return;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      gboolean framed;

      if (!sc->pointer || !sc->pointer_inside)
        continue;

      framed = wl_resource_get_version (sc->pointer) >= WL_POINTER_FRAME_SINCE_VERSION;

      if (framed)
        wl_pointer_send_axis_source (sc->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
      wl_pointer_send_axis (sc->pointer, time, axis, wl_fixed_from_double (delta));
      if (framed)
        {
          if (steps)
            wl_pointer_send_axis_discrete (sc->pointer, axis, steps);
          wl_pointer_send_axis_stop (sc->pointer, time, axis);
          wl_pointer_send_frame (sc->pointer);
        }
    }

  nested_flush (self);
}

void
nested_compositor_notify_keyboard_key (NestedCompositor *self, uint32_t evdev_code,
                                       gboolean pressed)
{
  SeatClient *sc;
  uint32_t serial = nested_next_serial (self);
  uint32_t time = nested_now_msec ();
  uint32_t i;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (pressed)
    g_array_append_val (self->pressed_keys, evdev_code);
  else
    for (i = 0; i < self->pressed_keys->len; i++)
      if (g_array_index (self->pressed_keys, uint32_t, i) == evdev_code)
        {
          g_array_remove_index (self->pressed_keys, i);
          break;
        }

  if (self->xkb_state)
    xkb_state_update_key (self->xkb_state, evdev_code + XKB_EVDEV_OFFSET,
                          pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

  if (!self->keyboard_focus)
    return;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      if (!sc->keyboard || !sc->keyboard_inside)
        continue;
      wl_keyboard_send_key (sc->keyboard, serial, time, evdev_code,
                            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                    : WL_KEYBOARD_KEY_STATE_RELEASED);
      keyboard_send_modifiers (self, sc->keyboard);
    }

  nested_flush (self);
}

/* ------------------------------------------------------------------ *
 * wp_viewporter                                                      *
 * ------------------------------------------------------------------ */

static void
viewport_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
viewport_resource_destroyed (struct wl_resource *resource)
{
  Viewport *viewport = wl_resource_get_user_data (resource);
  NestedSurface *surface = viewport->surface;

  if (surface->viewport == resource)
    surface->viewport = NULL;

  /* Losing the object takes the crop and scale away; the next commit applies
   * that, so a client that only destroys the viewport keeps its content. */
  surface_reset_viewport (surface);

  g_object_unref (surface);
}

static void
viewport_set_source (struct wl_client *client, struct wl_resource *resource,
                     wl_fixed_t x, wl_fixed_t y, wl_fixed_t width,
                     wl_fixed_t height)
{
  Viewport *viewport = wl_resource_get_user_data (resource);
  NestedSurface *surface = viewport->surface;
  double src_x = wl_fixed_to_double (x);
  double src_y = wl_fixed_to_double (y);
  double src_w = wl_fixed_to_double (width);
  double src_h = wl_fixed_to_double (height);

  /* Four minus ones mean "no crop at all", not a rectangle of that size. */
  if (src_x == -1.0 && src_y == -1.0 && src_w == -1.0 && src_h == -1.0)
    {
      surface->pending_src.x = 0.0;
      surface->pending_src.y = 0.0;
      surface->pending_src.width = -1.0;
      surface->pending_src.height = -1.0;
      return;
    }

  if (src_x < 0.0 || src_y < 0.0 || src_w <= 0.0 || src_h <= 0.0)
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                              "source rectangle %g,%g %gx%g is not usable",
                              src_x, src_y, src_w, src_h);
      return;
    }

  surface->pending_src.x = src_x;
  surface->pending_src.y = src_y;
  surface->pending_src.width = src_w;
  surface->pending_src.height = src_h;
}

static void
viewport_set_destination (struct wl_client *client, struct wl_resource *resource,
                          int32_t width, int32_t height)
{
  Viewport *viewport = wl_resource_get_user_data (resource);
  NestedSurface *surface = viewport->surface;

  if (width == -1 && height == -1)
    {
      surface->pending_dst.width = -1;
      surface->pending_dst.height = -1;
      return;
    }

  if (width <= 0 || height <= 0)
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                              "destination size %dx%d is not usable", width, height);
      return;
    }

  surface->pending_dst.width = width;
  surface->pending_dst.height = height;
}

static const struct wp_viewport_interface viewport_impl = {
  .destroy = viewport_destroy,
  .set_source = viewport_set_source,
  .set_destination = viewport_set_destination,
};

static void
viewporter_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
viewporter_get_viewport (struct wl_client *client, struct wl_resource *resource,
                         uint32_t id, struct wl_resource *surface_resource)
{
  struct wl_resource *viewport_resource;
  NestedSurface *surface;
  Viewport *viewport;

  surface = surface_from_resource (resource, surface_resource);
  if (!surface)
    return;

  if (surface->viewport)
    {
      wl_resource_post_error (resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                              "surface already has a viewport");
      return;
    }

  viewport_resource = wl_resource_create (client, &wp_viewport_interface,
                                          wl_resource_get_version (resource), id);
  viewport = g_new0 (Viewport, 1);
  viewport->surface = g_object_ref (surface);
  viewport->resource = viewport_resource;
  surface->viewport = viewport_resource;

  wl_resource_set_implementation (viewport_resource, &viewport_impl, viewport,
                                  viewport_resource_destroyed);
}

static const struct wp_viewporter_interface viewporter_impl = {
  .destroy = viewporter_destroy,
  .get_viewport = viewporter_get_viewport,
};

static void
viewporter_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_viewporter_interface, version, id);
  wl_resource_set_implementation (resource, &viewporter_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * wp_fractional_scale_v1                                             *
 * ------------------------------------------------------------------ */

/* The protocol counts in 120ths of a scale factor, so 1.25 becomes 150. */
#define NESTED_SCALE_UNITS 120.0

static uint32_t
nested_scale_units (NestedCompositor *self)
{
  return (uint32_t) (self->scale * NESTED_SCALE_UNITS + 0.5);
}

static void
fractional_scale_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
  .destroy = fractional_scale_destroy,
};

static void
fractional_scale_resource_destroyed (struct wl_resource *resource)
{
  FractionalScale *scale = wl_resource_get_user_data (resource);
  NestedSurface *surface = scale->surface;

  if (surface->fractional == resource)
    surface->fractional = NULL;

  wl_list_remove (&scale->link);
  wl_list_init (&scale->link);
  g_object_unref (surface);
  g_free (scale);
}

/* Pushed whenever the widget's device scale changes, including the first time
 * a client asks: the event is the only way a client learns a non-integer
 * scale, and it is what makes wl_output.scale=1 a sane advertisement. */
static void
compositor_send_scale (NestedCompositor *self)
{
  NestedSurface *surface;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      FractionalScale *scale;

      wl_list_for_each (scale, &surface->fractional_links, link)
        wp_fractional_scale_v1_send_preferred_scale (scale->resource,
                                                    nested_scale_units (self));
    }

  nested_flush (self);
}

static void
fractional_scale_manager_destroy (struct wl_client *client,
                                  struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
fractional_scale_manager_get_scale (struct wl_client *client,
                                    struct wl_resource *resource, uint32_t id,
                                    struct wl_resource *surface_resource)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *scale_resource;
  NestedSurface *surface;
  FractionalScale *scale;

  surface = surface_from_resource (resource, surface_resource);
  if (!surface)
    return;

  if (surface->fractional)
    {
      wl_resource_post_error (resource,
                              WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                              "surface already has a fractional scale object");
      return;
    }

  scale_resource = wl_resource_create (client, &wp_fractional_scale_v1_interface,
                                       wl_resource_get_version (resource), id);
  scale = g_new0 (FractionalScale, 1);
  scale->surface = g_object_ref (surface);
  scale->resource = scale_resource;
  wl_list_insert (&surface->fractional_links, &scale->link);
  surface->fractional = scale_resource;

  wl_resource_set_implementation (scale_resource, &fractional_scale_impl, scale,
                                  fractional_scale_resource_destroyed);

  wp_fractional_scale_v1_send_preferred_scale (scale_resource,
                                               nested_scale_units (self));
}

static const struct wp_fractional_scale_manager_v1_interface
fractional_scale_manager_impl = {
  .destroy = fractional_scale_manager_destroy,
  .get_fractional_scale = fractional_scale_manager_get_scale,
};

static void
fractional_scale_manager_bind (struct wl_client *client, void *data,
                               uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_fractional_scale_manager_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &fractional_scale_manager_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * wl_data_device_manager                                             *
 *                                                                    *
 * Clipboard only. A drag needs a pointer-owned icon and a second      *
 * surface to drop onto, and this compositor has neither, so           *
 * start_drag is ignored: answering a drag with a protocol error would *
 * kill the client for something as ordinary as selecting text.        *
 * ------------------------------------------------------------------ */

#define NESTED_DATA_DEVICE_MANAGER_VERSION 3

/* The names a text copy travels under, best first. The X11-era ones are here
 * because clients that predate the charset suffix still mean UTF-8. */
static const char *const text_mimes[] = {
  "text/plain;charset=utf-8",
  "text/plain;charset=UTF-8",
  "UTF8_STRING",
  "TEXT",
  "text/plain",
  "STRING",
};

struct _DataSource {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  GPtrArray *mime_types;      /* char *, owned */
  struct wl_list offers;      /* DataOffer.source_link */
};

struct _DataOffer {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  DataSource *source;         /* set: forward reads to a client's source */
  struct wl_list source_link;
  struct wl_list device_link;
};

struct _DataDevice {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  struct wl_list link;
  struct wl_list offers;      /* DataOffer.device_link */
};

struct _ClipboardTransfer {
  NestedCompositor *compositor;
  int fd;                 /* read end of the pipe, non-blocking */
  guint source_id;        /* the watch that drains it */
  GByteArray *data;
  char *mime;
};

/* A copy is text, not a file: past that it is a client misbehaving, and the UI
 * process must not be talked into exhausting its memory. */
#define CLIPBOARD_MAX_BYTES (4 * 1024 * 1024)

static const struct wl_data_source_interface data_source_impl;
static const struct wl_data_offer_interface data_offer_impl;
static const struct wl_data_device_interface data_device_impl;

static void data_offer_resource_destroyed (struct wl_resource *resource);
static void compositor_advertise_selection (NestedCompositor *self, DataSource *source);
static void compositor_clipboard_read_done (GObject *clipboard, GAsyncResult *result,
                                            gpointer user_data);

static gboolean
mime_is_text (const char *mime)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
    if (!strcmp (mime, text_mimes[i]))
      return TRUE;

  return g_str_has_prefix (mime, "text/");
}

/* Any text type can be served as any other: the payload is the same UTF-8 and
 * the difference is only a promise about the missing terminator. */
static gboolean
mime_can_serve (const char *have, const char *asked)
{
  if (!strcmp (have, asked))
    return TRUE;

  return mime_is_text (have) && mime_is_text (asked);
}

static const char *
pick_text_mime (GPtrArray *types)
{
  gsize i, j;

  for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
    for (j = 0; j < types->len; j++)
      if (!strcmp ((const char *) g_ptr_array_index (types, j), text_mimes[i]))
        return text_mimes[i];

  return types->len > 0 ? g_ptr_array_index (types, 0) : NULL;
}

/* ------------------------------------------------------------ one client copy */

static void
clipboard_transfer_destroy (ClipboardTransfer *transfer)
{
  NestedCompositor *self = transfer->compositor;

  if (self && self->clipboard_transfer == transfer)
    self->clipboard_transfer = NULL;

  if (transfer->source_id != 0)
    g_source_remove (transfer->source_id);
  close (transfer->fd);
  g_byte_array_unref (transfer->data);
  g_free (transfer->mime);
  g_free (transfer);
}

static void
clipboard_transfer_cancel (ClipboardTransfer *transfer)
{
  if (transfer)
    clipboard_transfer_destroy (transfer);
}

static void
clipboard_transfer_finish (ClipboardTransfer *transfer, gboolean mirror)
{
  NestedCompositor *self = transfer->compositor;
  char *text = NULL;

  if (mirror && mime_is_text (transfer->mime))
    {
      /* Some clients count the terminator in the payload. */
      while (transfer->data->len > 0 && transfer->data->data[transfer->data->len - 1] == 0)
        transfer->data->len--;

      text = g_strndup ((const char *) transfer->data->data, transfer->data->len);
      {
        char *valid = g_utf8_make_valid (text, -1);

        g_free (text);
        text = valid;
      }
    }

  if (text && self && self->clipboard && !self->clipboard_echo)
    {
      self->clipboard_echo++;
      gdk_clipboard_set_text (self->clipboard, text);
      self->clipboard_echo--;

      /* The widget clipboard and the mirror have to agree: the copying client
       * may be gone by the time another one asks, and a paste must not depend
       * on the notification for a change we made ourselves. */
      g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
      g_clear_pointer (&self->clipboard_mime, g_free);
      self->clipboard_bytes = g_bytes_new (text, strlen (text));
      self->clipboard_mime = g_strdup (text_mimes[0]);
    }

  g_debug ("nested: a client copied %s (%u bytes), %s", transfer->mime,
           transfer->data->len, mirror ? (text ? "the widget clipboard now holds it"
                                               : "which is not bridged")
                                        : "and it was superseded");

  g_free (text);

  /* Reached from the watch itself: returning G_SOURCE_REMOVE detaches it, so
   * only the descriptor and the buffers are ours to free. */
  transfer->source_id = 0;
  clipboard_transfer_destroy (transfer);
}

static gboolean
clipboard_transfer_on_data (int fd, GIOCondition condition, gpointer user_data)
{
  ClipboardTransfer *transfer = user_data;
  guint8 chunk[8192];

  for (;;)
    {
      ssize_t n = read (fd, chunk, sizeof chunk);

      if (n > 0)
        {
          g_byte_array_append (transfer->data, chunk, (gsize) n);
          if (transfer->data->len > CLIPBOARD_MAX_BYTES)
            {
              g_debug ("nested: a copy bigger than %u bytes: dropped",
                       CLIPBOARD_MAX_BYTES);
              clipboard_transfer_finish (transfer, FALSE);
              return G_SOURCE_REMOVE;
            }
          continue;
        }

      if (n < 0 && errno == EAGAIN)
        return G_SOURCE_CONTINUE;
      if (n < 0 && errno == EINTR)
        continue;

      /* n == 0 means the writer is done; a negative other than EAGAIN means it
       * went wrong. Either way the payload is as complete as it will get. */
      break;
    }

  /* A finished copy looks exactly like a hangup: the client closed its write
   * end. Only speak up when nothing came through. */
  if (transfer->data->len == 0)
    g_debug ("nested: a copy arrived empty");

  clipboard_transfer_finish (transfer, TRUE);
  return G_SOURCE_REMOVE;
}

static void
client_selection_transfer (NestedCompositor *self, DataSource *source, const char *mime)
{
  ClipboardTransfer *transfer;
  int fds[2];

  if (pipe2 (fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
      g_warning ("nested: cannot make a pipe for the selection: %s", g_strerror (errno));
      return;
    }

  /* libwayland dups the fd while encoding, so the write end stays ours and we
   * are the ones who must close it. */
  wl_data_source_send_send (source->resource, mime, fds[1]);
  close (fds[1]);

  transfer = g_new0 (ClipboardTransfer, 1);
  transfer->compositor = self;
  transfer->mime = g_strdup (mime);
  transfer->data = g_byte_array_new ();
  transfer->fd = fds[0];
  self->clipboard_transfer = transfer;

  transfer->source_id =
    g_unix_fd_add_full (G_PRIORITY_DEFAULT, fds[0],
                        G_IO_IN | G_IO_ERR | G_IO_HUP,
                        clipboard_transfer_on_data, transfer, NULL);
}

/* ----------------------------------------------------- serving a client paste */

typedef struct {
  GBytes *bytes;
  int fd;
} ClipboardWrite;

static void
clipboard_write_run (GTask *task, gpointer source_object, gpointer data,
                     GCancellable *cancellable)
{
  ClipboardWrite *job = data;
  const guint8 *payload;
  gsize size, offset = 0;

  payload = g_bytes_get_data (job->bytes, &size);

  /* A slow target must not stall the event loop, which is why this runs on a
   * thread and why a short write simply continues. */
  while (offset < size)
    {
      ssize_t n = write (job->fd, payload + offset, size - offset);

      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }

      offset += (gsize) n;
    }

  g_task_return_boolean (task, offset == size);
}

static void
clipboard_write_done (GObject *object, GAsyncResult *result, gpointer data)
{
  ClipboardWrite *job = data;

  if (!g_task_propagate_boolean (G_TASK (result), NULL))
    g_debug ("nested: the paste target stopped reading");

  close (job->fd);
  g_bytes_unref (job->bytes);
  g_free (job);
}

static void
clipboard_send_bytes (GBytes *bytes, int fd)
{
  ClipboardWrite *job;
  GTask *task;

  job = g_new0 (ClipboardWrite, 1);
  job->bytes = g_bytes_ref (bytes);
  job->fd = fd;

  /* The thread function is handed the task data, not the callback data, so the
   * job has to be reachable through both. */
  task = g_task_new (NULL, NULL, clipboard_write_done, job);
  g_task_set_task_data (task, job, NULL);
  g_task_run_in_thread (task, clipboard_write_run);
  g_object_unref (task);
}

/* The mirror is taken from change notifications, and a widget clipboard that
 * is busy handing ownership back can report nothing at that instant. Reading
 * again when the paste is asked for is both fresher and the only way out of
 * that state, so a stale mirror costs a round trip instead of an empty paste. */
typedef struct {
  NestedCompositor *self;
  int fd;
} ClipboardPaste;

static void
clipboard_paste_read_done (GObject *clipboard, GAsyncResult *result, gpointer data)
{
  ClipboardPaste *paste = data;
  char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard), result, NULL);

  if (text && *text)
    {
      GBytes *bytes = g_bytes_new_take (text, strlen (text));

      clipboard_send_bytes (bytes, paste->fd);
      g_bytes_unref (bytes);
    }
  else
    {
      g_free (text);
      close (paste->fd);
      g_debug ("nested: the widget clipboard had nothing to paste");
    }

  g_object_unref (paste->self);
  g_free (paste);
}

static void
clipboard_paste_later (NestedCompositor *self, int fd)
{
  ClipboardPaste *paste;

  if (!self->clipboard)
    {
      close (fd);
      return;
    }

  paste = g_new0 (ClipboardPaste, 1);
  paste->self = g_object_ref (self);
  paste->fd = fd;
  gdk_clipboard_read_text_async (self->clipboard, NULL,
                                 clipboard_paste_read_done, paste);
}

/* ------------------------------------------------------------- the offers */

static void
data_device_advertise (DataDevice *device, DataSource *source)
{
  NestedCompositor *self = device->compositor;
  struct wl_client *client = wl_resource_get_client (device->resource);
  struct wl_resource *offer_resource;
  GPtrArray *types;
  DataOffer *offer;
  gsize i;

  if (source && wl_resource_get_client (source->resource) == client)
    return;                 /* nobody is handed back their own copy */

  if (!source && !self->clipboard_bytes)
    {
      /* A device that was handed an offer has to be told there is nothing
       * now, not left holding one whose owner is gone. */
      g_debug ("nested: nothing to offer a data device");
      wl_data_device_send_selection (device->resource, NULL);
      return;
    }

  types = g_ptr_array_new ();
  if (source)
    for (i = 0; i < source->mime_types->len; i++)
      g_ptr_array_add (types, g_ptr_array_index (source->mime_types, i));
  else
    for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
      g_ptr_array_add (types, (char *) text_mimes[i]);

  if (types->len == 0)
    {
      g_ptr_array_free (types, TRUE);
      return;
    }

  offer_resource = wl_resource_create (client, &wl_data_offer_interface,
                                       wl_resource_get_version (device->resource), 0);
  if (!offer_resource)
    {
      wl_client_post_no_memory (client);
      g_ptr_array_free (types, TRUE);
      return;
    }

  offer = g_new0 (DataOffer, 1);
  offer->resource = offer_resource;
  offer->compositor = self;
  offer->source = source;
  wl_list_init (&offer->source_link);
  wl_list_init (&offer->device_link);
  wl_list_insert (&device->offers, &offer->device_link);
  if (source)
    wl_list_insert (&source->offers, &offer->source_link);
  wl_resource_set_implementation (offer_resource, &data_offer_impl, offer,
                                  data_offer_resource_destroyed);

  /* data_offer is the event that actually creates the object on the client:
   * selection only names it, so introducing the offer comes first. */
  wl_data_device_send_data_offer (device->resource, offer_resource);

  for (i = 0; i < types->len; i++)
    wl_data_offer_send_offer (offer_resource, g_ptr_array_index (types, i));
  g_ptr_array_free (types, TRUE);

  if (wl_resource_get_version (device->resource) >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
    wl_data_offer_send_source_actions (offer_resource,
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

  wl_data_device_send_selection (device->resource, offer_resource);
  g_debug ("nested: offered the selection as id %u, version %u",
           wl_resource_get_id (offer_resource),
           wl_resource_get_version (offer_resource));
}

/* Each device gets its own offer object: the protocol has the client destroy
 * the previous one when a new selection arrives, so the compositor must not
 * retire it early or the client's destroy request lands on a dead id. */
static void
compositor_advertise_selection (NestedCompositor *self, DataSource *source)
{
  struct wl_list *link;

  for (link = self->data_devices.next; link != &self->data_devices; link = link->next)
    {
      DataDevice *device = wl_container_of (link, device, link);

      data_device_advertise (device, source);
    }
}

/* ------------------------------------------------------------ the GTK clipboard */

static void
compositor_paste_clipboard (NestedCompositor *self)
{
  const char *const *types;
  const char *mime = NULL;
  gsize n_types = 0;
  gsize i;

  if (!self->clipboard || self->clipboard_echo)
    return;

  types = gdk_content_formats_get_mime_types (gdk_clipboard_get_formats (self->clipboard),
                                              &n_types);
  for (i = 0; i < n_types && !mime; i++)
    if (mime_is_text (types[i]))
      mime = types[i];

  if (!mime)
    {
      /* Images and file lists are not bridged: the clients get told there is
       * no selection rather than a stale text one. */
      g_debug ("nested: the widget clipboard holds no text (%u formats)",
               (guint) n_types);
      g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
      g_clear_pointer (&self->clipboard_mime, g_free);
      if (!self->selection_source)
        compositor_advertise_selection (self, NULL);
      return;
    }

  gdk_clipboard_read_text_async (self->clipboard, NULL,
                                 (GAsyncReadyCallback) compositor_clipboard_read_done, self);
}

static void
compositor_clipboard_read_done (GObject *clipboard, GAsyncResult *result, gpointer user_data)
{
  NestedCompositor *self = user_data;
  GError *error = NULL;
  char *text;

  text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard), result, &error);
  if (!text)
    {
      g_debug ("nested: the clipboard gave no text: %s",
               error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
  g_clear_pointer (&self->clipboard_mime, g_free);

  self->clipboard_bytes = g_bytes_new (text, strlen (text));
  self->clipboard_mime = g_strdup (text_mimes[0]);
  g_free (text);

  g_debug ("nested: the widget clipboard holds %u bytes of text",
           (guint) g_bytes_get_size (self->clipboard_bytes));

  /* A client that owns the selection is served from its source, not from here. */
  if (!self->selection_source)
    compositor_advertise_selection (self, NULL);
}

static void
compositor_on_clipboard_changed (GObject *clipboard, GParamSpec *pspec, gpointer data)
{
  compositor_paste_clipboard (data);
}

/* ------------------------------------------------------------- wl_data_offer */

static void
data_offer_accept (struct wl_client *client, struct wl_resource *resource,
                   uint32_t serial, const char *mime_type)
{
  /* Only drags use it, and there are no drags here. */
}

static void
data_offer_receive (struct wl_client *client, struct wl_resource *resource,
                    const char *mime_type, int32_t fd)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  NestedCompositor *self = offer->compositor;

  if (offer->source)
    {
      /* The owner of the selection writes into the fd the requester made;
       * libwayland dups it, so our copy can go. */
      g_debug ("nested: passing %s to the client that copied it", mime_type);
      wl_data_source_send_send (offer->source->resource, mime_type, fd);
      close (fd);
      return;
    }

  if (self->clipboard_mime && !mime_can_serve (self->clipboard_mime, mime_type))
    {
      g_debug ("nested: %s asked for, the clipboard holds %s", mime_type,
               self->clipboard_mime);
      close (fd);
      return;
    }

  if (!self->clipboard_bytes)
    {
      g_debug ("nested: the mirror is empty, reading the clipboard for the paste");
      clipboard_paste_later (self, fd);
      return;
    }

  g_debug ("nested: pasting %u bytes of the widget clipboard",
           (guint) g_bytes_get_size (self->clipboard_bytes));
  clipboard_send_bytes (self->clipboard_bytes, fd);
}

static void
data_offer_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_offer_resource_destroyed (struct wl_resource *resource)
{
  DataOffer *offer = wl_resource_get_user_data (resource);

  wl_list_remove (&offer->device_link);
  if (offer->source)
    wl_list_remove (&offer->source_link);
  g_free (offer);
}

static const struct wl_data_offer_interface data_offer_impl = {
  .accept = data_offer_accept,
  .receive = data_offer_receive,
  .destroy = data_offer_destroy,
};

/* ------------------------------------------------------------ wl_data_source */

static void
data_source_offer (struct wl_client *client, struct wl_resource *resource,
                   const char *mime_type)
{
  DataSource *source = wl_resource_get_user_data (resource);
  gsize i;

  for (i = 0; i < source->mime_types->len; i++)
    if (!strcmp (g_ptr_array_index (source->mime_types, i), mime_type))
      return;

  g_ptr_array_add (source->mime_types, g_strdup (mime_type));
}

static void
data_source_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_source_resource_destroyed (struct wl_resource *resource)
{
  DataSource *source = wl_resource_get_user_data (resource);
  NestedCompositor *self = source->compositor;

  /* The offers outlive the source: their clients still hold them and will
   * destroy them themselves. Detached, they fall back to the widget
   * clipboard, which still holds what was copied. */
  while (!wl_list_empty (&source->offers))
    {
      DataOffer *offer = wl_container_of (source->offers.next, offer, source_link);

      wl_list_remove (&offer->source_link);
      wl_list_init (&offer->source_link);
      offer->source = NULL;
    }

  if (self->selection_source == source)
    {
      self->selection_source = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);

      /* The copying client is gone: the widget clipboard is the selection
       * again, and whoever is still connected has to be told. */
      compositor_advertise_selection (self, NULL);
    }

  g_clear_pointer (&source->mime_types, g_ptr_array_unref);
  g_free (source);
}

static const struct wl_data_source_interface data_source_impl = {
  .offer = data_source_offer,
  .destroy = data_source_destroy,
};

/* ----------------------------------------------------------- wl_data_device */

static void
data_device_start_drag (struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *source, struct wl_resource *origin,
                        struct wl_resource *icon, uint32_t serial)
{
  g_debug ("nested: ignoring a drag: a surface inside a widget has nowhere to drop");
}

static void
data_device_set_selection (struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *source_resource, uint32_t serial)
{
  NestedCompositor *self =
    ((DataDevice *) wl_resource_get_user_data (resource))->compositor;
  DataSource *source = source_resource ? wl_resource_get_user_data (source_resource) : NULL;
  const char *mime;

  if (self->selection_source && self->selection_source != source)
    wl_data_source_send_cancelled (self->selection_source->resource);

  clipboard_transfer_cancel (self->clipboard_transfer);
  self->selection_source = source;

  if (!source)
    {
      compositor_advertise_selection (self, NULL);
      return;
    }

  compositor_advertise_selection (self, source);

  mime = pick_text_mime (source->mime_types);
  if (mime)
    client_selection_transfer (self, source, mime);
  else
    g_debug ("nested: a client took a selection offering nothing we can bridge");
}

static void
data_device_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_device_resource_destroyed (struct wl_resource *resource)
{
  DataDevice *device = wl_resource_get_user_data (resource);

  while (!wl_list_empty (&device->offers))
    {
      DataOffer *offer = wl_container_of (device->offers.next, offer, device_link);

      wl_resource_destroy (offer->resource);
    }

  wl_list_remove (&device->link);
  g_free (device);
}

static const struct wl_data_device_interface data_device_impl = {
  .start_drag = data_device_start_drag,
  .set_selection = data_device_set_selection,
  .release = data_device_release,
};

/* ------------------------------------------------------ wl_data_device_manager */

static void
data_device_manager_create_data_source (struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *source_resource;
  DataSource *source;

  source = g_new0 (DataSource, 1);
  source->compositor = self;
  source->mime_types = g_ptr_array_new_with_free_func (g_free);
  wl_list_init (&source->offers);

  source_resource = wl_resource_create (client, &wl_data_source_interface,
                                        wl_resource_get_version (resource), id);
  source->resource = source_resource;
  wl_resource_set_implementation (source_resource, &data_source_impl, source,
                                  data_source_resource_destroyed);
}

static void
data_device_manager_get_data_device (struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id, struct wl_resource *seat)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *device_resource;
  DataDevice *device;

  device = g_new0 (DataDevice, 1);
  device->compositor = self;
  wl_list_init (&device->offers);

  device_resource = wl_resource_create (client, &wl_data_device_interface,
                                        wl_resource_get_version (resource), id);
  device->resource = device_resource;
  wl_resource_set_implementation (device_resource, &data_device_impl, device,
                                  data_device_resource_destroyed);
  wl_list_insert (&self->data_devices, &device->link);

  /* A client that starts after the copy still has to be able to paste, and the
   * mirror may not have been taken while there was nothing to take. */
  if (!self->selection_source && !self->clipboard_bytes)
    compositor_paste_clipboard (self);
  else
    data_device_advertise (device, self->selection_source);
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
  .create_data_source = data_device_manager_create_data_source,
  .get_data_device = data_device_manager_get_data_device,
};

static void
data_device_manager_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_data_device_manager_interface, version, id);
  wl_resource_set_implementation (resource, &data_device_manager_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * zwp_text_input_manager_v3                                          *
 *                                                                    *
 * The nested compositor is the input-method host: a client describes  *
 * its caret, the text around it and what the field is for, and the     *
 * embedding widget's own GtkIMContext composes into preedit and        *
 * commit events. State is double-buffered as the protocol describes    *
 * it: the requests fill the pending copy, commit() makes it current,   *
 * and done() carries the commit count as the serial the client has to  *
 * see before it applies what we sent.                                  *
 * ------------------------------------------------------------------ */

#define NESTED_TEXT_INPUT_VERSION 1

/* Indices go over the wire in bytes; the widget side works in characters. */
struct _TextInputState {
  gboolean enabled;
  char *surrounding;                /* UTF-8, as the client reported it */
  gint32 cursor, anchor;            /* bytes into surrounding */
  guint32 change_cause;
  guint32 hint, purpose;            /* zwp_text_input_v3_content_* */
  gint32 x, y;
  gint32 width, height;
  gboolean has_rect;
};

struct _TextInput {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  NestedSurface *surface;           /* set between enter and leave */
  gboolean entered;
  TextInputState pending, current;
  char *preedit;                    /* the last preedit_string we sent */
  guint32 commits;                  /* the serial done() events use */
  struct wl_list link;
};

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl;
static const struct zwp_text_input_v3_interface text_input_impl;

static void
text_input_state_clear (TextInputState *state)
{
  g_clear_pointer (&state->surrounding, g_free);
  memset (state, 0, sizeof (*state));
}

static void
text_input_state_apply (TextInputState *to, const TextInputState *from)
{
  text_input_state_clear (to);
  *to = *from;
  to->surrounding = from->surrounding ? g_strdup (from->surrounding) : NULL;
}

/* A password field must not have its contents handed to an input method, which
 * is often a separate process with its own logging. The client's hint is the
 * only thing that says so, so it is honoured even where it costs features. */
static gboolean
text_input_state_is_sensitive (const TextInputState *state)
{
  return (state->hint & (ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT |
                         ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA)) != 0 ||
         state->purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
}

static void
text_input_done (TextInput *input)
{
  zwp_text_input_v3_send_done (input->resource, input->commits);
}

static void
text_input_update_active (NestedCompositor *self)
{
  TextInput *input;
  TextInput *found = NULL;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->entered && input->current.enabled)
        {
          found = input;
          break;
        }
    }

  self->active_input = found;

  /* The widget reads the new state through the getters when it is told about
   * it, so one signal covers "composition possible", "not any more" and
   * "the caret or the text around it moved". */
  g_signal_emit (self, signals[SIGNAL_TEXT_INPUT_STATE], 0);
}

static void
text_input_enter (NestedCompositor *self, TextInput *input, NestedSurface *surface)
{
  input->entered = TRUE;
  g_set_object (&input->surface, surface);

  /* "After an enter event ... all state information is invalidated": the
   * client has to send it again, so throw the old copy away now. */
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);

  g_debug ("nested: text input entered on the keyboard-focused surface");

  zwp_text_input_v3_send_enter (input->resource, surface->wl_surface);
}

static void
text_input_leave (NestedCompositor *self, TextInput *input)
{
  if (!input->entered)
    return;

  zwp_text_input_v3_send_leave (input->resource,
                                input->surface ? input->surface->wl_surface : NULL);

  input->entered = FALSE;
  g_clear_object (&input->surface);
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);
}

/* Focus moved to @surface (or to nothing): every text input of the client that
 * owns the surface enters, the rest leave. */
static void
text_input_retarget (NestedCompositor *self, NestedSurface *surface)
{
  TextInput *input;

  if (!self)
    return;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->entered && input->surface != surface)
        text_input_leave (self, input);

      if (!input->entered && surface &&
          wl_resource_get_client (input->resource) ==
            wl_resource_get_client (surface->wl_surface))
        text_input_enter (self, input, surface);
    }

  text_input_update_active (self);
}

/* The surface is gone from the protocol already, so its inputs can only be
 * dropped, not told goodbye. */
static void
text_input_forget_surface (NestedCompositor *self, NestedSurface *surface)
{
  TextInput *input;

  if (!self)
    return;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->surface != surface)
        continue;

      input->entered = FALSE;
      g_clear_object (&input->surface);
      text_input_state_clear (&input->pending);
      text_input_state_clear (&input->current);
      g_clear_pointer (&input->preedit, g_free);
    }

  text_input_update_active (self);
}

static TextInput *
text_input_from_resource (struct wl_resource *resource)
{
  return wl_resource_get_user_data (resource);
}

static void
text_input_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
text_input_enable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  if (!input->entered)
    {
      g_debug ("nested: a text input was enabled before it was entered: ignored");
      return;
    }

  /* "At most one instance of text input may be in enabled state per instance":
   * a second client cannot take the keyboard's composition away. */
  if (self->active_input && self->active_input != input &&
      self->active_input->current.enabled)
    {
      g_debug ("nested: a text input is already enabled on this seat: ignored");
      return;
    }

  /* enable() resets everything that came before it. */
  text_input_state_clear (&input->pending);
  input->pending.enabled = TRUE;
}

static void
text_input_disable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.enabled = FALSE;
}

static void
text_input_set_surrounding_text (struct wl_client *client,
                                 struct wl_resource *resource,
                                 const char *text, int32_t cursor, int32_t anchor)
{
  TextInput *input = text_input_from_resource (resource);
  gsize size;

  if (!input->entered)
    return;

  g_free (input->pending.surrounding);
  input->pending.surrounding = g_strdup (text && *text ? text : NULL);
  size = input->pending.surrounding ? strlen (input->pending.surrounding) : 0;

  /* The protocol puts the indices inside the buffer; a client that is off by
   * one must not talk the compositor into reading outside it. */
  if (cursor < 0)
    cursor = 0;
  if ((gsize) cursor > size)
    cursor = (gint32) size;
  if (anchor < 0)
    anchor = 0;
  if ((gsize) anchor > size)
    anchor = (gint32) size;

  input->pending.cursor = cursor;
  input->pending.anchor = anchor;
}

static void
text_input_set_text_change_cause (struct wl_client *client,
                                  struct wl_resource *resource, uint32_t cause)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.change_cause = cause;
}

static void
text_input_set_content_type (struct wl_client *client, struct wl_resource *resource,
                             uint32_t hint, uint32_t purpose)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.hint = hint;
  input->pending.purpose = purpose;
}

static void
text_input_set_cursor_rectangle (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x, int32_t y, int32_t width, int32_t height)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.x = x;
  input->pending.y = y;
  input->pending.width = width;
  input->pending.height = height;
  input->pending.has_rect = width > 0 && height > 0;
}

static void
text_input_commit (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  input->commits++;
  text_input_state_apply (&input->current, &input->pending);

  /* Never the text itself: a sensitive field's contents stay out of the log. */
  g_debug ("nested: text input commit #%u enabled=%d caret=%d anchor=%d "
           "surrounding=%zu bytes purpose=%u hint=%u",
           input->commits, input->current.enabled, input->current.cursor,
           input->current.anchor,
           input->current.surrounding ? strlen (input->current.surrounding) : (size_t) 0,
           input->current.purpose, input->current.hint);

  /* The change cause only describes this one commit. */
  input->pending.change_cause = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;

  text_input_done (input);
  text_input_update_active (self);
  nested_flush (self);
}

static void
text_input_resource_destroyed (struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  wl_list_remove (&input->link);
  wl_list_init (&input->link);

  g_clear_object (&input->surface);
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);

  text_input_update_active (self);
  g_free (input);
}

static const struct zwp_text_input_v3_interface text_input_impl = {
  .destroy = text_input_destroy,
  .enable = text_input_enable,
  .disable = text_input_disable,
  .set_surrounding_text = text_input_set_surrounding_text,
  .set_text_change_cause = text_input_set_text_change_cause,
  .set_content_type = text_input_set_content_type,
  .set_cursor_rectangle = text_input_set_cursor_rectangle,
  .commit = text_input_commit,
};

static void
text_input_manager_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
text_input_manager_get_text_input (struct wl_client *client,
                                   struct wl_resource *resource, uint32_t id,
                                   struct wl_resource *seat)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *input_resource;
  TextInput *input;

  if (!wl_resource_instance_of (seat, &wl_seat_interface, &seat_impl))
    {
      post_wrong_object (resource, seat, "wl_seat");
      return;
    }

  input_resource = wl_resource_create (client, &zwp_text_input_v3_interface,
                                       wl_resource_get_version (resource), id);
  input = g_new0 (TextInput, 1);
  input->resource = input_resource;
  input->compositor = self;
  wl_list_init (&input->link);
  wl_list_insert (&self->text_inputs, &input->link);

  wl_resource_set_implementation (input_resource, &text_input_impl, input,
                                  text_input_resource_destroyed);

  /* A client that binds late still has to be told which surface it composes
   * for, or it never gets an enter and stays silent. */
  if (self->keyboard_focus &&
      wl_resource_get_client (self->keyboard_focus->wl_surface) == client)
    text_input_enter (self, input, self->keyboard_focus);

  text_input_update_active (self);
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl = {
  .destroy = text_input_manager_destroy,
  .get_text_input = text_input_manager_get_text_input,
};

static void
text_input_manager_bind (struct wl_client *client, void *data, uint32_t version,
                         uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_text_input_manager_v3_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &text_input_manager_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * xdg_wm_base                                                        *
 * ------------------------------------------------------------------ */

static void
xdg_wm_base_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_noop (struct wl_client *client, struct wl_resource *resource)
{
}

static void
xdg_noop_ii (struct wl_client *client, struct wl_resource *resource,
             int32_t a, int32_t b)
{
}

static void
xdg_noop_iiii (struct wl_client *client, struct wl_resource *resource,
               int32_t a, int32_t b, int32_t c, int32_t d)
{
}

static void
xdg_noop_u (struct wl_client *client, struct wl_resource *resource, uint32_t a)
{
}

static void
xdg_positioner_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct xdg_positioner_interface xdg_positioner_impl = {
  .destroy = xdg_positioner_destroy,
  .set_size = xdg_noop_ii,
  .set_anchor_rect = xdg_noop_iiii,
  .set_anchor = xdg_noop_u,
  .set_gravity = xdg_noop_u,
  .set_constraint_adjustment = xdg_noop_u,
  .set_offset = xdg_noop_ii,
  .set_reactive = xdg_noop,
  .set_parent_size = xdg_noop_ii,
  .set_parent_configure = xdg_noop_u,
};

static void
xdg_wm_base_create_positioner (struct wl_client *client, struct wl_resource *resource,
                               uint32_t id)
{
  struct wl_resource *positioner;

  positioner = wl_resource_create (client, &xdg_positioner_interface,
                                   wl_resource_get_version (resource), id);
  wl_resource_set_implementation (positioner, &xdg_positioner_impl, NULL, NULL);
}

static void xdg_surface_resource_destroyed (struct wl_resource *resource);
static void xdg_toplevel_resource_destroyed (struct wl_resource *resource);
static void xdg_popup_resource_destroyed (struct wl_resource *resource);

static void
xdg_wm_base_get_xdg_surface (struct wl_client *client, struct wl_resource *resource,
                             uint32_t id, struct wl_resource *surface_resource)
{
  struct wl_resource *xdg_resource, *output;
  NestedCompositor *self = wl_resource_get_user_data (resource);
  NestedSurface *surface;

  surface = surface_from_resource (resource, surface_resource);
  if (!surface)
    return;

  if (surface->xdg_surface)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
                              "surface already has a role");
      return;
    }

  xdg_resource = wl_resource_create (client, &xdg_surface_interface,
                                     wl_resource_get_version (resource), id);
  surface->xdg_surface = xdg_resource;
  wl_resource_set_implementation (xdg_resource, &xdg_surface_impl,
                                  g_object_ref (surface),
                                  xdg_surface_resource_destroyed);

  output = find_output_for_client (self, client);
  if (output)
    wl_surface_send_enter (surface_resource, output);
}

static void
xdg_wm_base_pong (struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
  .destroy = xdg_wm_base_destroy,
  .create_positioner = xdg_wm_base_create_positioner,
  .get_xdg_surface = xdg_wm_base_get_xdg_surface,
  .pong = xdg_wm_base_pong,
};

static void
xdg_wm_base_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &xdg_wm_base_interface, version, id);
  wl_resource_set_implementation (resource, &xdg_wm_base_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * xdg_surface                                                        *
 * ------------------------------------------------------------------ */

static void
xdg_surface_configure (NestedSurface *self)
{
  NestedCompositor *nested = self->nested;
  uint32_t serial;

  if (!nested || !self->xdg_surface)
    return;

  serial = ++nested->serial;
  self->configure_serial = serial;

  if (self->toplevel)
    {
      struct wl_array states;
      uint32_t *state;

      wl_array_init (&states);
      if (self->width == nested->width && self->height == nested->height)
        {
          state = wl_array_add (&states, sizeof (*state));
          if (state)
            *state = XDG_TOPLEVEL_STATE_MAXIMIZED;
        }
      xdg_toplevel_send_configure (self->toplevel, nested->width, nested->height,
                                   &states);
      wl_array_release (&states);
    }
  else if (self->popup)
    {
      int width = self->geom_width > 0 ? self->geom_width : 1;
      int height = self->geom_height > 0 ? self->geom_height : 1;

      xdg_popup_send_configure (self->popup, 0, 0, width, height);
    }

  if (wl_resource_get_version (self->xdg_surface) >= XDG_SURFACE_CONFIGURE_SINCE_VERSION)
    xdg_surface_send_configure (self->xdg_surface, serial);

  nested_flush (nested);
}

static void
xdg_surface_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_surface_resource_destroyed (struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  NestedCompositor *nested = self->nested;

  self->xdg_surface = NULL;
  self->mapped = FALSE;

  if (nested && self == nested->primary)
    nested_update_primary (nested);

  g_object_unref (self);
}

static void
xdg_surface_get_toplevel (struct wl_client *client, struct wl_resource *resource,
                          uint32_t id)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  struct wl_resource *toplevel;

  if (self->is_toplevel || self->is_popup)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
                              "surface already has a role");
      return;
    }

  toplevel = wl_resource_create (client, &xdg_toplevel_interface,
                                 wl_resource_get_version (resource), id);
  self->toplevel = toplevel;
  self->is_toplevel = TRUE;
  wl_resource_set_implementation (toplevel, &xdg_toplevel_impl, g_object_ref (self),
                                  xdg_toplevel_resource_destroyed);

  xdg_surface_configure (self);
}

static void
xdg_surface_get_popup (struct wl_client *client, struct wl_resource *resource,
                       uint32_t id, struct wl_resource *parent_resource,
                       struct wl_resource *positioner_resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  struct wl_resource *popup;

  if (!parent_resource ||
      !wl_resource_instance_of (parent_resource, &xdg_surface_interface,
                                &xdg_surface_impl))
    {
      post_wrong_object (resource, parent_resource, "xdg_surface");
      return;
    }

  if (!positioner_resource ||
      !wl_resource_instance_of (positioner_resource, &xdg_positioner_interface,
                                &xdg_positioner_impl))
    {
      post_wrong_object (resource, positioner_resource, "xdg_positioner");
      return;
    }

  if (self->is_toplevel || self->is_popup)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
                              "surface already has a role");
      return;
    }

  popup = wl_resource_create (client, &xdg_popup_interface,
                              wl_resource_get_version (resource), id);
  self->popup = popup;
  self->is_popup = TRUE;
  wl_resource_set_implementation (popup, &xdg_popup_impl, g_object_ref (self),
                                  xdg_popup_resource_destroyed);

  xdg_surface_configure (self);
}

static void
xdg_surface_set_window_geometry (struct wl_client *client, struct wl_resource *resource,
                                 int32_t x, int32_t y, int32_t width, int32_t height)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  self->geom_width = width;
  self->geom_height = height;
}

static void
xdg_surface_ack_configure (struct wl_client *client, struct wl_resource *resource,
                           uint32_t serial)
{
}

static const struct xdg_surface_interface xdg_surface_impl = {
  .destroy = xdg_surface_destroy,
  .get_toplevel = xdg_surface_get_toplevel,
  .get_popup = xdg_surface_get_popup,
  .set_window_geometry = xdg_surface_set_window_geometry,
  .ack_configure = xdg_surface_ack_configure,
};

/* ------------------------------------------------------------------ *
 * xdg_toplevel                                                       *
 * ------------------------------------------------------------------ */

static void
xdg_toplevel_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_toplevel_resource_destroyed (struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  NestedCompositor *nested = self->nested;

  self->toplevel = NULL;
  self->is_toplevel = FALSE;
  self->mapped = FALSE;

  if (nested && self == nested->primary)
    nested_update_primary (nested);

  g_object_unref (self);
}

static void
xdg_toplevel_set_title (struct wl_client *client, struct wl_resource *resource,
                        const char *title)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  g_free (self->title);
  self->title = g_strdup (title);
}

static void
xdg_toplevel_set_app_id (struct wl_client *client, struct wl_resource *resource,
                         const char *app_id)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  g_free (self->app_id);
  self->app_id = g_strdup (app_id);
}

static void
xdg_toplevel_set_parent (struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *parent)
{
}

static void
xdg_toplevel_grab (struct wl_client *client, struct wl_resource *resource,
                   struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{
}

static void
xdg_toplevel_move_resize (struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
}

static void
xdg_toplevel_set_edges (struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *seat, uint32_t serial)
{
}

static void
xdg_toplevel_set_size_hint (struct wl_client *client, struct wl_resource *resource,
                            int32_t width, int32_t height)
{
}

static void
xdg_toplevel_set_fullscreen (struct wl_client *client, struct wl_resource *resource,
                             struct wl_resource *output)
{
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
  .destroy = xdg_toplevel_destroy,
  .set_parent = xdg_toplevel_set_parent,
  .set_title = xdg_toplevel_set_title,
  .set_app_id = xdg_toplevel_set_app_id,
  .show_window_menu = xdg_toplevel_grab,
  .move = xdg_toplevel_set_edges,
  .resize = xdg_toplevel_move_resize,
  .set_max_size = xdg_toplevel_set_size_hint,
  .set_min_size = xdg_toplevel_set_size_hint,
  .set_maximized = xdg_noop,
  .unset_maximized = xdg_noop,
  .set_fullscreen = xdg_toplevel_set_fullscreen,
  .unset_fullscreen = xdg_noop,
  .set_minimized = xdg_noop,
};

/* ------------------------------------------------------------------ *
 * xdg_popup                                                          *
 * ------------------------------------------------------------------ */

static void
xdg_popup_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_popup_resource_destroyed (struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  self->popup = NULL;
  self->is_popup = FALSE;
  self->mapped = FALSE;
  g_object_unref (self);
}

static void
xdg_popup_grab (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *seat, uint32_t serial)
{
}

static void
xdg_popup_reposition (struct wl_client *client, struct wl_resource *resource,
                      struct wl_resource *positioner, uint32_t token)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  xdg_surface_configure (self);

  if (wl_resource_get_version (resource) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
    xdg_popup_send_repositioned (resource, token);
}

static const struct xdg_popup_interface xdg_popup_impl = {
  .destroy = xdg_popup_destroy,
  .grab = xdg_popup_grab,
  .reposition = xdg_popup_reposition,
};

/* ------------------------------------------------------------------ *
 * NestedCompositor                                                             *
 * ------------------------------------------------------------------ */

G_DEFINE_TYPE (NestedCompositor, nested_compositor, G_TYPE_OBJECT)

static gboolean
frame_clock_tick (gpointer data)
{
  NestedCompositor *self = data;
  uint32_t msec = (uint32_t) (g_get_monotonic_time () / 1000);
  NestedSurface *surface;
  gboolean again = FALSE;

  self->frame_timer_id = 0;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    surface_fire_frame_callbacks (surface, msec);

  nested_flush (self);

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface_has_pending_frames (surface))
        again = TRUE;
    }

  if (again)
    self->frame_timer_id = g_timeout_add (16, frame_clock_tick, self);

  return G_SOURCE_REMOVE;
}

static void
nested_start_frame_clock (NestedCompositor *self)
{
  NestedSurface *surface;

  if (self->frame_timer_id)
    return;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface_has_pending_frames (surface))
        {
          self->frame_timer_id = g_timeout_add (16, frame_clock_tick, self);
          return;
        }
    }
}

static void
nested_update_primary (NestedCompositor *self)
{
  NestedSurface *candidate = NULL, *surface;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface->is_toplevel && surface->mapped && surface->texture)
        {
          candidate = surface;
          break;
        }
    }

  if (candidate == self->primary)
    return;

  g_set_object (&self->primary, candidate);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SURFACE]);
  g_signal_emit (self, signals[SIGNAL_FRAME], 0);

  /* Input follows whatever is on screen: a client that maps a second surface,
   * or unmaps the only one, must not keep the pointer. */
  seat_retarget (self);
}

static void
nested_flush (NestedCompositor *self)
{
  if (self->display)
    wl_display_flush_clients (self->display);
}

/* g_unix_fd_source_new() reinterprets the GSourceFunc as a GUnixFDSourceFunc,
 * which is called with (fd, condition, user_data). */
static gboolean
nested_dispatch (gint fd, GIOCondition condition, gpointer data)
{
  NestedCompositor *self = data;

  wl_event_loop_dispatch (self->loop, 0);
  wl_event_loop_dispatch_idle (self->loop);
  nested_flush (self);

  return G_SOURCE_CONTINUE;
}

static void
nested_compositor_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  NestedCompositor *self = NESTED_COMPOSITOR (object);

  if (id == PROP_SURFACE)
    g_value_set_object (value, self->primary);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
nested_compositor_dispose (GObject *object)
{
  NestedCompositor *self = NESTED_COMPOSITOR (object);
  NestedSurface *surface, *tmp;

  if (self->frame_timer_id)
    {
      g_source_remove (self->frame_timer_id);
      self->frame_timer_id = 0;
    }

  if (self->source_id)
    {
      g_source_remove (self->source_id);
      self->source_id = 0;
    }
  g_clear_pointer (&self->source, g_source_unref);

  if (self->display)
    {
      /* Clients first: their resource destroy callbacks release the
       * surfaces, which are only ever touched through this list. */
      wl_display_destroy_clients (self->display);

      wl_list_for_each_safe (surface, tmp, &self->surfaces, nested_link)
        {
          wl_list_remove (&surface->nested_link);
          wl_list_init (&surface->nested_link);
          surface->in_list = FALSE;
          surface->nested = NULL;
        }

      wl_display_destroy (self->display);
      self->display = NULL;
      self->loop = NULL;
    }

  g_clear_object (&self->primary);
  g_clear_object (&self->pointer_focus);
  g_clear_object (&self->keyboard_focus);
  g_clear_pointer (&self->socket_name, g_free);

  if (self->xkb_state)
    xkb_state_unref (self->xkb_state);
  if (self->xkb_keymap)
    xkb_keymap_unref (self->xkb_keymap);
  if (self->xkb_context)
    xkb_context_unref (self->xkb_context);
  if (self->gbm)
    gbm_device_destroy (self->gbm);
  self->gbm = NULL;
  g_clear_pointer (&self->dmabuf_formats, g_array_unref);
  if (self->dmabuf_table_fd >= 0)
    close (self->dmabuf_table_fd);
  self->dmabuf_table_fd = -1;
  if (self->clipboard_transfer)
    {
      /* Its watch fires on the main loop, so it must not reach for an object
       * that is going away. */
      self->clipboard_transfer->compositor = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);
    }
  nested_compositor_set_clipboard (self, NULL);
  g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
  g_clear_pointer (&self->clipboard_mime, g_free);
  free (self->keymap_text);

  g_array_free (self->pressed_keys, TRUE);

  G_OBJECT_CLASS (nested_compositor_parent_class)->dispose (object);
}

static void
nested_compositor_init (NestedCompositor *self)
{
  self->width = 640;
  self->height = 480;
  self->scale = 1.0;
  wl_list_init (&self->outputs);
  wl_list_init (&self->surfaces);
  wl_list_init (&self->seat_clients);
  wl_list_init (&self->data_devices);
  wl_list_init (&self->text_inputs);
  self->pressed_keys = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  self->dmabuf_formats = g_array_new (FALSE, FALSE, sizeof (DmabufFormat));
  self->dmabuf_table_fd = -1;
}

static void
nested_compositor_class_init (NestedCompositorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = nested_compositor_dispose;
  object_class->get_property = nested_compositor_get_property;

  properties[PROP_SURFACE] =
    g_param_spec_object ("surface", "Surface",
                         "The surface currently being displayed",
                         NESTED_TYPE_SURFACE,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SURFACE, properties[PROP_SURFACE]);

  /**
   * NestedCompositor::frame:
   *
   * Emitted when the displayed surface has a new frame, and when a different
   * surface becomes the one to display.
   */
  signals[SIGNAL_FRAME] =
    g_signal_new ("frame", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * NestedCompositor::text-input-state:
   *
   * Emitted when the text input the widget should compose for changes, or when
   * that input reports new surrounding text or a moved caret. The widget reads
   * the result back with nested_compositor_text_input_active() and
   * nested_compositor_get_text_input_state().
   */
  signals[SIGNAL_TEXT_INPUT_STATE] =
    g_signal_new ("text-input-state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

NestedCompositor *
nested_compositor_new (GError **error)
{
  NestedCompositor *self;
  const char *name;

  self = g_object_new (NESTED_TYPE_COMPOSITOR, NULL);

  self->display = wl_display_create ();
  if (!self->display)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create wl_display");
      g_object_unref (self);
      return NULL;
    }

  self->loop = wl_display_get_event_loop (self->display);

  name = wl_display_add_socket_auto (self->display);
  if (!name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "cannot create a wayland socket: %s", g_strerror (errno));
      wl_display_destroy (self->display);
      self->display = NULL;
      g_object_unref (self);
      return NULL;
    }
  self->socket_name = g_strdup (name);

  self->global_compositor =
    wl_global_create (self->display, &wl_compositor_interface, 4, self,
                      compositor_bind);
  self->global_shm =
    wl_global_create (self->display, &wl_shm_interface, 1, self, shm_bind);
  self->global_output =
    wl_global_create (self->display, &wl_output_interface, 4, self, output_bind);
  self->global_xdg_wm_base =
    wl_global_create (self->display, &xdg_wm_base_interface, 3, self, xdg_wm_base_bind);
  self->global_viewporter =
    wl_global_create (self->display, &wp_viewporter_interface, 1, self, viewporter_bind);
  self->global_fractional =
    wl_global_create (self->display, &wp_fractional_scale_manager_v1_interface, 1, self,
                      fractional_scale_manager_bind);

  nested_setup_keymap (self);
  self->global_seat =
    wl_global_create (self->display, &wl_seat_interface, NESTED_SEAT_VERSION, self,
                      seat_bind);
  self->global_data =
    wl_global_create (self->display, &wl_data_device_manager_interface,
                      NESTED_DATA_DEVICE_MANAGER_VERSION, self,
                      data_device_manager_bind);
  self->global_text_input =
    wl_global_create (self->display, &zwp_text_input_manager_v3_interface,
                      NESTED_TEXT_INPUT_VERSION, self, text_input_manager_bind);

  /* Optional: without a render node there is nothing to advertise and clients
   * keep using wl_shm. */
  nested_setup_dmabuf (self);
  if (self->dmabuf_formats->len > 0)
    self->global_dmabuf =
      wl_global_create (self->display, &zwp_linux_dmabuf_v1_interface,
                        NESTED_DMABUF_VERSION, self, dmabuf_bind);

  if (!self->global_compositor || !self->global_shm ||
      !self->global_output || !self->global_xdg_wm_base || !self->global_seat)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot advertise globals");
      g_object_unref (self);
      return NULL;
    }

  self->source = g_unix_fd_source_new (wl_event_loop_get_fd (self->loop),
                                       G_IO_IN | G_IO_ERR | G_IO_HUP);
  g_source_set_callback (self->source, (GSourceFunc) nested_dispatch, self, NULL);
  self->source_id = g_source_attach (self->source, NULL);

  g_debug ("nested: compositor listens on %s", self->socket_name);

  return self;
}

const char *
nested_compositor_get_socket_name (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);
  return self->socket_name;
}

void
nested_compositor_get_size (NestedCompositor *self, int *width, int *height)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  if (width)
    *width = self->width;
  if (height)
    *height = self->height;
}

void
nested_compositor_set_size (NestedCompositor *self, int width, int height)
{
  OutputResource *output;
  NestedSurface *surface;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (width >= 0 && height >= 0);

  if (self->width == width && self->height == height)
    return;

  self->width = width;
  self->height = height;

  wl_list_for_each (output, &self->outputs, link)
    output_send_state (output);

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (!surface->xdg_surface)
        continue;
      xdg_surface_configure (surface);
    }
}

void
nested_compositor_set_scale (NestedCompositor *self, double scale)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (scale > 0.0);

  double difference = scale - self->scale;

  if (difference < 0.0)
    difference = -difference;
  if (difference < 0.001)
    return;

  self->scale = scale;

  /* Clients size their buffers off preferred_scale, so a change here is a
   * resize request in everything but name. */
  compositor_send_scale (self);
}

void
nested_compositor_set_clipboard (NestedCompositor *self, GdkClipboard *clipboard)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (self->clipboard == clipboard)
    return;

  if (self->clipboard)
    {
      if (self->clipboard_handler != 0)
        g_signal_handler_disconnect (self->clipboard, self->clipboard_handler);
      self->clipboard_handler = 0;
      g_clear_object (&self->clipboard);
    }

  if (!clipboard)
    return;

  self->clipboard = g_object_ref (clipboard);
  self->clipboard_handler =
    g_signal_connect (self->clipboard, "notify::content",
                      G_CALLBACK (compositor_on_clipboard_changed), self);

  /* Whatever is already on the clipboard becomes available to clients. */
  compositor_paste_clipboard (self);
}

NestedSurface *
nested_compositor_get_surface (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);
  return self->primary;
}

uint64_t
nested_compositor_get_frame_count (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), 0);
  return self->frame_count;
}

/* ------------------------------------------------------------------ *
 * text input, widget side                                            *
 * ------------------------------------------------------------------ */

/* The protocol counts bytes, GTK counts characters, so both directions are
 * clamped against the string the client actually sent. */
static gsize
text_input_offset_to_bytes (const char *text, gint offset)
{
  gsize size = text ? strlen (text) : 0;
  const char *p;
  gsize bytes;

  if (!text)
    return 0;

  p = g_utf8_offset_to_pointer (text, MAX (offset, 0));
  bytes = (gsize) (p - text);

  return MIN (bytes, size);
}

static gint
text_input_bytes_to_offset (const char *text, gsize bytes)
{
  gsize size = text ? strlen (text) : 0;

  if (!text)
    return 0;

  return (gint) g_utf8_pointer_to_offset (text, text + MIN (bytes, size));
}

gboolean
nested_compositor_text_input_active (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);
  return self->active_input != NULL;
}

void
nested_compositor_get_text_input_state (NestedCompositor *self,
                                        const char **surrounding,
                                        int *cursor, int *anchor,
                                        GdkRectangle *cursor_rect)
{
  TextInput *input;
  const char *text;

  if (surrounding)
    *surrounding = NULL;
  if (cursor)
    *cursor = 0;
  if (anchor)
    *anchor = 0;
  if (cursor_rect)
    {
      cursor_rect->x = cursor_rect->y = 0;
      cursor_rect->width = cursor_rect->height = 0;
    }

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  /* A password field keeps its contents to itself: the input method gets the
   * caret and the purpose, never the text. */
  text = text_input_state_is_sensitive (&input->current) ? NULL : input->current.surrounding;

  if (!text && input->current.surrounding)
    g_debug ("nested: hiding the field's text from the input method "
             "(purpose=%u hint=%u)", input->current.purpose, input->current.hint);

  if (surrounding)
    *surrounding = text;
  if (text)
    {
      if (cursor)
        *cursor = text_input_bytes_to_offset (text, input->current.cursor);
      if (anchor)
        *anchor = text_input_bytes_to_offset (text, input->current.anchor);
    }
  if (cursor_rect && input->current.has_rect)
    {
      cursor_rect->x = input->current.x;
      cursor_rect->y = input->current.y;
      cursor_rect->width = (int) input->current.width;
      cursor_rect->height = (int) input->current.height;
    }
}

void
nested_compositor_set_preedit_text (NestedCompositor *self, const char *text,
                                    int cursor)
{
  TextInput *input;
  int32_t begin = -1;
  int32_t end = -1;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  if (cursor >= 0)
    {
      begin = end = (int32_t) text_input_offset_to_bytes (text, cursor);
    }

  /* Repeating an unchanged preedit makes the client redraw for nothing, and an
   * empty preedit after an empty one is pure noise. */
  if (g_strcmp0 (input->preedit, text ? text : "") == 0)
    return;

  g_free (input->preedit);
  input->preedit = g_strdup (text ? text : "");

  zwp_text_input_v3_send_preedit_string (input->resource, input->preedit, begin, end);
  text_input_done (input);
  nested_flush (self);
}

void
nested_compositor_commit_text (NestedCompositor *self, const char *text)
{
  TextInput *input;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input || !text || !*text)
    return;

  g_debug ("nested: committing %u characters to the client",
           (guint) g_utf8_strlen (text, -1));

  zwp_text_input_v3_send_commit_string (input->resource, text);

  /* The preedit the client still holds is the text this commit replaces. */
  if (input->preedit)
    {
      g_clear_pointer (&input->preedit, g_free);
      zwp_text_input_v3_send_preedit_string (input->resource, "", -1, -1);
    }

  text_input_done (input);
  nested_flush (self);
}

void
nested_compositor_delete_surrounding_text (NestedCompositor *self, int offset,
                                           int n_chars)
{
  TextInput *input;
  const char *text;
  gint cursor_chars, start_chars, end_chars;
  gsize cursor_bytes, start_bytes, end_bytes;
  guint32 before, after;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  text = text_input_state_is_sensitive (&input->current) ? NULL : input->current.surrounding;
  if (!text)
    {
      g_debug ("nested: the input method wants text deleted, the client gave no context");
      return;
    }

  /* @offset is where the range starts relative to the cursor and @n_chars how
   * long it is; the protocol wants the two halves on either side of it. */
  cursor_chars = text_input_bytes_to_offset (text, input->current.cursor);
  start_chars = MAX (cursor_chars + MIN (offset, 0), 0);
  end_chars = MAX (cursor_chars + MAX (offset + n_chars, 0), start_chars);

  start_bytes = text_input_offset_to_bytes (text, start_chars);
  cursor_bytes = text_input_offset_to_bytes (text, cursor_chars);
  end_bytes = text_input_offset_to_bytes (text, end_chars);

  before = (guint32) (cursor_bytes - start_bytes);
  after = (guint32) (end_bytes - cursor_bytes);

  if (!before && !after)
    return;

  zwp_text_input_v3_send_delete_surrounding_text (input->resource, before, after);
  text_input_done (input);
  nested_flush (self);
}
