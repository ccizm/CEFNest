#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include "xdg-shell-server-protocol.h"

static const struct xdg_surface_interface xdg_surface_impl;
static const struct xdg_toplevel_interface xdg_toplevel_impl;
static const struct xdg_popup_interface xdg_popup_impl;

typedef struct _OutputResource OutputResource;

struct _OutputResource {
  NestedCompositor *nested;
  struct wl_resource *resource;
  struct wl_list link;
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

void
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

void
xdg_wm_base_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &xdg_wm_base_interface, version, id);
  wl_resource_set_implementation (resource, &xdg_wm_base_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * xdg_surface                                                        *
 * ------------------------------------------------------------------ */

void
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

/* The compositor resizes every configured output together with the toplevels. */
void
output_send_state_all (NestedCompositor *self)
{
  OutputResource *output;

  wl_list_for_each (output, &self->outputs, link)
    output_send_state (output);
}
