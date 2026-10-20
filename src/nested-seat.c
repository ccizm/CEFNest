#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

/* One seat, one pointer, one keyboard: the embedding widget is the only device
 * the nested compositor knows about. */
#define NESTED_SEAT_NAME       "seat0"
#define NESTED_POINTER_VERSION 5
#define NESTED_KEYBOARD_VERSION 4

typedef struct _SeatClient SeatClient;

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
  /* The serial of the enter @pointer is inside, which is the only serial a
   * cursor request may cite. Zero when nothing is under the pointer. */
  uint32_t pointer_enter_serial;
  struct wl_list link;
};

static const struct wl_seat_interface seat_impl;
static const struct wl_pointer_interface pointer_impl;
static const struct wl_keyboard_interface keyboard_impl;

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
  SeatClient *sc = wl_resource_get_user_data (resource);

  /* The embedding widget owns the only pointer the user sees, so a client's
   * cursor surface becomes a GdkCursor on it. A serial from an enter that is
   * no longer current means the client is not under the pointer any more, and
   * its cursor is not the one to show. */
  if (!seat_pointer_serial_is_current (resource, serial))
    return;

  cursor_pointer_set_cursor (sc->nested, resource, surface, hotspot_x, hotspot_y);
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
  sc->pointer_enter_serial = 0;
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

gboolean
seat_resource_is_seat (struct wl_resource *resource)
{
  return wl_resource_instance_of (resource, &wl_seat_interface, &seat_impl);
}

gboolean
seat_resource_is_pointer (struct wl_resource *resource)
{
  return wl_resource_instance_of (resource, &wl_pointer_interface, &pointer_impl);
}

gboolean
seat_pointer_serial_is_current (struct wl_resource *pointer, uint32_t serial)
{
  SeatClient *sc = wl_resource_get_user_data (pointer);

  return sc->pointer == pointer && sc->pointer_inside &&
         sc->pointer_enter_serial == serial;
}

static void
pointer_send_enter (NestedCompositor *self, struct wl_resource *pointer,
                    NestedSurface *surface)
{
  SeatClient *sc = wl_resource_get_user_data (pointer);
  uint32_t serial;

  if (!seat_surface_is_mine (pointer, surface))
    return;

  serial = nested_next_serial (self);
  wl_pointer_send_enter (pointer, serial, surface->wl_surface,
                         wl_fixed_from_double (self->pointer_x),
                         wl_fixed_from_double (self->pointer_y));

  if (wl_resource_get_version (pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
    wl_pointer_send_frame (pointer);

  sc->pointer_inside = TRUE;
  sc->pointer_enter_serial = serial;
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
  sc->pointer_enter_serial = 0;
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

void
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

      /* Whatever cursor the surface that just left had asked for is no longer
       * the one to show; the new focus sets its own if it wants one. */
      cursor_reset (self);
    }
  else if (!self->pointer_wanted && self->pointer_focus)
    {
      NestedSurface *old = self->pointer_focus;

      wl_list_for_each (sc, &self->seat_clients, link)
        if (sc->pointer)
          pointer_send_leave (self, sc->pointer, old);
      g_clear_object (&self->pointer_focus);
      cursor_reset (self);
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

void
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
        {
          sc->pointer_inside = FALSE;
          sc->pointer_enter_serial = 0;
        }
      g_clear_object (&self->pointer_focus);
      cursor_reset (self);
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
void
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

void
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
