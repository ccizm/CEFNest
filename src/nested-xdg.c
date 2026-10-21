#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include "xdg-shell-server-protocol.h"

static const struct xdg_surface_interface xdg_surface_impl;
static const struct xdg_toplevel_interface xdg_toplevel_impl;
static const struct xdg_popup_interface xdg_popup_impl;

typedef struct _OutputResource OutputResource;
typedef struct _XdgPositioner XdgPositioner;

struct _OutputResource {
  NestedCompositor *nested;
  struct wl_resource *resource;
  struct wl_list link;
};

/* The rules a client writes into an xdg_positioner. At get_popup and at every
 * reposition the compositor is entitled to a copy of them, and the copy is what
 * a later re-place has to work from: the protocol says the positioner object
 * may be destroyed as soon as the request is made. */
struct _XdgPositioner {
  PositionerRules rules;
};

/* ------------------------------------------------------------------ *
 * Popup placement                                                    *
 * ------------------------------------------------------------------ */

/* Which edge or corner of the anchor rectangle the derived point is on. An
 * anchor that names no edge centres on that axis, as the request spells out. */
static void
anchor_point (uint32_t anchor, int32_t x, int32_t y, int32_t width, int32_t height,
              int32_t *out_x, int32_t *out_y)
{
  int32_t centre_x = x + width / 2;
  int32_t centre_y = y + height / 2;

  switch (anchor)
    {
    case XDG_POSITIONER_ANCHOR_TOP:
      *out_x = centre_x; *out_y = y;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM:
      *out_x = centre_x; *out_y = y + height;
      break;
    case XDG_POSITIONER_ANCHOR_LEFT:
      *out_x = x; *out_y = centre_y;
      break;
    case XDG_POSITIONER_ANCHOR_RIGHT:
      *out_x = x + width; *out_y = centre_y;
      break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
      *out_x = x; *out_y = y;
      break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
      *out_x = x + width; *out_y = y;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
      *out_x = x; *out_y = y + height;
      break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
      *out_x = x + width; *out_y = y + height;
      break;
    case XDG_POSITIONER_ANCHOR_NONE:
    default:
      *out_x = centre_x; *out_y = centre_y;
      break;
    }
}

/* Where the child goes relative to the derived anchor point: a corner gravity
 * puts the whole surface that way, an edge one centres the other axis, and
 * "none" centres both. */
static void
gravity_shift (uint32_t gravity, int32_t width, int32_t height,
               int32_t *out_x, int32_t *out_y)
{
  switch (gravity)
    {
    case XDG_POSITIONER_GRAVITY_TOP:
      *out_x = -width / 2; *out_y = -height;
      break;
    case XDG_POSITIONER_GRAVITY_BOTTOM:
      *out_x = -width / 2; *out_y = 0;
      break;
    case XDG_POSITIONER_GRAVITY_LEFT:
      *out_x = -width; *out_y = -height / 2;
      break;
    case XDG_POSITIONER_GRAVITY_RIGHT:
      *out_x = 0; *out_y = -height / 2;
      break;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:
      *out_x = -width; *out_y = -height;
      break;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
      *out_x = 0; *out_y = -height;
      break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
      *out_x = -width; *out_y = 0;
      break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
      *out_x = 0; *out_y = 0;
      break;
    case XDG_POSITIONER_GRAVITY_NONE:
    default:
      *out_x = -width / 2; *out_y = -height / 2;
      break;
    }
}

/* The same rule read the other way round, which is what a flip asks for: the
 * popup opens over the other side of the anchor point instead of under it. */
static uint32_t
mirror_anchor_x (uint32_t anchor)
{
  switch (anchor)
    {
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
    case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
    default: return anchor;
    }
}

static uint32_t
mirror_anchor_y (uint32_t anchor)
{
  switch (anchor)
    {
    case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
    case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    default: return anchor;
    }
}

static uint32_t
mirror_gravity_x (uint32_t gravity)
{
  switch (gravity)
    {
    case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    case XDG_POSITIONER_GRAVITY_LEFT: return XDG_POSITIONER_GRAVITY_RIGHT;
    case XDG_POSITIONER_GRAVITY_RIGHT: return XDG_POSITIONER_GRAVITY_LEFT;
    default: return gravity;
    }
}

static uint32_t
mirror_gravity_y (uint32_t gravity)
{
  switch (gravity)
    {
    case XDG_POSITIONER_GRAVITY_TOP: return XDG_POSITIONER_GRAVITY_BOTTOM;
    case XDG_POSITIONER_GRAVITY_BOTTOM: return XDG_POSITIONER_GRAVITY_TOP;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    default: return gravity;
    }
}

static int32_t
overflow (int32_t start, int32_t size, int32_t limit)
{
  int32_t past = start + size - limit;

  return past > 0 ? past : 0;
}

/* One round of the rules, with the anchor rectangle already moved into the
 * parent's surface space and the constraint box beside it. */
static void
place_with (const int32_t *rules_rect, uint32_t anchor, uint32_t gravity,
            int32_t width, int32_t height,
            int32_t box_x, int32_t box_y, int32_t box_width, int32_t box_height,
            uint32_t constraint, int32_t *out_x, int32_t *out_y)
{
  int32_t point_x, point_y, shift_x, shift_y;

  anchor_point (anchor, rules_rect[0] + box_x, rules_rect[1] + box_y,
                rules_rect[2], rules_rect[3], &point_x, &point_y);

  /* The offset belongs to the anchor point: the child is placed relative to it,
   * so moving it is what set_offset says it does. */
  point_x += rules_rect[4];
  point_y += rules_rect[5];

  gravity_shift (gravity, width, height, &shift_x, &shift_y);
  *out_x = point_x + shift_x;
  *out_y = point_y + shift_y;

  /* Precedence the protocol lays down: flip, then slide. Resize is left to the
   * client, which is the only one that can redraw at another size. */
  if ((constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) != 0 &&
      (*out_x < box_x || *out_x + width > box_x + box_width))
    {
      int32_t flipped_x, flipped_y;

      place_with (rules_rect, mirror_anchor_x (anchor), mirror_gravity_x (gravity),
                  width, height, box_x, box_y, box_width, box_height,
                  constraint & ~XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X,
                  &flipped_x, &flipped_y);

      /* Only take the flip when it is the better of the two: a popup wider than
       * the parent gains nothing by changing sides. */
      if (overflow (flipped_x, width, box_x + box_width) +
          (box_x - MIN (flipped_x, box_x)) <
          overflow (*out_x, width, box_x + box_width) + (box_x - MIN (*out_x, box_x)))
        {
          *out_x = flipped_x;
          *out_y = flipped_y;
        }
    }

  if ((constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) != 0 &&
      (*out_y < box_y || *out_y + height > box_y + box_height))
    {
      int32_t flipped_x, flipped_y;

      place_with (rules_rect, mirror_anchor_y (anchor), mirror_gravity_y (gravity),
                  width, height, box_x, box_y, box_width, box_height,
                  constraint & ~(XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y),
                  &flipped_x, &flipped_y);

      if (overflow (flipped_y, height, box_y + box_height) +
          (box_y - MIN (flipped_y, box_y)) <
          overflow (*out_y, height, box_y + box_height) + (box_y - MIN (*out_y, box_y)))
        {
          *out_x = flipped_x;
          *out_y = flipped_y;
        }
    }

  if ((constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) != 0)
    {
      int32_t past = overflow (*out_x, width, box_x + box_width);

      if (past > 0)
        *out_x -= past;
      if (*out_x < box_x)
        *out_x = box_x;
    }

  if ((constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) != 0)
    {
      int32_t past = overflow (*out_y, height, box_y + box_height);

      if (past > 0)
        *out_y -= past;
      if (*out_y < box_y)
        *out_y = box_y;
    }
}

/* Work out where this popup belongs and say so. The rules are relative to the
 * parent's window geometry, and the placement the widget draws is relative to
 * its surface origin, so the geometry offset is folded in here. */
void
xdg_popup_replace (NestedSurface *self)
{
  NestedSurface *parent = self->popup_parent;
  int32_t rules_rect[6];
  int32_t box_x, box_y, box_width, box_height;
  int32_t width, height;
  int32_t x = 0, y = 0;

  if (!parent)
    return;

  /* A client that has set its window geometry already is the one whose size
   * matters; before that, the size the positioner asked for is all there is. */
  width = self->geom_width > 0 ? self->geom_width : self->positioner.size_width;
  height = self->geom_height > 0 ? self->geom_height : self->positioner.size_height;
  if (width <= 0)
    width = 1;
  if (height <= 0)
    height = 1;

  box_x = parent->geom_x;
  box_y = parent->geom_y;
  box_width = parent->geom_width > 0 ? parent->geom_width : parent->width;
  box_height = parent->geom_height > 0 ? parent->geom_height : parent->height;
  if (box_width <= 0)
    box_width = 1;
  if (box_height <= 0)
    box_height = 1;

  rules_rect[0] = self->positioner.rect_x;
  rules_rect[1] = self->positioner.rect_y;
  rules_rect[2] = self->positioner.rect_width;
  rules_rect[3] = self->positioner.rect_height;
  rules_rect[4] = self->positioner.offset_x;
  rules_rect[5] = self->positioner.offset_y;

  place_with (rules_rect, self->positioner.anchor, self->positioner.gravity,
              width, height, box_x, box_y, box_width, box_height,
              self->positioner.constraint, &x, &y);

  /* The popup's own window geometry is what sits at that point, so the surface
   * itself starts a geometry offset earlier. */
  self->popup_x = x - self->geom_x;
  self->popup_y = y - self->geom_y;

  g_debug ("nested: popup %dx%d placed at %d,%d over %dx%d of its parent",
           width, height, self->popup_x, self->popup_y, box_width, box_height);
}

/* Every popup mapped over @surface, in the order they are drawn. A submenu is
 * itself a parent, so the walk goes down the tree. */
void
xdg_popups_replace (NestedSurface *surface)
{
  NestedSurface *child;

  wl_list_for_each (child, &surface->popup_children, popup_link)
    {
      xdg_popup_replace (child);
      xdg_popups_replace (child);
    }
}

/* Tell the client where its popup ended up. What configure reports is the
 * popup's window geometry, relative to its parent's window geometry — which is
 * where the client wrote the anchor rectangle, so the two geometry offsets come
 * out again here. */
static void
popup_send_configure (NestedSurface *self)
{
  NestedSurface *parent = self->popup_parent;
  int32_t width, height;

  if (!self->popup)
    return;

  width = self->geom_width > 0 ? self->geom_width : self->positioner.size_width;
  height = self->geom_height > 0 ? self->geom_height : self->positioner.size_height;

  xdg_popup_send_configure (self->popup,
                            self->popup_x + self->geom_x -
                              (parent ? parent->geom_x : 0),
                            self->popup_y + self->geom_y -
                              (parent ? parent->geom_y : 0),
                            MAX (width, 1), MAX (height, 1));
}

/* A popup with a grab is the client's own modal: a press outside it, or the
 * pointer leaving for good, is the dismissal its code is waiting for. */
void
xdg_popups_dismiss (NestedCompositor *self, NestedSurface *keep)
{
  NestedSurface *surface;

  wl_list_for_each (surface, &self->surfaces, nested_link)
    {
      NestedSurface *test;
      gboolean kept = FALSE;

      if (!surface->popup || !surface->popup_grabbed || surface == keep)
        continue;

      /* A submenu shares its dismissal with the menu above it: keeping one of
       * a chain keeps the whole chain. */
      for (test = keep; test; test = test->popup_parent)
        if (test == surface)
          kept = TRUE;

      if (kept)
        continue;

      surface->popup_grabbed = FALSE;
      xdg_popup_send_popup_done (surface->popup);
    }

  nested_flush (self);
}

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

  resource = resource_create (client, &wl_output_interface, version, id);
  if (!resource)
    return;

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
xdg_positioner_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
positioner_set_size (struct wl_client *client, struct wl_resource *resource,
                     int32_t width, int32_t height)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.size_width = width;
  positioner->rules.size_height = height;
}

static void
positioner_set_anchor_rect (struct wl_client *client, struct wl_resource *resource,
                            int32_t x, int32_t y, int32_t width, int32_t height)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.rect_x = x;
  positioner->rules.rect_y = y;
  positioner->rules.rect_width = width;
  positioner->rules.rect_height = height;
}

static void
positioner_set_anchor (struct wl_client *client, struct wl_resource *resource,
                       uint32_t anchor)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.anchor = anchor;
}

static void
positioner_set_gravity (struct wl_client *client, struct wl_resource *resource,
                        uint32_t gravity)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.gravity = gravity;
}

static void
positioner_set_constraint (struct wl_client *client, struct wl_resource *resource,
                           uint32_t adjustment)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.constraint = adjustment;
}

static void
positioner_set_offset (struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y)
{
  XdgPositioner *positioner = wl_resource_get_user_data (resource);

  positioner->rules.offset_x = x;
  positioner->rules.offset_y = y;
}

/* Reactive positioners and the parent's future size are how a client says "get
 * this right when the parent changes". Nothing here can predict that, so the
 * rules that matter are the ones the client re-sends through
 * xdg_popup.reposition. */
static void
positioner_set_noop_u (struct wl_client *client, struct wl_resource *resource,
                       uint32_t a)
{
}

static const struct xdg_positioner_interface xdg_positioner_impl = {
  .destroy = xdg_positioner_destroy,
  .set_size = positioner_set_size,
  .set_anchor_rect = positioner_set_anchor_rect,
  .set_anchor = positioner_set_anchor,
  .set_gravity = positioner_set_gravity,
  .set_constraint_adjustment = positioner_set_constraint,
  .set_offset = positioner_set_offset,
  .set_reactive = xdg_noop,
  .set_parent_size = xdg_noop_ii,
  .set_parent_configure = positioner_set_noop_u,
};

static void
positioner_resource_destroyed (struct wl_resource *resource)
{
  g_free (wl_resource_get_user_data (resource));
}

static void
xdg_wm_base_create_positioner (struct wl_client *client, struct wl_resource *resource,
                               uint32_t id)
{
  struct wl_resource *positioner;
  XdgPositioner *rules;

  positioner = resource_create (client, &xdg_positioner_interface,
                                wl_resource_get_version (resource), id);
  if (!positioner)
    return;

  rules = g_new0 (XdgPositioner, 1);
  wl_resource_set_implementation (positioner, &xdg_positioner_impl, rules,
                                  positioner_resource_destroyed);
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

  xdg_resource = resource_create (client, &xdg_surface_interface,
                                  wl_resource_get_version (resource), id);
  if (!xdg_resource)
    return;

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

  resource = resource_create (client, &xdg_wm_base_interface, version, id);
  if (!resource)
    return;

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
      if (nested->window_fullscreen)
        {
          state = wl_array_add (&states, sizeof (*state));
          if (state)
            *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
        }
      else if (self->width == nested->width && self->height == nested->height)
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
      /* Placement first: the configure is a report of where the rules put the
       * surface, not an instruction of our own. */
      xdg_popup_replace (self);
      popup_send_configure (self);
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

  if (nested)
    {
      if (self == nested->primary)
        nested_update_primary (nested);
      else if (self->popup)
        /* A popup that disappears takes its pixels and its hit area with it. */
        nested_surfaces_changed (nested);
    }

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

  toplevel = resource_create (client, &xdg_toplevel_interface,
                              wl_resource_get_version (resource), id);
  if (!toplevel)
    return;

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
  XdgPositioner *rules;
  struct wl_resource *popup;
  NestedSurface *parent;
  NestedSurface *ancestor;
  int depth;

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

  /* A positioner without both a size and an anchor rectangle cannot say where
   * anything goes, and the protocol makes that an error rather than a guess. */
  rules = wl_resource_get_user_data (positioner_resource);
  if (rules->rules.size_width <= 0 || rules->rules.size_height <= 0 ||
      rules->rules.rect_width <= 0 || rules->rules.rect_height <= 0)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                              "positioner has no size or no anchor rectangle");
      return;
    }

  /* The xdg_surface's user data is the surface it was made for, which is what
   * the placement and the draw order have to hang off. */
  parent = wl_resource_get_user_data (parent_resource);
  if (!parent || parent == self)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                              "a popup cannot be its own parent");
      return;
    }

  /* How far up this parent already hangs. Counting here, at the one place a
   * popup link is ever made, is what keeps every later walk of the tree — the
   * placement of a window's popups, the paint stack — inside a depth the host's
   * stack can carry. */
  for (depth = 0, ancestor = parent; ancestor->popup_parent; depth++)
    ancestor = ancestor->popup_parent;
  if (depth >= NESTED_POPUP_MAX_DEPTH)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                              "a popup may not hang more than %d deep",
                              NESTED_POPUP_MAX_DEPTH);
      return;
    }

  popup = resource_create (client, &xdg_popup_interface,
                           wl_resource_get_version (resource), id);
  if (!popup)
    return;

  self->popup = popup;
  self->is_popup = TRUE;
  wl_resource_set_implementation (popup, &xdg_popup_impl, g_object_ref (self),
                                  xdg_popup_resource_destroyed);

  /* The child keeps the parent alive: it is drawn relative to it, and a parent
   * that goes out of the tree first would otherwise leave it hanging. */
  self->popup_parent = g_object_ref (parent);
  wl_list_insert (parent->popup_children.prev, &self->popup_link);
  self->positioner = rules->rules;

  xdg_surface_configure (self);
}

static void
xdg_surface_set_window_geometry (struct wl_client *client, struct wl_resource *resource,
                                 int32_t x, int32_t y, int32_t width, int32_t height)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  self->geom_x = x;
  self->geom_y = y;
  self->geom_width = width;
  self->geom_height = height;

  /* The anchor rectangle is stated against this rectangle, so a change here
   * moves everything hanging off it. */
  xdg_popups_replace (self);
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
  g_debug ("xdg: set_fullscreen request received");
  nested_request_window_fullscreen (wl_resource_get_user_data (resource), TRUE);
}

static void
xdg_toplevel_unset_fullscreen (struct wl_client *client, struct wl_resource *resource)
{
  nested_request_window_fullscreen (wl_resource_get_user_data (resource), FALSE);
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
  .unset_fullscreen = xdg_toplevel_unset_fullscreen,
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
  NestedCompositor *nested = self->nested;

  self->popup = NULL;
  self->is_popup = FALSE;
  self->popup_grabbed = FALSE;
  self->mapped = FALSE;

  /* Out of the parent's draw order: the surface object can still be alive and
   * walking its own children, so a stale link would be drawn and hit-tested. */
  if (self->popup_parent)
    {
      wl_list_remove (&self->popup_link);
      wl_list_init (&self->popup_link);
      g_clear_object (&self->popup_parent);
    }

  if (nested)
    nested_surfaces_changed (nested);

  g_object_unref (self);
}

/* The grab is the client's own idea of modality: while it holds, keys belong to
 * this popup and a press elsewhere means dismissal. The protocol wants it before
 * the first commit, but a menu can be reparented and re-grabbed on a reposition,
 * so a late one is honoured instead of being treated as a protocol error — a
 * compositor that kills Chromium's context menu here loses it for the user. */
static void
xdg_popup_grab (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *seat, uint32_t serial)
{
  NestedSurface *self = wl_resource_get_user_data (resource);

  if (!seat_resource_is_seat (seat))
    {
      wl_resource_post_error (resource, XDG_POPUP_ERROR_INVALID_GRAB,
                              "the grab is not named by a seat");
      return;
    }

  self->popup_grabbed = TRUE;

  /* A popup that grabs after it is already on screen changes who the keyboard
   * belongs to right away. */
  if (self->mapped)
    nested_surfaces_changed (self->nested);
}

static void
xdg_popup_reposition (struct wl_client *client, struct wl_resource *resource,
                      struct wl_resource *positioner, uint32_t token)
{
  NestedSurface *self = wl_resource_get_user_data (resource);
  XdgPositioner *rules;

  if (!positioner ||
      !wl_resource_instance_of (positioner, &xdg_positioner_interface,
                                &xdg_positioner_impl))
    {
      post_wrong_object (resource, positioner, "xdg_positioner");
      return;
    }

  /* The new rules replace the old ones: the client is allowed to destroy the
   * positioner as soon as this request is made, so they are copied now. */
  rules = wl_resource_get_user_data (positioner);
  self->positioner = rules->rules;

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
