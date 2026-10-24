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
#define NESTED_POINTER_VERSION 8
#define NESTED_KEYBOARD_VERSION 4
/* wl_keyboard.repeat_info, in the units the event asks for: characters per
 * second, and milliseconds of hold before the first repeat. */
#define NESTED_KEY_REPEAT_RATE  25
#define NESTED_KEY_REPEAT_DELAY 250

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
  /* The serial of the press whose implicit grab this client still holds, which
   * is the only serial wl_data_device.start_drag may cite. It is the button
   * going up that ends a grab, not the pointer wandering off the surface it
   * was pressed on, so this stays set across an enter or a leave. */
  uint32_t pointer_grab_serial;
  /* The axes this client has been sent a wl_pointer.axis on since the last stop
   * frame. wl_pointer.axis_stop stops a motion that was going on, so it is only
   * ever owed for an axis that had one: a touchpad lift that names an axis nobody
   * moved hands the client a scroll sequence it never started, and one that names
   * none at all swallows the frame a real gesture ends. */
  uint32_t scroll_axes;
  struct wl_list link;
};

static const struct wl_seat_interface seat_impl;
static const struct wl_pointer_interface pointer_impl;
static const struct wl_keyboard_interface keyboard_impl;

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
  sc->pointer_grab_serial = 0;
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

/* TRUE when @client is the one this compositor gave the press @serial to and
 * still holds the button down for. wl_data_device.start_drag names a surface by
 * a serial it must have grabbed with, so this is what makes a drag the user
 * asked for rather than one a connection invented. */
gboolean
seat_client_grab_is_current (NestedCompositor *self, struct wl_client *client,
                             uint32_t serial)
{
  SeatClient *sc;

  if (serial == 0)
    return FALSE;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      if (sc->pointer_grab_serial == serial && sc->pointer &&
          wl_resource_get_client (sc->pointer) == client)
        return TRUE;
    }

  return FALSE;
}

/* Pointer events carry coordinates inside the surface they are addressed to,
 * while the widget reports them inside itself. A popup is drawn at an offset
 * over its parent, so that offset comes off here — it is what makes a click
 * land on the menu row the pointer is actually over. */
static void
pointer_local (NestedCompositor *self, NestedSurface *surface, double *x, double *y)
{
  int left = 0, above = 0;

  nested_surface_get_position (surface, &left, &above);

  *x = self->pointer_x - left;
  *y = self->pointer_y - above;
}

static void
pointer_send_enter (NestedCompositor *self, struct wl_resource *pointer,
                    NestedSurface *surface)
{
  SeatClient *sc = wl_resource_get_user_data (pointer);
  double local_x, local_y;
  uint32_t serial;

  if (!seat_surface_is_mine (pointer, surface))
    return;

  pointer_local (self, surface, &local_x, &local_y);

  serial = nested_next_serial (self);
  wl_pointer_send_enter (pointer, serial, surface->wl_surface,
                         wl_fixed_from_double (local_x),
                         wl_fixed_from_double (local_y));

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
  sc->scroll_axes = 0;
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
  /* Fast path: motion runs this per event, and the full walk below builds two
   * paint stacks and flushes. Every surface-tree change, popup grab change and
   * popup reposition moves tree_generation, and the other inputs are compared
   * directly, so if all of them match the last full retarget, the pointer is
   * still inside the focused surface, and the focuses are still usable, the
   * recomputation would return exactly what is already in place. The input
   * surface is compared by pointer because rerouting it does not move the
   * tree: a different surface there must reach the full walk even though
   * every generation and wanted flag still matches. */
  if (self->retarget_tree_generation == self->tree_generation &&
      self->retarget_pointer_wanted == self->pointer_wanted &&
      self->retarget_keyboard_wanted == self->keyboard_wanted &&
      self->retarget_drag_active ==
      (nested_compositor_drag_active (self) ? 1u : 0u) &&
      self->input_surface == self->retarget_input_surface &&
      (!self->input_surface ||
       (self->input_surface->mapped && self->input_surface->wl_surface)) &&
      self->pointer_focus && self->pointer_focus->mapped &&
      self->pointer_focus->wl_surface)
    {
      int left, above;

      nested_surface_get_position (self->pointer_focus, &left, &above);
      if (self->pointer_x >= left && self->pointer_y >= above &&
          self->pointer_x < left + self->pointer_focus->width &&
          self->pointer_y < above + self->pointer_focus->height &&
          (!self->keyboard_wanted ||
           (self->keyboard_focus && self->keyboard_focus->mapped &&
            self->keyboard_focus->wl_surface)))
        return;
    }

  /* The pointer belongs to whichever painted surface is under it: a menu drawn
   * over the page takes it, and the page gets it back the moment the pointer
   * steps off the menu. A drag is the exception: while it holds the pointer the
   * surface the icon is over must not be told anything about it, so the focus
   * stays exactly where the press that started the drag left it. */
  NestedSurface *pointer_target;
  /* Keys go where the client asked for them: a popup that grabbed the seat is
   * that client's own modal, so it is focused ahead of the toplevel. */
  NestedSurface *keyboard_target = self->keyboard_wanted ?
    nested_compositor_keyboard_target (self) : NULL;
  SeatClient *sc;

  if (nested_compositor_drag_active (self))
    pointer_target = self->pointer_focus;
  else if (self->input_surface && self->input_surface->mapped &&
           self->input_surface->wl_surface)
    pointer_target = nested_compositor_surface_at_for (self, self->input_surface,
                                                       self->pointer_x,
                                                       self->pointer_y);
  else if (self->pointer_wanted)
    pointer_target = nested_compositor_surface_at (self, self->pointer_x,
                                                   self->pointer_y);
  else
    pointer_target = NULL;

  if (pointer_target && !pointer_target->wl_surface)
    pointer_target = NULL;
  if (keyboard_target && (!keyboard_target->mapped || !keyboard_target->wl_surface))
    keyboard_target = NULL;

  if (self->pointer_wanted && pointer_target != self->pointer_focus)
    {
      if (self->pointer_focus)
        {
          NestedSurface *old = self->pointer_focus;

          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->pointer)
              pointer_send_leave (self, sc->pointer, old);
          g_clear_object (&self->pointer_focus);
        }
      if (pointer_target)
        {
          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->pointer)
              pointer_send_enter (self, sc->pointer, pointer_target);
          g_set_object (&self->pointer_focus, pointer_target);
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

  if (self->keyboard_wanted && keyboard_target != self->keyboard_focus)
    {
      if (self->keyboard_focus)
        {
          NestedSurface *old = self->keyboard_focus;

          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->keyboard)
              keyboard_send_leave (self, sc->keyboard, old);
          g_clear_object (&self->keyboard_focus);
        }
      if (keyboard_target)
        {
          wl_list_for_each (sc, &self->seat_clients, link)
            if (sc->keyboard)
              keyboard_send_enter (self, sc->keyboard, keyboard_target);
          g_set_object (&self->keyboard_focus, keyboard_target);
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

  /* Record what this full walk saw, so the fast path can prove later calls
   * redundant until the tree or the inputs move again. */
  self->retarget_tree_generation = self->tree_generation;
  g_set_object (&self->retarget_input_surface, self->input_surface);
  self->retarget_pointer_wanted = self->pointer_wanted;
  self->retarget_keyboard_wanted = self->keyboard_wanted;
  self->retarget_drag_active =
    nested_compositor_drag_active (self) ? 1u : 0u;

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
          sc->scroll_axes = 0;
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

  pointer = resource_create (client, &wl_pointer_interface,
                             MIN (wl_resource_get_version (resource),
                                  NESTED_POINTER_VERSION), id);
  if (!pointer)
    return;

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

  keyboard = resource_create (client, &wl_keyboard_interface,
                              MIN (wl_resource_get_version (resource),
                                   NESTED_KEYBOARD_VERSION), id);
  if (!keyboard)
    return;

  sc->keyboard = keyboard;
  wl_resource_set_implementation (keyboard, &keyboard_impl, sc,
                                  keyboard_resource_destroyed);

  if (!keyboard_send_keymap (self, keyboard))
    {
      wl_resource_destroy (keyboard);
      sc->keyboard = NULL;
      return;
    }

  /* The protocol promises a client that repeats keys itself will have the rate
   * before the first press, so it goes out with the object rather than with the
   * enter. GDK no longer tells a widget what the keyboard's settings are, so
   * these are the conventional numbers. */
  if (wl_resource_get_version (keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
    wl_keyboard_send_repeat_info (keyboard, NESTED_KEY_REPEAT_RATE,
                                  NESTED_KEY_REPEAT_DELAY);

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

  resource = resource_create (client, &wl_seat_interface, version, id);
  if (!resource)
    return;

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

  /* A pointer that left the widget while the page's drag still held it is not
   * coming back with that drag anywhere to go: the surface it would have been
   * dropped on belongs to another window. Only worth acting on when a host can
   * carry the payload on — otherwise the page keeps its drag exactly as it did
   * before this existed, and the release that arrives later calls it off. */
  if (!active && nested_compositor_drag_active (self) &&
      nested_has_drag_out_host (self))
    nested_compositor_drag_take_out (self);

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

  /* A drag has the pointer: wl_pointer.motion would tell the page that the
   * cursor moved over it while the client's own code is waiting for the
   * wl_data_device channel instead. */
  if (nested_compositor_drag_active (self))
    {
      nested_compositor_drag_motion (self);
      nested_flush (self);
      return;
    }

  /* Which surface is under the pointer can change with the position: moving onto
   * a menu enters it and moving off the menu enters the page again. */
  seat_retarget (self);

  if (!self->pointer_focus)
    return;

  pointer_local (self, self->pointer_focus, &x, &y);

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
  double x, y;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  /* The button going up ends every implicit grab the seat handed out — including
   * the one a drag swallowed the release of, which the page never sees. Doing
   * this before the drag takes the event is what stops a client that lost its
   * button from starting another drag over a press it let go of. */
  if (!pressed)
    {
      wl_list_for_each (sc, &self->seat_clients, link)
        sc->pointer_grab_serial = 0;
    }

  /* The release that lets go of a drag is its drop, and it must not arrive at
   * the page as a button click: the destination is told through
   * wl_data_device.drop instead. The press that started the drag came through
   * here before there was a drag to hold it back. */
  if (nested_compositor_drag_button (self, button, pressed))
    {
      nested_flush (self);
      return;
    }

  /* A press can be the first event after the pointer moved, so the surface
   * under it is worked out again rather than trusted from the last motion. */
  seat_retarget (self);

  if (!self->pointer_focus)
    return;

  pointer_local (self, self->pointer_focus, &x, &y);

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      if (!sc->pointer || !sc->pointer_inside)
        continue;
      wl_pointer_send_button (sc->pointer, serial, time, button,
                              pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                      : WL_POINTER_BUTTON_STATE_RELEASED);
      if (wl_resource_get_version (sc->pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame (sc->pointer);
      /* Only the clients that are given the press hold the grab that comes with
       * it, and only for as long as this button is down. */
      if (pressed)
        sc->pointer_grab_serial = serial;
    }

  /* A press that did not land inside the grabbed popup is the dismissal the
   * client is waiting for; it is sent after the press, so the page under the
   * menu sees the click as well. */
  if (pressed && self->pointer_focus != nested_compositor_keyboard_target (self))
    {
      xdg_popups_dismiss (self, self->pointer_focus);
      /* The grab is gone, so keys belong to the page again right away: a client
       * that keeps its menu surface alive would otherwise still hold them. */
      seat_retarget (self);
    }

  nested_flush (self);
}

/* wl_pointer.axis is the channel every client understands, so it always goes
 * out. The step count has two forms: wl_pointer.axis_discrete, which can only
 * say whole detents, and wl_pointer.axis_value120 from version 8, which the
 * protocol describes as replacing it. A half-detent wheel therefore reaches an
 * old client as nothing at all and a new one as 60. */
static void
axis_send_one (struct wl_resource *pointer, uint32_t version, uint32_t time,
               uint32_t axis, double delta, int32_t value120, gboolean framed)
{
  wl_pointer_send_axis (pointer, time, axis, wl_fixed_from_double (delta));

  if (!framed || value120 == 0)
    return;

  if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
    wl_pointer_send_axis_value120 (pointer, axis, value120);
  else if (value120 / 120 != 0)
    wl_pointer_send_axis_discrete (pointer, axis, value120 / 120);
}

void
nested_compositor_notify_pointer_axis (NestedCompositor *self, double dx, double dy,
                                       int32_t value120_x, int32_t value120_y,
                                       uint32_t source, gboolean stop)
{
  SeatClient *sc;
  uint32_t time = nested_now_msec ();

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));
  g_return_if_fail (source == WL_POINTER_AXIS_SOURCE_WHEEL ||
                    source == WL_POINTER_AXIS_SOURCE_FINGER ||
                    source == WL_POINTER_AXIS_SOURCE_CONTINUOUS ||
                    source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT);

  if (!self->pointer_focus)
    return;

  wl_list_for_each (sc, &self->seat_clients, link)
    {
      gboolean framed;
      gboolean moved;
      uint32_t version;

      if (!sc->pointer || !sc->pointer_inside)
        continue;

      version = wl_resource_get_version (sc->pointer);
      framed = version >= WL_POINTER_FRAME_SINCE_VERSION;
      moved = dy != 0.0 || dx != 0.0;

      /* Both axes of one device movement belong to one frame: a client that
       * builds momentum out of a fling accumulates per frame, and two frames
       * make one diagonal gesture look like two scrolls. */
      if (framed && moved)
        wl_pointer_send_axis_source (sc->pointer, source);

      if (dy != 0.0)
        {
          sc->scroll_axes |= 1u << WL_POINTER_AXIS_VERTICAL_SCROLL;
          axis_send_one (sc->pointer, version, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                         dy, value120_y, framed);
        }
      if (dx != 0.0)
        {
          sc->scroll_axes |= 1u << WL_POINTER_AXIS_HORIZONTAL_SCROLL;
          axis_send_one (sc->pointer, version, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                         dx, value120_x, framed);
        }

      if (!framed)
        continue;

      if (moved)
        wl_pointer_send_frame (sc->pointer);

      /* A stop rides in a frame of its own, never in the frame of the axis
       * events it stops. Chromium settles the scroll sequence when it reads a
       * stop, so a stop that lands in the same frame as a delta throws that
       * delta away — a wheel click sent this way scrolls nothing at all, on any
       * page. GDK hands a stop over as an event carrying no distance, so a
       * caller that passes both gets the step and the end in the two frames a
       * real device would have used.
       *
       * Only the axes that were moved get one: wayland.xml describes
       * wl_pointer.axis_stop as stopping the motion of the axis named by the
       * events before it, so a stop for an axis that never moved is a scroll
       * sequence the client is told ended that it never saw begin — and a wheel
       * that only ever turns vertically turns that way on every frame. */
      if (stop && sc->scroll_axes != 0)
        {
          wl_pointer_send_axis_source (sc->pointer, source);

          if (sc->scroll_axes & (1u << WL_POINTER_AXIS_VERTICAL_SCROLL))
            wl_pointer_send_axis_stop (sc->pointer, time,
                                       WL_POINTER_AXIS_VERTICAL_SCROLL);
          if (sc->scroll_axes & (1u << WL_POINTER_AXIS_HORIZONTAL_SCROLL))
            wl_pointer_send_axis_stop (sc->pointer, time,
                                       WL_POINTER_AXIS_HORIZONTAL_SCROLL);

          wl_pointer_send_frame (sc->pointer);
          sc->scroll_axes = 0;
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
    {
      /* A press for a key that is already down is the host's auto-repeat. The
       * clients have to keep seeing it — a compositor that repeats is what the
       * protocol expects them to rely on — but pressed_keys is the set of keys
       * that are physically down and one release removes a single entry, so
       * appending here would leave the key listed once per repeat and every
       * later wl_keyboard.enter would report a handful of keys held at once. */
      for (i = 0; i < self->pressed_keys->len; i++)
        if (g_array_index (self->pressed_keys, uint32_t, i) == evdev_code)
          break;

      if (i == self->pressed_keys->len)
        g_array_append_val (self->pressed_keys, evdev_code);
    }
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
