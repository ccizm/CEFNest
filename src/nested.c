#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include <errno.h>
#include <glib-unix.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cursor-shape-server-protocol.h"
#include "fractional-scale-server-protocol.h"
#include "linux-dmabuf-server-protocol.h"
#include "primary-selection-server-protocol.h"
#include "text-input-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

/* ------------------------------------------------------------------ *
 * Types                                                              *
 * ------------------------------------------------------------------ */

typedef struct _FrameCallback FrameCallback;
typedef struct _Viewport Viewport;
typedef struct _FractionalScale FractionalScale;

struct _FrameCallback {
  struct wl_resource *resource;
  NestedSurface *surface;
  struct wl_list link;
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

enum {
  PROP_0,
  PROP_SURFACE,
  N_PROPS,
};

enum {
  SIGNAL_FRAME,
  SIGNAL_TEXT_INPUT_STATE,
  SIGNAL_WINDOW_FULLSCREEN_REQUEST,
  SIGNAL_DRAG_OUT,
  SIGNAL_DRAG_IN_END,
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
static const struct wp_viewporter_interface viewporter_impl;
static const struct wp_viewport_interface viewport_impl;
static const struct wp_fractional_scale_manager_v1_interface fractional_scale_manager_impl;
static const struct wp_fractional_scale_v1_interface fractional_scale_impl;

/* ------------------------------------------------------------------ *
 * Small helpers                                                      *
 * ------------------------------------------------------------------ */

static void  nested_start_frame_clock (NestedCompositor *self);
static void  surface_resource_destroyed (struct wl_resource *resource);
static void  pool_resource_destroyed (struct wl_resource *resource);
static void  nested_compositor_release_shm (NestedCompositor *self,
                                            struct wl_client *client, gsize bytes);
static void  surface_reset_viewport (NestedSurface *self);
static gboolean surface_apply_viewport (NestedSurface *self,
                                        struct wl_resource *resource,
                                        int natural_width, int natural_height);
static void  surface_update_local_size (NestedSurface *self);
static gboolean surface_is_painted (NestedSurface *self);
static void  compositor_send_scale (NestedCompositor *self);

void
post_wrong_object (struct wl_resource *where, struct wl_resource *what, const char *iface)
{
  wl_resource_post_error (where, WL_DISPLAY_ERROR_INVALID_OBJECT,
                          "expected a %s, got id %u",
                          iface, what ? wl_resource_get_id (what) : 0u);
}

NestedSurface *
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
  nested_compositor_release_shm (pool->compositor, pool->client, pool->size);
  if (pool->map)
    munmap (pool->map, pool->size);
  close (pool->fd);
  g_free (pool);
}

/* ------------------------------------------------------------------ *
 * Per-connection shm budget                                          *
 *                                                                    *
 * One pool is capped; these add up. A client that opens pool after    *
 * pool otherwise holds as much of the host's memory as it likes, and  *
 * every frame of it costs a readback of the whole thing.              *
 * ------------------------------------------------------------------ */

static gsize
nested_compositor_shm_used (NestedCompositor *self, struct wl_client *client)
{
  return GPOINTER_TO_SIZE (g_hash_table_lookup (self->shm_budget, client));
}

/* The check and the write are separate because a refused request must not
 * change the tally: nothing was taken, so there is nothing to give back. */
static gboolean
nested_compositor_shm_fits (NestedCompositor *self, struct wl_client *client,
                            gsize bytes)
{
  return nested_compositor_shm_used (self, client) + bytes <= NESTED_SHM_MAX_CLIENT;
}

static void
nested_compositor_add_shm (NestedCompositor *self, struct wl_client *client,
                           gsize bytes)
{
  g_hash_table_insert (self->shm_budget, client,
                       GSIZE_TO_POINTER (nested_compositor_shm_used (self, client)
                                         + bytes));
}

static void
nested_compositor_release_shm (NestedCompositor *self, struct wl_client *client,
                               gsize bytes)
{
  gsize used = nested_compositor_shm_used (self, client);

  if (used > bytes)
    g_hash_table_insert (self->shm_budget, client, GSIZE_TO_POINTER (used - bytes));
  else
    g_hash_table_remove (self->shm_budget, client);
}

/* Every object that cites a serial — wl_pointer.enter, wl_data_device.enter, an
 * xdg_popup grab — takes it from the one counter, because a serial a client is
 * told to answer with has to be distinguishable from every other one we made. */
uint32_t
nested_next_serial (NestedCompositor *self)
{
  return ++self->serial;
}

/* wl_pointer.axis, wl_data_device.motion and the keyboard timestamps are all
 * "milliseconds since an unspecified start", and monotonic is the only clock
 * that cannot go backwards when the laptop wakes from a suspend. */
uint32_t
nested_now_msec (void)
{
  return (uint32_t) (g_get_monotonic_time () / 1000);
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
  wl_list_init (&self->popup_children);
  wl_list_init (&self->popup_link);
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

  /* Out of the parent's draw order before anything else can look at it. A
   * parent cannot be disposed while a child still refers to it, so the list it
   * removes itself from is always live. */
  if (self->popup_parent)
    {
      wl_list_remove (&self->popup_link);
      wl_list_init (&self->popup_link);
      g_clear_object (&self->popup_parent);
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
  g_clear_pointer (&self->shm_shadow, g_free);
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

void
nested_surface_get_position (NestedSurface *self, int *x, int *y)
{
  g_return_if_fail (NESTED_IS_SURFACE (self));

  if (x)
    *x = 0;
  if (y)
    *y = 0;

  /* A popup hangs off a parent, and that parent may itself be a popup over
   * another. The widget wants the position in its own space, so walk down the
   * chain adding every placement; the toplevel at the bottom of it has none.
   * A popup whose parent has gone outlives it as an object but is never drawn,
   * so the chain always ends at a toplevel. */
  while (self)
    {
      if (x)
        *x += self->popup_x;
      if (y)
        *y += self->popup_y;
      self = self->popup_parent;
    }
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
  gboolean was_painted;

  if (!self->texture && !self->mapped)
    return;

  was_primary = self->nested && self == self->nested->primary;
  was_painted = surface_is_painted (self);

  g_clear_object (&self->texture);
  g_clear_pointer (&self->shm_shadow, g_free);
  self->shm_shadow_len = 0;
  self->shm_shadow_width = 0;
  self->shm_shadow_height = 0;
  self->shm_shadow_stride = 0;
  self->shm_damage_valid = FALSE;
  self->width = 0;
  self->height = 0;
  self->natural_width = 0;
  self->natural_height = 0;
  self->mapped = FALSE;

  if (was_primary)
    nested_update_primary (self->nested);
  else if (was_painted)
    /* A popup that stops painting leaves pixels and a pointer target behind. */
    nested_surfaces_changed (self->nested);
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

  cb_resource = resource_create (client, &wl_callback_interface,
                                 wl_resource_get_version (resource), id);
  if (!cb_resource)
    return;

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

  if (pool->map && offset >= 0 && (size_t) offset + length <= pool->size)
    {
      memcpy (dst, pool->map + offset, length);
      return TRUE;
    }

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

static void
copy_pool_rect (Buffer *buffer, NestedSurface *self, GdkRectangle rect)
{
  int x = MAX (rect.x, 0);
  int y = MAX (rect.y, 0);
  int width = MIN (rect.width, buffer->width - x);
  int height = MIN (rect.height, buffer->height - y);
  uint32_t *shadow_word;
  int row;

  if (width <= 0 || height <= 0)
    return;

  for (row = 0; row < height; row++)
    {
      size_t src = (size_t) buffer->offset +
                   (size_t) (y + row) * buffer->stride + (size_t) x * 4;
      size_t dst = (size_t) (y + row) * self->shm_shadow_stride +
                   (size_t) x * 4;

      if (buffer->pool->map &&
          src + (size_t) width * 4 <= buffer->pool->size)
        memcpy (self->shm_shadow + dst, buffer->pool->map + src,
                (size_t) width * 4);
      else
        {
          /* The mmap is the normal path; pread is only for a pool that somehow
           * did not map. */
          if (!read_pool (buffer->pool, (off_t) src, (size_t) width * 4,
                          self->shm_shadow + dst))
            return;
        }

      if (buffer->format == WL_SHM_FORMAT_XRGB8888)
        {
          shadow_word = (uint32_t *) (self->shm_shadow + dst);
          for (int i = 0; i < width; i++)
            shadow_word[i] |= 0xff000000u;
        }
    }
}

static GdkTexture *
buffer_create_shm_texture (Buffer *buffer, NestedSurface *self)
{
  size_t length = (size_t) buffer->stride * buffer->height;
  GdkMemoryFormat format;
  gboolean full = FALSE;
  GBytes *bytes;

  if (buffer->format != WL_SHM_FORMAT_ARGB8888 &&
      buffer->format != WL_SHM_FORMAT_XRGB8888)
    return NULL;

  format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;

  if (!self->shm_shadow ||
      self->shm_shadow_len != length ||
      self->shm_shadow_width != buffer->width ||
      self->shm_shadow_height != buffer->height ||
      self->shm_shadow_stride != buffer->stride ||
      self->shm_shadow_format != buffer->format)
    {
      g_clear_pointer (&self->shm_shadow, g_free);
      self->shm_shadow = g_malloc0 (length);
      self->shm_shadow_len = length;
      self->shm_shadow_width = buffer->width;
      self->shm_shadow_height = buffer->height;
      self->shm_shadow_stride = buffer->stride;
      self->shm_shadow_format = buffer->format;
      full = TRUE;
    }

  if (full)
    {
      if (!read_pool (buffer->pool, buffer->offset, length, self->shm_shadow))
        return NULL;

      if (buffer->format == WL_SHM_FORMAT_XRGB8888)
        {
          uint32_t *word = (uint32_t *) self->shm_shadow;
          size_t count = length / sizeof (uint32_t);
          size_t i;

          for (i = 0; i < count; i++)
            word[i] |= 0xff000000u;
        }
    }
  else if (self->shm_damage_valid)
    copy_pool_rect (buffer, self, self->shm_damage);

  bytes = g_bytes_new (self->shm_shadow, length);
  return gdk_memory_texture_new (buffer->width, buffer->height, format, bytes,
                                 buffer->stride);
}

/* The texture for a client buffer: dmabuf buffers are imported once and cached
 * (their contents are immutable), shm buffers are read out on every commit. */
static GdkTexture *
buffer_get_texture (Buffer *buffer, NestedSurface *self)
{
  if (!buffer->is_dmabuf)
    return buffer_create_shm_texture (buffer, self);

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
surface_add_damage (NestedSurface *self, int32_t x, int32_t y,
                    int32_t width, int32_t height)
{
  GdkRectangle rect;

  if (width <= 0 || height <= 0)
    return;

  rect.x = x;
  rect.y = y;
  rect.width = width;
  rect.height = height;

  if (!self->shm_damage_valid)
    {
      self->shm_damage = rect;
      self->shm_damage_valid = TRUE;
      return;
    }

  gdk_rectangle_union (&self->shm_damage, &rect, &self->shm_damage);
}

static void
surface_damage (struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  int32_t scale = self->scale > 0 ? self->scale : 1;

  /* wl_surface.damage is in surface-local coordinates; the shm shadow is in
   * buffer pixels. */
  surface_add_damage (self, x * scale, y * scale, width * scale, height * scale);
}

static void
surface_damage_buffer (struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  surface_add_damage (self, x, y, width, height);
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
  NestedSurface *self = wl_resource_get_user_data (resource);

  /* Deprecated for every surface but a drag icon, whose place the client still
   * moves: for an icon this is a distance from the pointer hotspot, and it
   * becomes real with the commit it travels with. */
  self->pending_offset_x = x;
  self->pending_offset_y = y;
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

  /* A drag icon is placed by the pointer, so a new offset moves it now; every
   * other surface keeps the offset the protocol tells us to ignore. */
  self->offset_x = self->pending_offset_x;
  self->offset_y = self->pending_offset_y;
  if (self->is_drag_icon && nested)
    nested_compositor_drag_move_icon (nested);

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

          texture = buffer_get_texture (buffer, self);
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
          self->shm_damage_valid = FALSE;
          repaint = TRUE;
        }
      else
        surface_clear_content (self);
    }

  /* Damage is consumed by the commit that carried it, even when it produced a
   * full read (first frame / resized buffer). */
  self->shm_damage_valid = FALSE;

  surface_update_local_size (self);

  /* Cropping or rescaling changes what has to be drawn even when the buffer
   * stays the same. */
  if (!repaint && self->texture &&
      (self->width != previous_width || self->height != previous_height))
    repaint = TRUE;

  /* A popup is placed against this surface's window geometry, and a commit is
   * where this surface's size becomes real. The geometry itself is worked over
   * by xdg_surface.set_window_geometry, so only a size change has to re-place
   * the children here — otherwise a steady 60Hz parent would re-write the whole
   * popup tree once a frame. */
  if (!wl_list_empty (&self->popup_children) &&
      (self->width != previous_width || self->height != previous_height))
    xdg_popups_replace (self);

  /* A client may have named this surface in wl_pointer.set_cursor before
   * attaching to it, so this is where the cursor it asked for becomes real. */
  if (nested)
    cursor_surface_committed (nested, self);

  /* A drag icon has no xdg_surface — wl_data_device.start_drag gave it its role
   * instead — but it still reaches the screen the same way: once it has
   * pixels. */
  if (!self->mapped && (self->xdg_surface || self->is_drag_icon) && self->texture)
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
  else if (self->is_popup)
    {
      /* Painted over the toplevel, so its frames are the widget's business too
       * — but they are not the page's own frames, and the pointer may have
       * moved onto it with this commit. */
      if (surface_is_painted (self))
        {
          if (repaint)
            nested_surfaces_changed (nested);
        }
      else
        nested_update_primary (nested);
    }
  else if (self->is_drag_icon)
    {
      /* Not a window and not part of one: the icon is only drawn while a drag
       * holds it, and it never takes the pointer from the surface under it, so
       * a commit here is a new picture and nothing else. */
      if (surface_is_painted (self))
        g_signal_emit (nested, signals[SIGNAL_FRAME], 0);
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
  .damage_buffer = surface_damage_buffer,
  .offset = surface_offset,
};

static void
surface_resource_destroyed (struct wl_resource *resource)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  NestedCompositor *nested = self->nested;
  gboolean was_painted;

  self->wl_surface = NULL;
  surface_set_pending (self, NULL);
  was_painted = surface_is_painted (self);
  self->mapped = FALSE;

  seat_clear_focus (nested, self);
  text_input_forget_surface (nested, self);
  surface_list_remove (self);

  if (nested && self == nested->primary)
    nested_update_primary (nested);
  else if (nested && was_painted)
    nested_surfaces_changed (nested);

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

  surface_resource = resource_create (client, &wl_surface_interface,
                                      wl_resource_get_version (resource), id);
  if (!surface_resource)
    return;

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

  region_resource = resource_create (client, &wl_region_interface,
                                     wl_resource_get_version (resource), id);
  if (!region_resource)
    return;

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

  resource = resource_create (client, &wl_compositor_interface, version, id);
  if (!resource)
    return;

  wl_resource_set_implementation (resource, &compositor_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * wl_shm                                                             *
 * ------------------------------------------------------------------ */

static void
shm_create_pool (struct wl_client *client, struct wl_resource *resource,
                 uint32_t id, int32_t fd, int32_t size)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
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

  if (size > NESTED_SHM_MAX_POOL)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "pool of %d bytes is over the %d byte limit",
                              size, NESTED_SHM_MAX_POOL);
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

  if (!nested_compositor_shm_fits (self, client, (gsize) size))
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "this client holds %zu bytes, and %d more is over "
                              "the %zu byte pool limit for one client",
                              nested_compositor_shm_used (self, client), size,
                              NESTED_SHM_MAX_CLIENT);
      close (fd);
      return;
    }

  pool = g_new0 (Pool, 1);
  pool->fd = fd;
  pool->size = (size_t) size;
  pool->map = mmap (NULL, pool->size, PROT_READ, MAP_SHARED, fd, 0);
  if (pool->map == MAP_FAILED)
    {
      pool->map = NULL;
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "cannot map the pool: %s", g_strerror (errno));
      close (fd);
      g_free (pool);
      return;
    }
  pool->refcount = 1;
  pool->client = client;
  pool->compositor = self;
  /* Charged before the resource exists, because pool_unref() — which every
   * failure below goes through — gives the bytes back. */
  nested_compositor_add_shm (self, client, (gsize) size);

  pool_resource = resource_create (client, &wl_shm_pool_interface,
                                   wl_resource_get_version (resource), id);
  if (!pool_resource)
    {
      pool_unref (pool); /* the pool owns the fd from the moment it was made */
      return;
    }

  g_debug ("shm: client %p now holds %zu of %zu bytes of pools",
           (void *) client, nested_compositor_shm_used (self, client),
           NESTED_SHM_MAX_CLIENT);

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

  resource = resource_create (client, &wl_shm_interface, version, id);
  if (!resource)
    return;

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

  /* Sixteen bits of size each way is 128 MB of pixels, which no window has and
   * which keeps the stride and area arithmetic below free of overflow. */
  if (width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
      (int64_t) stride < (int64_t) width * 4 || offset < 0)
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

  buffer_resource = resource_create (client, &wl_buffer_interface, 1, id);
  if (!buffer_resource)
    {
      pool_unref (buffer->pool);
      g_free (buffer);
      return;
    }

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

  if (size > NESTED_SHM_MAX_POOL)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "grown to %d bytes the pool is over the %d byte limit",
                              size, NESTED_SHM_MAX_POOL);
      return;
    }

  if (fstat (pool->fd, &info) != 0 || info.st_size < size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "fd does not hold %d bytes", size);
      return;
    }

  /* Growing costs the same budget a second pool would have. */
  if (!nested_compositor_shm_fits (pool->compositor, pool->client,
                                   (gsize) size - pool->size))
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                              "this client holds %zu bytes, and growing to %d is "
                              "over the %zu byte pool limit for one client",
                              nested_compositor_shm_used (pool->compositor, pool->client),
                              size, NESTED_SHM_MAX_CLIENT);
      return;
    }

  {
    guint8 *map = mmap (NULL, (size_t) size, PROT_READ, MAP_SHARED, pool->fd, 0);

    if (map == MAP_FAILED)
      {
        wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                                "cannot grow the pool: %s", g_strerror (errno));
        return;
      }
    if (pool->map)
      munmap (pool->map, pool->size);
    pool->map = map;
  }
  nested_compositor_add_shm (pool->compositor, pool->client,
                             (gsize) size - pool->size);
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

/* The dmabuf module creates wl_buffer resources for its buffers too; the
 * implementation and its destroy handler stay here with the rest of wl_buffer. */
struct wl_resource *
buffer_resource_new (struct wl_client *client, Buffer *buffer, uint32_t id)
{
  struct wl_resource *resource = resource_create (client, &wl_buffer_interface,
                                                  1, id);

  if (!resource)
    return NULL;

  buffer->resource = resource;
  wl_resource_set_implementation (resource, &buffer_impl, buffer,
                                  buffer_resource_destroyed);
  return resource;
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

  viewport_resource = resource_create (client, &wp_viewport_interface,
                                       wl_resource_get_version (resource), id);
  if (!viewport_resource)
    return;

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

  resource = resource_create (client, &wp_viewporter_interface, version, id);
  if (!resource)
    return;

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

  scale_resource = resource_create (client, &wp_fractional_scale_v1_interface,
                                    wl_resource_get_version (resource), id);
  if (!scale_resource)
    return;

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

  resource = resource_create (client, &wp_fractional_scale_manager_v1_interface,
                              version, id);
  if (!resource)
    return;

  wl_resource_set_implementation (resource, &fractional_scale_manager_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * NestedCompositor                                                             *
 * ------------------------------------------------------------------ */

G_DEFINE_TYPE (NestedCompositor, nested_compositor, G_TYPE_OBJECT)

/* Tell every client that is waiting for a frame to draw now, and say whether
 * anyone still is. */
static gboolean
fire_pending_frame_callbacks (NestedCompositor *self)
{
  uint32_t msec = (uint32_t) (g_get_monotonic_time () / 1000);
  NestedSurface *surface;
  gboolean again = FALSE;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    surface_fire_frame_callbacks (surface, msec);

  nested_flush (self);

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface_has_pending_frames (surface))
        again = TRUE;
    }

  return again;
}

/* How long to wait for the attached clock to run a cycle before pacing by timer
 * instead. Six nominal frame intervals: slower than any display that is worth
 * aligning to, so a healthy clock is never demoted mid-run, and one missed
 * interval of it is all a display without vsync costs the page. */
#define FRAME_CLOCK_WAIT_MS 100

/* Fallback pacing for a clock that cannot be waited for: either no widget clock
 * is attached at all (tests, headless runs), or the attached one has stopped
 * running cycles. A compositor with no output device sends GDK no frame events,
 * so a requested after-paint never arrives, the frame request it was covering
 * never gets answered, and Chromium stops producing frames after the handful it
 * got from its own vsync fallback — measured on a nested mutter with no monitor,
 * where a page froze at two animation frames a second and never painted at all.
 * A plain timer is neither aligned with the display nor paused when the widget
 * is hidden, which is why the clock is preferred whenever it answers. */
static gboolean
frame_clock_tick (gpointer data)
{
  NestedCompositor *self = data;

  self->frame_timer_id = 0;

  /* Keep asking while the timer paces: a display that comes back answers with an
   * after-paint, and that is what hands the pacing over to the clock again. */
  if (self->frame_clock)
    gdk_frame_clock_request_phase (self->frame_clock,
                                   GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);

  if (fire_pending_frame_callbacks (self))
    self->frame_timer_id = g_timeout_add (16, frame_clock_tick, self);

  return G_SOURCE_REMOVE;
}

/* The widget's display cycle. A client that asked for a frame is told to draw
 * when the cycle has finished painting, so it owns the idle gap before the
 * next cycle and what it commits is processed — through the socket, in the
 * main loop — long before that cycle paints. Firing at the start of the cycle
 * instead would leave the commit sitting in the socket while the same cycle
 * paints, and every frame would land two cycles after its done instead of
 * one. While anyone still waits, keep asking for after-paint phases; with
 * nobody waiting the clock idles until a commit or a frame request speaks up
 * again. Answering at all is also what promotes the clock back over the timer. */
static void
frame_clock_after_paint (GdkFrameClock *clock, gpointer data)
{
  NestedCompositor *self = data;

  self->clock_alive = TRUE;

  if (self->frame_watchdog_id)
    {
      g_source_remove (self->frame_watchdog_id);
      self->frame_watchdog_id = 0;
    }

  if (self->frame_timer_id)
    {
      g_source_remove (self->frame_timer_id);
      self->frame_timer_id = 0;
    }

  fire_pending_frame_callbacks (self);

  /* Whoever is still waiting after that gets the next cycle and a fresh
   * watchdog; with nobody waiting the clock idles until a commit or a frame
   * request speaks up again. */
  nested_start_frame_clock (self);
}

/* The clock went quiet. Say so and let the timer take the waiting clients,
 * because waiting on a display that never paints costs the page its frames. */
static gboolean
frame_clock_watchdog (gpointer data)
{
  NestedCompositor *self = data;

  self->frame_watchdog_id = 0;
  self->clock_alive = FALSE;

  nested_start_frame_clock (self);

  return G_SOURCE_REMOVE;
}

static void
nested_start_frame_clock (NestedCompositor *self)
{
  NestedSurface *surface;
  gboolean pending = FALSE;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface_has_pending_frames (surface))
        {
          pending = TRUE;
          break;
        }
    }

  if (!pending)
    return;

  if (self->frame_clock && self->clock_alive)
    {
      gdk_frame_clock_request_phase (self->frame_clock,
                                     GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);
      if (!self->frame_watchdog_id)
        self->frame_watchdog_id = g_timeout_add (FRAME_CLOCK_WAIT_MS,
                                                 frame_clock_watchdog, self);
    }
  else if (!self->frame_timer_id)
    self->frame_timer_id = g_timeout_add (16, frame_clock_tick, self);
}

/* ------------------------------------------------------------------ *
 * The surface tree                                                   *
 *                                                                    *
 * A client's toplevel fills the widget and its popups are placed on   *
 * top of it by the rules they hand over in an xdg_positioner. Drawing *
 * and hit-testing both have to walk that tree, so it is flattened     *
 * here into a stack, bottommost first, and everything else reads the  *
 * stack instead of the list of every surface the client made.         *
 * ------------------------------------------------------------------ */

/* Whether the surface reaches the screen at all: it needs pixels, and the
 * toplevel it ultimately hangs off has to be the one being shown. */
static gboolean
surface_is_painted (NestedSurface *self)
{
  NestedSurface *root = self;

  if (!self->nested || !self->mapped || !self->texture)
    return FALSE;

  /* A drag icon belongs to the pointer rather than to a window, so it is on
   * screen exactly as long as a drag is carrying it. */
  if (self->is_drag_icon)
    return self->wl_surface && nested_compositor_drag_icon (self->nested) == self;

  while (root->popup_parent)
    root = root->popup_parent;

  return root == self->nested->primary;
}

static void
surface_stack_append (NestedSurface *self, GList **stack)
{
  NestedSurface *child;

  if (!surface_is_painted (self))
    return;

  *stack = g_list_append (*stack, g_object_ref (self));

  wl_list_for_each (child, &self->popup_children, popup_link)
    surface_stack_append (child, stack);
}

/* The topmost painted surface under a point in the widget's logical pixels.
 * Searching from the top of the stack down is what makes a click land on the
 * menu the client opened rather than on the page underneath it. */
NestedSurface *
nested_compositor_surface_at (NestedCompositor *self, double x, double y)
{
  GList *stack = nested_compositor_get_paint_stack (self);
  NestedSurface *icon = nested_compositor_drag_icon (self);
  GList *l;
  NestedSurface *found = NULL;

  for (l = g_list_last (stack); l; l = l->prev)
    {
      NestedSurface *surface = l->data;
      int left, above;

      /* "The input region is ignored for wl_surfaces with the role of a
       * drag-and-drop icon": the pointer is over whatever the icon covers. */
      if (surface == icon)
        continue;

      nested_surface_get_position (surface, &left, &above);

      if (x >= left && y >= above && x < left + surface->width &&
          y < above + surface->height)
        {
          found = surface;
          break;
        }
    }

  g_list_free_full (stack, g_object_unref);

  return found;
}

/* The surface the keys belong to. A popup that asked for a grab is a client's
 * own idea of a modal — a context menu, a <select> list — and its code waits
 * for keys on that surface, so the topmost of those wins over the page. With
 * none of them grabbed, keys go to the toplevel as they always have. */
NestedSurface *
nested_compositor_keyboard_target (NestedCompositor *self)
{
  GList *stack = nested_compositor_get_paint_stack (self);
  GList *l;
  NestedSurface *target = self->primary;

  for (l = g_list_last (stack); l; l = l->prev)
    {
      NestedSurface *surface = l->data;

      if (surface->is_popup && surface->popup_grabbed)
        {
          target = surface;
          break;
        }
    }

  g_list_free_full (stack, g_object_unref);

  return target;
}

/* Call this when the widget has to look at the surfaces differently: a popup
 * mapped, moved or stopped painting. The pointer and keyboard may belong to a
 * different surface now, and the picture has changed. */
void
nested_surfaces_changed (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  seat_retarget (self);
  g_signal_emit (self, signals[SIGNAL_FRAME], 0);
}

/* Whether anybody would use a payload taken out of a page's drag. The seat
 * calls this instead of reading one on every pointer leave: a host that never
 * connects to ::drag-out has no use for the bytes, and taking them means
 * calling the page's drag off early for nothing. */
gboolean
nested_has_drag_out_host (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);

  return g_signal_has_handler_pending (self, signals[SIGNAL_DRAG_OUT], 0, TRUE);
}

void
nested_emit_drag_out (NestedCompositor *self, const char *mime, GBytes *bytes,
                      char **types)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (mime != NULL);
  g_return_if_fail (bytes != NULL);

  g_signal_emit (self, signals[SIGNAL_DRAG_OUT], 0, mime, bytes, types);
}

void
nested_emit_drag_in_end (NestedCompositor *self, gboolean taken, const char *mime)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  g_signal_emit (self, signals[SIGNAL_DRAG_IN_END], 0, taken, mime);
}

void
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

void
nested_flush (NestedCompositor *self)
{
  if (self->display)
    wl_display_flush_clients (self->display);
}

/* The text-input module lives in its own file; the signal it reports through
 * is registered here with the rest of the class. */
void
nested_notify_text_input_state (NestedCompositor *self)
{
  g_signal_emit (self, signals[SIGNAL_TEXT_INPUT_STATE], 0);
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

  if (self->frame_watchdog_id)
    {
      g_source_remove (self->frame_watchdog_id);
      self->frame_watchdog_id = 0;
    }

  if (self->selection_replay_id)
    {
      g_source_remove (self->selection_replay_id);
      self->selection_replay_id = 0;
    }

  if (self->frame_clock)
    {
      if (self->frame_clock_handler != 0)
        g_signal_handler_disconnect (self->frame_clock, self->frame_clock_handler);
      self->frame_clock_handler = 0;
      g_clear_object (&self->frame_clock);
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

  /* After the clients, because releasing a pool writes in here. */
  g_clear_pointer (&self->shm_budget, g_hash_table_destroy);

  g_clear_object (&self->primary);
  g_clear_object (&self->pointer_focus);
  g_clear_object (&self->keyboard_focus);
  g_clear_object (&self->cursor_pending);
  g_clear_object (&self->cursor_target);
  g_clear_pointer (&self->socket_name, g_free);

  if (self->xkb_state)
    xkb_state_unref (self->xkb_state);
  if (self->xkb_keymap)
    xkb_keymap_unref (self->xkb_keymap);
  if (self->xkb_context)
    xkb_context_unref (self->xkb_context);
  nested_shutdown_dmabuf (self);
  g_clear_pointer (&self->dmabuf_formats, g_array_unref);
  if (self->dmabuf_table_fd >= 0)
    close (self->dmabuf_table_fd);
  self->dmabuf_table_fd = -1;
  data_device_shutdown (self);
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
  wl_list_init (&self->primary_devices);
  wl_list_init (&self->text_inputs);
  self->pressed_keys = g_array_new (FALSE, FALSE, sizeof (uint32_t));
  self->dmabuf_formats = g_array_new (FALSE, FALSE, sizeof (DmabufFormat));
  self->dmabuf_table_fd = -1;
  self->shm_budget = g_hash_table_new (NULL, NULL);
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

  /**
   * NestedCompositor::window-fullscreen-request:
   * @enter: %TRUE when the displayed toplevel asked to go fullscreen.
   *
   * Emitted when the page (an HTML5 requestFullscreen or a video control)
   * asked for the platform window to change fullscreen state. The widget
   * answers by fullscreening its root window; hosts that want to intercept
   * or redirect can connect instead.
   */
  signals[SIGNAL_WINDOW_FULLSCREEN_REQUEST] =
    g_signal_new ("window-fullscreen-request", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * NestedCompositor::drag-out:
   * @mime: the type the payload actually came as
   * @bytes: (transfer none): the bytes taken out of the client's drag source
   * @types: (transfer none) (element-type utf8): every type the drag offered
   *
   * Emitted when a drag that the page (or any other client) started has left
   * the widget and its payload has been read out of the client's
   * wl_data_source. The page's own drag is over by then — it was called off,
   * because the destination that would have taken this payload is not one of
   * ours — so this is the host's chance to carry it on with a drag of its own,
   * usually gdk_drag_begin() with a provider built from @bytes.
   *
   * @bytes and @types belong to the compositor and are only valid while the
   * handler runs: ref or copy what is kept. A drag whose client answered
   * nothing emits nothing at all.
   */
  signals[SIGNAL_DRAG_OUT] =
    g_signal_new ("drag-out", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_BYTES, G_TYPE_STRV);

  /**
   * NestedCompositor::drag-in-end:
   * @taken: whether the destination ended up with the payload
   * @mime: (nullable): the type it accepted, or %NULL if it never named one
   *
   * Emitted when a drag the host carried into the widget with
   * nested_compositor_drag_from_host() is over — dropped and finished, or
   * called off. @taken is TRUE only when the destination both took the drop and
   * finished the offer, which is the answer a host's own drop target has to
   * report back: gdk_drag_drop_done() and gdk_drag_report_success() are
   * otherwise left guessing what the page made of the payload. Reading the
   * payload is not that answer — a page was measured asking for a host drag's
   * bytes the moment it was entered, whether or not it then dropped them.
   */
  signals[SIGNAL_DRAG_IN_END] =
    g_signal_new ("drag-in-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_BOOLEAN, G_TYPE_STRING);
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
  self->global_primary_selection =
    wl_global_create (self->display,
                      &zwp_primary_selection_device_manager_v1_interface, 1,
                      self, primary_selection_manager_bind);
  self->global_text_input =
    wl_global_create (self->display, &zwp_text_input_manager_v3_interface,
                      NESTED_TEXT_INPUT_VERSION, self, text_input_manager_bind);
  self->global_cursor_shape =
    wl_global_create (self->display, &wp_cursor_shape_manager_v1_interface,
                      NESTED_CURSOR_SHAPE_VERSION, self, cursor_shape_manager_bind);

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
  NestedSurface *surface;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (width >= 0 && height >= 0);

  if (self->width == width && self->height == height)
    return;

  self->width = width;
  self->height = height;

  output_send_state_all (self);

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

NestedSurface *
nested_compositor_get_surface (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);
  return self->primary;
}

GList *
nested_compositor_get_paint_stack (NestedCompositor *self)
{
  GList *stack = NULL;
  NestedSurface *icon;

  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);

  if (self->primary)
    surface_stack_append (self->primary, &stack);

  /* The drag icon is not part of any window's tree, so it is not in the tree
   * walk: it rides on top of all of it, above even the menu a page opened. */
  icon = nested_compositor_drag_icon (self);
  if (icon && surface_is_painted (icon))
    stack = g_list_append (stack, g_object_ref (icon));

  return stack;
}

uint64_t
nested_compositor_get_frame_count (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), 0);
  return self->frame_count;
}

void
nested_compositor_set_window_fullscreen (NestedCompositor *self, gboolean fullscreen)
{
  NestedSurface *surface;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  fullscreen = fullscreen != FALSE;
  if (self->window_fullscreen == fullscreen)
    return;

  self->window_fullscreen = fullscreen;

  /* The states array rides on every configure; resending now tells Chromium
   * the platform window changed state without waiting for a size change that
   * may never come. */
  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      if (surface->xdg_surface)
        xdg_surface_configure (surface);
    }
}

/* A displayed toplevel asked for the window to change fullscreen state (an
 * HTML5 requestFullscreen or a video control routed through Chromium). Only
 * the surface tree that is on screen may ask — a hidden toplevel must not
 * move the user's window, and popups have no business asking at all. The
 * widget turns this into gtk_window_fullscreen() on its root and reports the
 * window's real state back through set_window_fullscreen(). */
void
nested_request_window_fullscreen (NestedSurface *self, gboolean enter)
{
  g_return_if_fail (NESTED_IS_SURFACE (self));

  if (!self->nested || !self->is_toplevel || !surface_is_painted (self))
    return;

  g_debug ("fullscreen: request enter=%d painted=%d toplevel=%d", enter,
           self->nested ? surface_is_painted (self) : -1, self->is_toplevel);
  enter = enter != FALSE;
  if (self->nested->window_fullscreen_requested == enter)
    return;

  self->nested->window_fullscreen_requested = enter;
  g_signal_emit (self->nested, signals[SIGNAL_WINDOW_FULLSCREEN_REQUEST], 0,
                 enter);
}

void
nested_compositor_set_frame_clock (NestedCompositor *self, GdkFrameClock *clock)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (clock == NULL || GDK_IS_FRAME_CLOCK (clock));

  if (self->frame_clock == clock)
    return;

  if (self->frame_clock)
    {
      if (self->frame_clock_handler != 0)
        g_signal_handler_disconnect (self->frame_clock, self->frame_clock_handler);
      self->frame_clock_handler = 0;
      g_clear_object (&self->frame_clock);
    }

  if (self->frame_timer_id)
    {
      g_source_remove (self->frame_timer_id);
      self->frame_timer_id = 0;
    }

  if (self->frame_watchdog_id)
    {
      g_source_remove (self->frame_watchdog_id);
      self->frame_watchdog_id = 0;
    }

  /* A clock that has only just been attached has not missed anything yet. */
  self->clock_alive = TRUE;

  if (!clock)
    {
      /* Clients that are mid-animation keep going on the fallback timer. */
      nested_start_frame_clock (self);
      return;
    }

  self->frame_clock = g_object_ref (clock);
  self->frame_clock_handler =
    g_signal_connect (self->frame_clock, "after-paint",
                      G_CALLBACK (frame_clock_after_paint), self);

  /* A client may already be waiting for a frame: fold it into the display
   * cycle instead of leaving it on the timer until the next request. */
  nested_start_frame_clock (self);
}

