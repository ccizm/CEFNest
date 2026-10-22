#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>

#include "linux-dmabuf-server-protocol.h"

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

GdkTexture *
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


/* Dispose path: the gbm device owns the render-node fd. */
void
nested_shutdown_dmabuf (NestedCompositor *self)
{
  if (self->gbm)
    gbm_device_destroy (self->gbm);
  self->gbm = NULL;
}

/* ------------------------------------------------------------------ *
 * zwp_linux_dmabuf_v1                                                *
 * ------------------------------------------------------------------ */

typedef struct _Params Params;

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

/* Our four single-plane 8888 formats — everything else stays out (M2 硬约束:
 * 单平面 8888，见 .memory/pitfalls-and-constraints.md). GDK 的支持清单里
 * 只有这四个之一才会进入 feedback。 */
static gboolean
format_is_single_plane_8888 (uint32_t fourcc)
{
  static const uint32_t ours[] = {
    DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888,
  };
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (ours); i++)
    if (fourcc == ours[i])
      return TRUE;

  return FALSE;
}

void
nested_setup_dmabuf (NestedCompositor *self)
{
  /* The gbm device stays open for one reason: the feedback's main_device is
   * the dev_t of the render node, and clients (CEF) use it to pick the device
   * they allocate on. The authoritative format+modifier list, however, is
   * what this very process can import: GDK probes
   * its renderer's EGL and publishes every (format, modifier) pair it can
   * turn into a GdkTexture. Clients allocating from that list are guaranteed
   * an import, and a driver that offers tiles gets to render into tiles —
   * advertising only LINEAR here was forcing Chromium into linear buffers
   * (see the modifier=0x0 imports in every pre-change log). */
  self->gbm = gbm_open_render_node ();
  if (!self->gbm)
    {
      g_debug ("nested: no render node, so no zwp_linux_dmabuf_v1 global");
      return;
    }

  GdkDmabufFormats *formats = gdk_display_get_dmabuf_formats (gdk_display_get_default ());
  gsize n, i;

  if (!formats || (n = gdk_dmabuf_formats_get_n_formats (formats)) == 0)
    {
      g_debug ("nested: GDK advertises no dmabuf formats, "
               "so no zwp_linux_dmabuf_v1 global");
      return;
    }

  for (i = 0; i < n; i++)
    {
      guint32 fourcc;
      guint64 modifier;
      DmabufFormat entry;

      gdk_dmabuf_formats_get_format (formats, i, &fourcc, &modifier);

      if (!format_is_single_plane_8888 (fourcc))
        continue;

      entry = (DmabufFormat) { fourcc, modifier };
      g_array_append_val (self->dmabuf_formats, entry);
    }

  for (i = 0; i < self->dmabuf_formats->len; i++)
    {
      DmabufFormat *e = &g_array_index (self->dmabuf_formats, DmabufFormat, i);
      g_debug ("nested: dmabuf %c%c%c%c modifier 0x%" G_GINT64_MODIFIER "x",
               (char) (e->fourcc), (char) (e->fourcc >> 8),
               (char) (e->fourcc >> 16), (char) (e->fourcc >> 24),
               (guint64) e->modifier);
    }

  if (self->dmabuf_formats->len == 0)
    g_debug ("nested: GDK advertises no single-plane 8888 dmabuf format");
  else
    dmabuf_build_format_table (self);
}

static void
dmabuf_send_formats (struct wl_resource *resource, NestedCompositor *self)
{
  uint32_t version = wl_resource_get_version (resource);
  guint i;

  /* version 4 deprecates `format` and says a compositor must not send it;
   * from there on `modifier` is the whole answer. Versions 1 and 2 have only
   * `format`, and version 3 accepts both. */
  if (version < 4)
    {
      for (i = 0; i < self->dmabuf_formats->len; i++)
        {
          DmabufFormat *entry =
            &g_array_index (self->dmabuf_formats, DmabufFormat, i);

          zwp_linux_dmabuf_v1_send_format (resource, entry->fourcc);
        }
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

  params_resource = resource_create (client, &zwp_linux_buffer_params_v1_interface,
                                     wl_resource_get_version (resource), id);
  if (!params_resource)
    {
      g_free (params); /* no resource means no destroy handler either */
      return;
    }

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

  feedback = resource_create (client, &zwp_linux_dmabuf_feedback_v1_interface,
                              wl_resource_get_version (resource), id);
  if (!feedback)
    return;

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

void
dmabuf_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  NestedCompositor *self = data;
  struct wl_resource *resource;

  resource = resource_create (client, &zwp_linux_dmabuf_v1_interface, version, id);
  if (!resource)
    return;

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

/* A buffer that never reached a resource has no destroy handler to close the
 * planes it took over from the params, so this is where they go back. Taken over
 * is the condition: once the texture builder has the descriptors, closing them is
 * its notify's job, and a second close here would hand back numbers that are not
 * ours any more. */
static void
buffer_discard (Buffer *buffer)
{
  int i;

  if (!buffer->fds_transferred)
    {
      for (i = 0; i < NESTED_DMABUF_MAX_PLANES; i++)
        if (buffer->fd[i] >= 0)
          close (buffer->fd[i]);
    }

  g_free (buffer);
}

/* Imports the collected planes as a wl_buffer. On failure an error or the
 * 'failed' event has been sent and no buffer is left behind. */
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

  if (width <= 0 || height <= 0 || width > 65535 || height > 65535)
    {
      /* The same ceiling shm buffers get: no window is 128 MB of pixels wide,
       * and keeping the dimensions in 16 bits is what makes the plane
       * arithmetic below safe to hold in an int32. */
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
  params->used = TRUE;

  /* Import now rather than at the first commit: the client can only fall back
   * to wl_shm if it learns about a failure while creating the buffer. */
  buffer->texture = buffer_create_dmabuf_texture (buffer);
  if (!buffer->texture)
    {
      buffer_discard (buffer);
      zwp_linux_buffer_params_v1_send_failed (resource);
      return;
    }

  /* An id of 0 makes libwayland pick one when the 'created' event binds it. */
  buffer_resource = buffer_resource_new (client, buffer, immediate ? buffer_id : 0);
  if (!buffer_resource)
    {
      g_clear_object (&buffer->texture);
      buffer_discard (buffer);
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
