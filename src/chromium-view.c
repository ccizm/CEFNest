#define G_LOG_DOMAIN "chromium-view"

#include <math.h>

#include "chromium-view.h"

struct _ChromiumView {
  GtkWidget parent_instance;

  NestedCompositor *compositor;
  gulong frame_handler;
  gulong scale_handler;
  gulong text_input_handler;
  GdkSurface *scale_surface;
  int pushed_width;
  int pushed_height;

  GtkIMContext *im_context;
  GArray *im_keys;        /* evdev codes the input method claimed on press */
  gboolean im_focused;
  gulong fullscreen_notify_handler;
  GtkWidget *fullscreen_window;
};

G_DEFINE_TYPE (ChromiumView, chromium_view, GTK_TYPE_WIDGET)

static void
chromium_view_measure (GtkWidget *widget,
                       GtkOrientation orientation,
                       int for_size,
                       int *minimum,
                       int *natural,
                       int *minimum_baseline,
                       int *natural_baseline)
{
  *minimum = 320;
  *natural = 960;
}

/* Paint one surface of the stack at its offset inside the widget. Surface-local
 * pixels are the widget's logical pixels, so the texture lands at its own scale:
 * a client that renders a 1.25x buffer and scales it down with wp_viewport comes
 * out 1:1 on the real display. */
static void
snapshot_surface (GtkSnapshot *snapshot, NestedSurface *surface, int x, int y)
{
  GdkTexture *texture = nested_surface_get_texture (surface);
  graphene_rect_t src, dst, bounds;
  double scale_x, scale_y;

  if (!texture)
    return;

  nested_surface_get_paint_rect (surface, &src, &dst);
  dst.origin.x += x;
  dst.origin.y += y;
  scale_x = dst.size.width / src.size.width;
  scale_y = dst.size.height / src.size.height;

  if (scale_x == 1.0 && scale_y == 1.0 && src.origin.x == 0.0 && src.origin.y == 0.0)
    {
      gtk_snapshot_append_texture (snapshot, texture, &dst);
      return;
    }

  gtk_snapshot_push_clip (snapshot, &dst);
  graphene_rect_init (&bounds,
                      dst.origin.x - src.origin.x * scale_x,
                      dst.origin.y - src.origin.y * scale_y,
                      gdk_texture_get_width (texture) * scale_x,
                      gdk_texture_get_height (texture) * scale_y);
  gtk_snapshot_append_texture (snapshot, texture, &bounds);
  gtk_snapshot_pop (snapshot);
}

static void
chromium_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  ChromiumView *self = CHROMIUM_VIEW (widget);
  GList *stack = NULL, *l;

  if (self->compositor)
    stack = nested_compositor_get_paint_stack (self->compositor);

  if (!stack)
    {
      GdkRGBA background = { 0.07f, 0.07f, 0.09f, 1.0f };
      graphene_rect_t bounds;

      graphene_rect_init (&bounds, 0, 0,
                          gtk_widget_get_width (widget),
                          gtk_widget_get_height (widget));
      gtk_snapshot_append_color (snapshot, &background, &bounds);
      return;
    }

  /* Bottommost first, so a context menu ends up on top of the page it belongs
   * to and each layer overwrites what the rules placed under it. */
  for (l = stack; l; l = l->next)
    {
      NestedSurface *surface = l->data;
      int x = 0, y = 0;

      nested_surface_get_position (surface, &x, &y);
      snapshot_surface (snapshot, surface, x, y);
    }

  g_list_free_full (stack, g_object_unref);
}

/* BTN_* from linux/input-event-codes.h, spelled out so the kernel headers stay
 * out of the public API. */
#define NESTED_BTN_LEFT   0x110
#define NESTED_BTN_RIGHT  0x111
#define NESTED_BTN_MIDDLE 0x112

static double
chromium_view_device_scale (ChromiumView *self)
{
  GdkSurface *surface;

  surface = gtk_native_get_surface (GTK_NATIVE (gtk_widget_get_native (GTK_WIDGET (self))));

  return surface ? gdk_surface_get_scale (surface) : 1.0;
}

static void
chromium_view_push_size (ChromiumView *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  double scale = chromium_view_device_scale (self);
  int width, height;

  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  if (width <= 0 || height <= 0)
    return;

  /* Clients are configured with logical pixels and told the device scale
   * through wp_fractional_scale_v1, so a client that honours it hands back a
   * buffer that ends up 1:1 on the display. */
  nested_compositor_set_scale (self->compositor, scale);

  if (width == self->pushed_width && height == self->pushed_height)
    return;

  self->pushed_width = width;
  self->pushed_height = height;
  nested_compositor_set_size (self->compositor, width, height);
}

static void
chromium_view_on_scale_notify (GObject *gobject, GParamSpec *pspec, gpointer data)
{
  /* A window dragged onto a monitor with another scale has to reconfigure its
   * client: the device scale is what preferred_scale advertises. */
  chromium_view_push_size (CHROMIUM_VIEW (data));
}

static void
chromium_view_unwatch_scale (ChromiumView *self)
{
  if (!self->scale_handler)
    return;

  g_signal_handler_disconnect (self->scale_surface, self->scale_handler);
  self->scale_handler = 0;
  self->scale_surface = NULL;
}

static void
chromium_view_on_fullscreen_request (NestedCompositor *compositor, gboolean enter,
                                     gpointer data)
{
  ChromiumView *self = data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  g_debug ("fullscreen: widget got request enter=%d root=%p", enter, (void *) root);
  if (!GTK_IS_WINDOW (root))
    return;

  /* The page asked; the window answers. The resize comes back through
   * size_allocate and reaches the client as an ordinary configure. */
  if (enter)
    gtk_window_fullscreen (GTK_WINDOW (root));
  else
    gtk_window_unfullscreen (GTK_WINDOW (root));
}

static void
chromium_view_on_fullscreen_notify (GObject *window, GParamSpec *pspec,
                                    gpointer data)
{
  ChromiumView *self = data;

  /* Whoever moved the window — the page's request above, the user, the WM —
   * the client is told the real state, never the requested one. */
  nested_compositor_set_window_fullscreen (self->compositor,
                                           gtk_window_is_fullscreen (GTK_WINDOW (window)));
}

static void
chromium_view_watch_fullscreen (ChromiumView *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  if (!GTK_IS_WINDOW (root) || root == GTK_ROOT (self->fullscreen_window))
    return;

  if (self->fullscreen_notify_handler)
    {
      g_signal_handler_disconnect (self->fullscreen_window,
                                   self->fullscreen_notify_handler);
      self->fullscreen_notify_handler = 0;
    }

  self->fullscreen_window = GTK_WIDGET (root);
  self->fullscreen_notify_handler =
    g_signal_connect (root, "notify::fullscreened",
                      G_CALLBACK (chromium_view_on_fullscreen_notify), self);
  nested_compositor_set_window_fullscreen (self->compositor,
                                           gtk_window_is_fullscreen (GTK_WINDOW (root)));
}

static void
chromium_view_on_map (GtkWidget *widget, gpointer data)
{
  ChromiumView *self = data;
  GdkSurface *surface;

  /* While the view is on screen, the display's clipboard is the one the
   * clients copy to and paste from. */
  nested_compositor_set_clipboard (self->compositor,
                                   gdk_display_get_clipboard (gtk_widget_get_display (widget)));

  /* Client frames are paced by the same display cycle the widget paints on:
   * a client that asks for a frame is answered at the start of a cycle, so
   * what it commits lands on the next one instead of a fixed timer quantum
   * later — and a hidden window pauses the clients along with it. */
  nested_compositor_set_frame_clock (self->compositor,
                                     gtk_widget_get_frame_clock (GTK_WIDGET (widget)));
  /* Page fullscreen requests turn into window fullscreen here, and the
   * window's real state flows back to the client. */
  g_signal_connect_object (self->compositor, "window-fullscreen-request",
                           G_CALLBACK (chromium_view_on_fullscreen_request),
                           self, 0);
  chromium_view_watch_fullscreen (self);


  surface = gtk_native_get_surface (GTK_NATIVE (gtk_widget_get_native (GTK_WIDGET (self))));

  /* The page decides what the pointer looks like over it, and this window is
   * the only pointer the user can see. */
  nested_compositor_set_cursor_target (self->compositor, surface);

  if (!surface || surface == self->scale_surface)
    return;

  chromium_view_unwatch_scale (self);
  self->scale_surface = surface;
  self->scale_handler =
    g_signal_connect (surface, "notify::scale-factor",
                      G_CALLBACK (chromium_view_on_scale_notify), self);
}

static void
chromium_view_on_unmap (GtkWidget *widget, gpointer data)
{
  ChromiumView *self = CHROMIUM_VIEW (data);

  chromium_view_unwatch_scale (self);
  nested_compositor_set_clipboard (self->compositor, NULL);
  nested_compositor_set_frame_clock (self->compositor, NULL);

  if (self->fullscreen_notify_handler)
    {
      g_signal_handler_disconnect (self->fullscreen_window,
                                   self->fullscreen_notify_handler);
      self->fullscreen_notify_handler = 0;
      self->fullscreen_window = NULL;
    }
  nested_compositor_set_cursor_target (self->compositor, NULL);
}

static void
chromium_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  chromium_view_push_size (CHROMIUM_VIEW (widget));
}

static void
chromium_view_on_frame (NestedCompositor *compositor, gpointer data)
{
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

/* ------------------------------------------------------------------ *
 * Injected input                                                     *
 *                                                                    *
 * The widget owns the real devices; everything below turns GDK events *
 * into the surface-local device-pixel events the nested seat sends.  *
 * ------------------------------------------------------------------ */

static void
chromium_view_on_motion_enter (GtkEventControllerMotion *controller,
                               double x, double y, gpointer data)
{
  ChromiumView *self = data;

  /* Record the entry position first: the seat sends enter with the coordinates
   * it last knew, and an entering pointer has not produced a motion event yet. */
  nested_compositor_notify_pointer_motion (self->compositor, x, y);
  nested_compositor_set_pointer_active (self->compositor, TRUE);
}

static void
chromium_view_on_motion_leave (GtkEventControllerMotion *controller, gpointer data)
{
  ChromiumView *self = data;

  nested_compositor_set_pointer_active (self->compositor, FALSE);
}

static void
chromium_view_on_motion (GtkEventControllerMotion *controller,
                         double x, double y, gpointer data)
{
  ChromiumView *self = data;

  /* GDK hands out widget-relative logical pixels, which is exactly the space a
   * client's surface lives in now that the scaling is its own business. */
  g_debug ("input: motion logical=%.1f,%.1f", x, y);

  nested_compositor_notify_pointer_motion (self->compositor, x, y);
}

static uint32_t
nested_button_from_gdk (guint button)
{
  switch (button)
    {
    case GDK_BUTTON_PRIMARY:
      return NESTED_BTN_LEFT;
    case GDK_BUTTON_MIDDLE:
      return NESTED_BTN_MIDDLE;
    case GDK_BUTTON_SECONDARY:
      return NESTED_BTN_RIGHT;
    default:
      return NESTED_BTN_LEFT + button - 1;
    }
}

static gboolean
chromium_view_on_button_event (GtkEventController *controller, GdkEvent *event,
                               gpointer data)
{
  ChromiumView *self = data;
  GdkEventType type = gdk_event_get_event_type (event);
  gboolean pressed;
  guint button;
  uint32_t evdev;

  /* Raw press/release rather than GtkGestureClick: the click gesture waits out
   * the double-click interval before it reports the release, so any button held
   * longer than that (text selection, dragging) would lose its release. */
  if (type == GDK_BUTTON_PRESS)
    pressed = TRUE;
  else if (type == GDK_BUTTON_RELEASE)
    pressed = FALSE;
  else
    return FALSE;

  button = gdk_button_event_get_button (event);
  evdev = nested_button_from_gdk (button);

  if (pressed)
    {
      /* Clicking the page has to focus it, or the keyboard would stay outside. */
      gtk_widget_grab_focus (GTK_WIDGET (self));
      g_debug ("input: button gdk=%u -> evdev=0x%x pressed", button, evdev);
    }
  else
    g_debug ("input: button gdk=%u -> evdev=0x%x released", button, evdev);

  nested_compositor_notify_pointer_button (self->compositor, evdev, pressed);

  /* Never claim the event: the motion, scroll and key controllers on the same
   * widget have to keep seeing their own events. */
  return FALSE;
}

static void
chromium_view_on_scroll (GtkEventControllerScroll *controller, double dx, double dy,
                         gpointer data)
{
  ChromiumView *self = data;
  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  GdkScrollUnit unit = event ? gdk_scroll_event_get_unit (event) : GDK_SCROLL_UNIT_WHEEL;
  gboolean stop = event ? gdk_scroll_event_is_stop (event) : FALSE;
  uint32_t source = WL_POINTER_AXIS_SOURCE_FINGER;
  int32_t value120_x = 0;
  int32_t value120_y = 0;

  if (unit == GDK_SCROLL_UNIT_WHEEL)
    {
      /* A wheel detent is 120 in the high-resolution channel, and ten surface
       * pixels is the distance every client already assumes for one of those
       * detents. Rounding here is what a truncated step count used to throw
       * away: half a detent is 60, not nothing. */
      source = WL_POINTER_AXIS_SOURCE_WHEEL;
      value120_x = (int32_t) lround (dx * 120.0);
      value120_y = (int32_t) lround (dy * 120.0);
      dx = value120_x / 12.0;
      dy = value120_y / 12.0;
    }

  g_debug ("input: scroll unit=%d stop=%d value120=%+d,%+d px=%+.2f,%+.2f source=%u",
           unit, stop, value120_x, value120_y, dx, dy, source);

  /* The stop is GDK's own: a touchpad gesture ends with an event that carries no
   * distance and says so, and that is the moment a client may start its momentum
   * run. Claiming a stop on every event would end the fling before it began. */
  nested_compositor_notify_pointer_axis (self->compositor, dx, dy,
                                         value120_x, value120_y, source, stop);
}

/* ------------------------------------------------------------------ *
 * Text input                                                         *
 *                                                                    *
 * A nested client composes through zwp_text_input_v3, but only this   *
 * process can reach a real engine, so the widget drives its own       *
 * GtkIMContext and pipes the results into the compositor. The context *
 * is created here instead of using gtk_widget_get_im_context() so     *
 * that GTK's own input handling never filters the same key twice.     *
 * ------------------------------------------------------------------ */

static void
chromium_view_im_set_surrounding (ChromiumView *self)
{
  const char *surrounding;
  int cursor, anchor;
  GdkRectangle rect;

  nested_compositor_get_text_input_state (self->compositor, &surrounding,
                                          &cursor, &anchor, &rect);

  /* No surrounding text means either a password field or a client that has
   * not reported any; the engine gets the caret either way, just no context. */
  if (surrounding)
    gtk_im_context_set_surrounding_with_selection (self->im_context, surrounding,
                                                   strlen (surrounding),
                                                   cursor, anchor);
  else
    gtk_im_context_set_surrounding_with_selection (self->im_context, "", 0, 0, 0);

  gtk_im_context_set_cursor_location (self->im_context, &rect);
}

static void
chromium_view_im_on_text_input_state (NestedCompositor *compositor, gpointer data)
{
  ChromiumView *self = data;
  gboolean active = nested_compositor_text_input_active (compositor);

  if (active == self->im_focused)
    {
      if (active)
        chromium_view_im_set_surrounding (self);
      return;
    }

  self->im_focused = active;

  if (!active)
    {
      /* Composition is over: forget which keys were claimed, since the engine
       * has been told to drop them too. */
      g_array_set_size (self->im_keys, 0);
      gtk_im_context_focus_out (self->im_context);
      return;
    }

  /* Entering a field invalidates everything the compositor remembered, so the
   * engine starts clean and re-reads the surrounding text. */
  gtk_im_context_reset (self->im_context);
  gtk_im_context_focus_in (self->im_context);
  chromium_view_im_set_surrounding (self);
}

static void
chromium_view_im_push_preedit (ChromiumView *self)
{
  g_autofree char *preedit = NULL;
  g_autoptr (PangoAttrList) attrs = NULL;
  int cursor = 0;

  gtk_im_context_get_preedit_string (self->im_context, &preedit, &attrs, &cursor);

  if (preedit && *preedit)
    g_debug ("im: preedit \"%s\" caret=%d", preedit, cursor);

  nested_compositor_set_preedit_text (self->compositor, preedit ? preedit : "", cursor);
}

static void
chromium_view_im_on_commit (GtkIMContext *context, const char *text, gpointer data)
{
  ChromiumView *self = data;

  g_debug ("im: commit \"%s\"", text);
  nested_compositor_commit_text (self->compositor, text);
}

static void
chromium_view_im_on_preedit_start (GtkIMContext *context, gpointer data)
{
  ChromiumView *self = data;

  chromium_view_im_push_preedit (self);
}

static void
chromium_view_im_on_preedit_changed (GtkIMContext *context, gpointer data)
{
  ChromiumView *self = data;

  chromium_view_im_push_preedit (self);
}

static void
chromium_view_im_on_preedit_end (GtkIMContext *context, gpointer data)
{
  ChromiumView *self = data;

  nested_compositor_set_preedit_text (self->compositor, "", -1);
}

static gboolean
chromium_view_im_on_retrieve_surrounding (GtkIMContext *context, gpointer data)
{
  ChromiumView *self = data;

  chromium_view_im_set_surrounding (self);

  return nested_compositor_text_input_active (self->compositor);
}

static void
chromium_view_im_on_delete_surrounding (GtkIMContext *context, int offset,
                                        int n_chars, gpointer data)
{
  ChromiumView *self = data;

  g_debug ("im: delete surrounding offset=%d chars=%d", offset, n_chars);
  nested_compositor_delete_surrounding_text (self->compositor, offset, n_chars);
}

static gboolean
chromium_view_im_take_release (ChromiumView *self, guint32 evdev)
{
  guint i;

  for (i = 0; i < self->im_keys->len; i++)
    {
      if (g_array_index (self->im_keys, guint32, i) != evdev)
        continue;

      g_array_remove_index (self->im_keys, i);
      return TRUE;
    }

  return FALSE;
}

/* Returns TRUE when the key belongs to the engine and must not be sent to the
 * client as a raw key event: a field being composed for wants the text the
 * engine makes, not the keystrokes behind it. */
static gboolean
chromium_view_im_filter_key (ChromiumView *self, GtkEventControllerKey *controller,
                             guint keycode, gboolean pressed)
{
  GdkEvent *event;
  guint32 evdev = keycode - 8;
  gboolean claimed;

  if (!self->im_context || !nested_compositor_text_input_active (self->compositor))
    return FALSE;

  event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  if (!event)
    return FALSE;

  claimed = gtk_im_context_filter_keypress (self->im_context, event);

  if (pressed)
    {
      if (claimed)
        g_array_append_val (self->im_keys, evdev);

      return claimed;
    }

  /* An engine may hand back a release it claimed the press of, so the pairing
   * recorded on press is what keeps the release away from the client. */
  return claimed || chromium_view_im_take_release (self, evdev);
}

static gboolean
chromium_view_on_key_pressed (GtkEventControllerKey *controller, guint keyval,
                              guint keycode, GdkModifierType state, gpointer data)
{
  ChromiumView *self = data;

  /* GDK reports XKB key codes, which are the evdev codes Wayland wants plus 8. */
  g_debug ("input: press keyval=%u (%s) xkb=%u evdev=%u",
           keyval, gdk_keyval_name (keyval), keycode, keycode - 8);

  if (chromium_view_im_filter_key (self, controller, keycode, TRUE))
    return TRUE;

  nested_compositor_notify_keyboard_key (self->compositor, keycode - 8, TRUE);

  return TRUE;
}

static gboolean
chromium_view_on_key_released (GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state, gpointer data)
{
  ChromiumView *self = data;

  g_debug ("input: release keyval=%u (%s) xkb=%u evdev=%u",
           keyval, gdk_keyval_name (keyval), keycode, keycode - 8);

  if (chromium_view_im_filter_key (self, controller, keycode, FALSE))
    return TRUE;

  nested_compositor_notify_keyboard_key (self->compositor, keycode - 8, FALSE);

  return TRUE;
}

static void
chromium_view_on_has_focus_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
  ChromiumView *self = data;
  gboolean focused = gtk_widget_has_focus (GTK_WIDGET (self));

  g_debug ("focus: has-focus=%d", focused);
  nested_compositor_set_keyboard_active (self->compositor, focused);
}

static void
chromium_view_dispose (GObject *object)
{
  ChromiumView *self = CHROMIUM_VIEW (object);

  chromium_view_unwatch_scale (self);

  if (self->frame_handler)
    {
      g_signal_handler_disconnect (self->compositor, self->frame_handler);
      self->frame_handler = 0;
    }

  if (self->text_input_handler)
    {
      g_signal_handler_disconnect (self->compositor, self->text_input_handler);
      self->text_input_handler = 0;
    }

  if (self->im_context)
    {
      if (self->im_focused)
        gtk_im_context_focus_out (self->im_context);
      gtk_im_context_set_client_widget (self->im_context, NULL);
      g_clear_object (&self->im_context);
    }

  g_clear_pointer (&self->im_keys, g_array_unref);

  g_clear_object (&self->compositor);

  G_OBJECT_CLASS (chromium_view_parent_class)->dispose (object);
}

static void
chromium_view_init (ChromiumView *self)
{
  GtkEventController *controller;
  GError *error = NULL;

  self->compositor = nested_compositor_new (&error);
  if (!self->compositor)
    {
      g_warning ("chromium-view: %s", error ? error->message : "no compositor");
      g_clear_error (&error);
      return;
    }

  self->frame_handler = g_signal_connect (self->compositor, "frame",
                                          G_CALLBACK (chromium_view_on_frame), self);
  self->text_input_handler =
    g_signal_connect (self->compositor, "text-input-state",
                      G_CALLBACK (chromium_view_im_on_text_input_state), self);

  /* Focusable, not merely can-focus: since GTK 4.14 grab_focus() is rejected
   * unless the new property is set, and without focus the page would never see
   * a key. */
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);

  self->im_keys = g_array_new (FALSE, FALSE, sizeof (guint32));
  self->im_context = gtk_im_multicontext_new ();
  gtk_im_context_set_client_widget (self->im_context, GTK_WIDGET (self));
  gtk_im_context_set_use_preedit (self->im_context, TRUE);
  g_signal_connect (self->im_context, "commit",
                    G_CALLBACK (chromium_view_im_on_commit), self);
  g_signal_connect (self->im_context, "preedit-start",
                    G_CALLBACK (chromium_view_im_on_preedit_start), self);
  g_signal_connect (self->im_context, "preedit-changed",
                    G_CALLBACK (chromium_view_im_on_preedit_changed), self);
  g_signal_connect (self->im_context, "preedit-end",
                    G_CALLBACK (chromium_view_im_on_preedit_end), self);
  g_signal_connect (self->im_context, "retrieve-surrounding",
                    G_CALLBACK (chromium_view_im_on_retrieve_surrounding), self);
  g_signal_connect (self->im_context, "delete-surrounding",
                    G_CALLBACK (chromium_view_im_on_delete_surrounding), self);

  controller = gtk_event_controller_motion_new ();
  g_signal_connect_object (controller, "enter", G_CALLBACK (chromium_view_on_motion_enter),
                           self, 0);
  g_signal_connect_object (controller, "leave", G_CALLBACK (chromium_view_on_motion_leave),
                           self, 0);
  g_signal_connect_object (controller, "motion", G_CALLBACK (chromium_view_on_motion),
                           self, 0);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_legacy_new ();
  g_signal_connect_object (controller, "event",
                           G_CALLBACK (chromium_view_on_button_event), self, 0);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect_object (controller, "scroll", G_CALLBACK (chromium_view_on_scroll),
                           self, 0);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = gtk_event_controller_key_new ();
  g_signal_connect_object (controller, "key-pressed", G_CALLBACK (chromium_view_on_key_pressed),
                           self, 0);
  g_signal_connect_object (controller, "key-released", G_CALLBACK (chromium_view_on_key_released),
                           self, 0);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  g_signal_connect_object (self, "notify::has-focus",
                           G_CALLBACK (chromium_view_on_has_focus_notify), self, 0);
  g_signal_connect_object (self, "map", G_CALLBACK (chromium_view_on_map), self, 0);
  g_signal_connect_object (self, "unmap", G_CALLBACK (chromium_view_on_unmap), self, 0);
}

static void
chromium_view_class_init (ChromiumViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = chromium_view_dispose;
  widget_class->measure = chromium_view_measure;
  widget_class->snapshot = chromium_view_snapshot;
  widget_class->size_allocate = chromium_view_size_allocate;

  gtk_widget_class_set_css_name (widget_class, "chromiumview");
}

const char *
chromium_view_get_socket_name (ChromiumView *self)
{
  g_return_val_if_fail (CHROMIUM_IS_VIEW (self), NULL);
  return self->compositor ? nested_compositor_get_socket_name (self->compositor) : NULL;
}

NestedCompositor *
chromium_view_get_compositor (ChromiumView *self)
{
  g_return_val_if_fail (CHROMIUM_IS_VIEW (self), NULL);
  return self->compositor;
}
