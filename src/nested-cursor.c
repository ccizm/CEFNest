#define G_LOG_DOMAIN "nested"

#include "nested-internal.h"

#include "cursor-shape-server-protocol.h"

/* Cursor identity. A nested client has two ways to say what the pointer should
 * look like: the enumerated shapes of wp_cursor_shape_device_v1, and the older
 * wl_pointer.set_cursor with a surface of its own. Both end as a GdkCursor on
 * the embedding widget's surface, which is the only pointer the user sees. */

typedef struct _CursorDevice CursorDevice;

/* wp_cursor_shape_device_v1.shape numbers the CSS keywords, and GTK names its
 * cursors by those same keywords, so the table is indexed by the protocol
 * value. Index 0 is empty, and the two shapes version 2 added fall past the end
 * of the table: a device handed out at version 1 may not be asked for either. */
static const char *const shape_names[] = {
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT]       = "default",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU]  = "context-menu",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP]          = "help",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER]       = "pointer",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS]      = "progress",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT]          = "wait",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL]          = "cell",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR]     = "crosshair",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT]          = "text",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT] = "vertical-text",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS]         = "alias",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY]          = "copy",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE]          = "move",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP]       = "no-drop",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED]   = "not-allowed",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB]          = "grab",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING]      = "grabbing",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE]      = "e-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE]      = "n-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE]     = "ne-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE]     = "nw-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE]      = "s-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE]     = "se-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE]     = "sw-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE]      = "w-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE]     = "ew-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE]     = "ns-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE]   = "nesw-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE]   = "nwse-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE]    = "col-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE]    = "row-resize",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL]    = "all-scroll",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN]       = "zoom-in",
  [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT]      = "zoom-out",
};

/* A cursor the theme does not have is a hole in the user's desktop, not a
 * reason to leave the pointer wherever it last was, so every name but
 * "default" gets "default" as its fallback. */
static GdkCursor *
cursor_named (const char *name)
{
  GdkCursor *fallback = NULL;
  GdkCursor *cursor;

  if (strcmp (name, "default") != 0)
    fallback = gdk_cursor_new_from_name ("default", NULL);

  cursor = gdk_cursor_new_from_name (name, fallback);
  g_clear_object (&fallback);

  return cursor;
}

/* Takes our reference on @cursor, which is how every path here leaves GDK. */
static void
cursor_apply (NestedCompositor *self, GdkCursor *cursor)
{
  const char *name;

  if (self->cursor_target)
    gdk_surface_set_cursor (self->cursor_target, cursor);

  name = cursor ? gdk_cursor_get_name (cursor) : NULL;
  g_debug ("nested: pointer becomes %s",
           !cursor ? "the widget's own" : (name ? name : "a client image"));

  g_clear_object (&cursor);
}

void
cursor_reset (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  /* The surface it was waiting on may never commit again, so this forgets it
   * along with the cursor image it was going to ask for. */
  g_clear_object (&self->cursor_pending);

  /* NULL is GDK's "the widget's own cursor", which is right when the pointer is
   * over nothing of ours: the client that set it is no longer under it. */
  if (self->cursor_target)
    gdk_surface_set_cursor (self->cursor_target, NULL);
}

/* --------------------------------------------------- the shape device */

struct _CursorDevice {
  NestedCompositor *self;
  /* NULL once the pointer this was made for is gone, which makes the device
   * inert exactly as the protocol describes it. */
  struct wl_resource *pointer;
  struct wl_listener pointer_destroy;
};

static void cursor_device_destroyed (struct wl_resource *resource);

static void
cursor_device_pointer_gone (struct wl_listener *listener, void *data)
{
  CursorDevice *device = wl_container_of (listener, device, pointer_destroy);

  /* The destroyed resource takes its listener list with it, so this is also
   * where the link comes out — an empty list is how the device records that. */
  device->pointer = NULL;
  wl_list_remove (&device->pointer_destroy.link);
  wl_list_init (&device->pointer_destroy.link);
}

static void
cursor_device_set_shape (struct wl_client *client, struct wl_resource *resource,
                         uint32_t serial, uint32_t shape)
{
  CursorDevice *device = wl_resource_get_user_data (resource);
  const char *name;

  if (!device->pointer)
    return;

  if (shape >= G_N_ELEMENTS (shape_names))
    {
      wl_resource_post_error (resource,
                              WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                              "no cursor shape %u", shape);
      return;
    }

  name = shape_names[shape];
  if (!name)
    {
      /* Either shape 0 or one of the two shapes version 2 added, and this
       * device was handed out at version 1. */
      wl_resource_post_error (resource,
                              WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                              "cursor shape %u is not one this device knows",
                              shape);
      return;
    }

  /* A serial that is not the enter this client is inside of is ignored rather
   * than an error: racing a leave is normal, and the client that is no longer
   * under the pointer has no claim on it. */
  if (!seat_pointer_serial_is_current (device->pointer, serial))
    return;

  cursor_apply (device->self, cursor_named (name));
}

static void
cursor_device_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_cursor_shape_device_v1_interface cursor_device_impl = {
  .destroy = cursor_device_destroy,
  .set_shape = cursor_device_set_shape,
};

static void
cursor_device_destroyed (struct wl_resource *resource)
{
  CursorDevice *device = wl_resource_get_user_data (resource);

  if (!wl_list_empty (&device->pointer_destroy.link))
    wl_list_remove (&device->pointer_destroy.link);
  g_free (device);
}

static void
cursor_device_new (struct wl_client *client, uint32_t version, uint32_t id,
                   NestedCompositor *self, struct wl_resource *pointer)
{
  struct wl_resource *resource;
  CursorDevice *device;

  resource = wl_resource_create (client, &wp_cursor_shape_device_v1_interface,
                                 version, id);
  device = g_new0 (CursorDevice, 1);
  device->self = self;
  device->pointer = pointer;

  wl_resource_set_implementation (resource, &cursor_device_impl, device,
                                  cursor_device_destroyed);

  if (pointer)
    {
      device->pointer_destroy.notify = cursor_device_pointer_gone;
      wl_resource_add_destroy_listener (pointer, &device->pointer_destroy);
    }
  else
    wl_list_init (&device->pointer_destroy.link);
}

/* --------------------------------------------------- the manager */

static void
cursor_manager_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
cursor_manager_get_pointer (struct wl_client *client, struct wl_resource *resource,
                            uint32_t id, struct wl_resource *pointer)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);

  if (!seat_resource_is_pointer (pointer) ||
      wl_resource_get_client (pointer) != client)
    {
      post_wrong_object (resource, pointer, "wl_pointer");
      return;
    }

  cursor_device_new (client, wl_resource_get_version (resource), id, self, pointer);
}

/* There is no tablet in a nested compositor: the widget has one pointer. The
 * id still has to become an object, or the client's own bookkeeping breaks, so
 * it becomes a device that ignores every request. */
static void
cursor_manager_get_tablet_tool_v2 (struct wl_client *client,
                                   struct wl_resource *resource, uint32_t id,
                                   struct wl_resource *tablet_tool)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);

  cursor_device_new (client, wl_resource_get_version (resource), id, self, NULL);
}

static const struct wp_cursor_shape_manager_v1_interface cursor_manager_impl = {
  .destroy = cursor_manager_destroy,
  .get_pointer = cursor_manager_get_pointer,
  .get_tablet_tool_v2 = cursor_manager_get_tablet_tool_v2,
};

void
cursor_shape_manager_bind (struct wl_client *client, void *data,
                           uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_cursor_shape_manager_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &cursor_manager_impl, data, NULL);
}

/* --------------------------------------------------- wl_pointer.set_cursor */

/* Takes our reference on the pending cursor surface and puts its current
 * buffer on the pointer. FALSE when the surface has nothing to draw with yet,
 * which is the case the protocol's "commit to take effect" refers to. */
static gboolean
cursor_apply_surface (NestedCompositor *self)
{
  GdkTexture *texture;
  GdkCursor *fallback;
  GdkCursor *cursor;
  int32_t scale;

  if (!self->cursor_pending)
    return FALSE;

  texture = nested_surface_get_texture (self->cursor_pending);
  if (!texture)
    return FALSE;

  /* The hotspot is in surface-local units but GDK measures it on the texture,
   * so a cursor drawn for a scaled surface has to be scaled up to match. */
  scale = self->cursor_pending->scale > 0 ? self->cursor_pending->scale : 1;
  fallback = cursor_named ("default");
  cursor = gdk_cursor_new_from_texture (texture,
                                        self->cursor_hotspot_x * scale,
                                        self->cursor_hotspot_y * scale,
                                        fallback);
  g_clear_object (&fallback);
  g_clear_object (&self->cursor_pending);

  cursor_apply (self, cursor);

  return TRUE;
}

void
cursor_pointer_set_cursor (NestedCompositor *self, struct wl_resource *pointer,
                           struct wl_resource *surface,
                           int32_t hotspot_x, int32_t hotspot_y)
{
  NestedSurface *cursor_surface;

  if (!surface)
    {
      /* "Hide the pointer while it is over this client's surfaces" — GDK
       * spells that with the none cursor, because passing NULL would put the
       * widget's own arrow back. */
      g_clear_object (&self->cursor_pending);
      cursor_apply (self, cursor_named ("none"));
      return;
    }

  cursor_surface = surface_from_resource (pointer, surface);
  if (!cursor_surface)
    return;

  self->cursor_hotspot_x = hotspot_x;
  self->cursor_hotspot_y = hotspot_y;
  g_set_object (&self->cursor_pending, cursor_surface);

  /* The buffer is usually already current, because a client that redraws its
   * cursor commits before asking for it. */
  if (!cursor_apply_surface (self))
    cursor_apply (self, cursor_named ("default"));
}

void
cursor_surface_committed (NestedCompositor *self, NestedSurface *surface)
{
  if (self->cursor_pending != surface)
    return;

  if (!cursor_apply_surface (self))
    {
      /* attach(NULL) then commit: the client took the cursor image away, which
       * is as good as asking for no cursor at all. */
      g_clear_object (&self->cursor_pending);
      cursor_apply (self, cursor_named ("none"));
    }
}

void
nested_compositor_set_cursor_target (NestedCompositor *self, GdkSurface *surface)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (surface == NULL || GDK_IS_SURFACE (surface));

  if (self->cursor_target == surface)
    return;

  /* Put the widget's own pointer back while the previous surface can still
   * take it: a client's cursor must not outlive the surface it was drawn on. */
  cursor_reset (self);
  g_set_object (&self->cursor_target, surface);
}
