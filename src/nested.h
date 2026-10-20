#pragma once

#include <gtk/gtk.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

G_BEGIN_DECLS

#define NESTED_TYPE_SURFACE (nested_surface_get_type ())
G_DECLARE_FINAL_TYPE (NestedSurface, nested_surface, NESTED, SURFACE, GObject)

GdkTexture     *nested_surface_get_texture    (NestedSurface *self);
void            nested_surface_get_size       (NestedSurface *self,
                                               int           *width,
                                               int           *height);
/* Where the current texture has to end up: @src is the region of the texture to
 * show, in texture pixels, and @dst the size it occupies in surface-local
 * (logical) pixels. They differ only when the client uses wp_viewport to crop
 * or to scale a high-resolution buffer down to its logical size. */
void            nested_surface_get_paint_rect (NestedSurface *self,
                                               graphene_rect_t *src,
                                               graphene_rect_t *dst);
const char     *nested_surface_get_app_id     (NestedSurface *self);
const char     *nested_surface_get_title      (NestedSurface *self);
gboolean        nested_surface_is_mapped      (NestedSurface *self);
/* Where the surface is drawn, in the embedding widget's logical pixels. A
 * toplevel fills the widget and sits at 0,0; a popup is placed by the rules it
 * handed the compositor in its xdg_positioner, so its origin is somewhere on
 * top of the surface it hangs off. */
void            nested_surface_get_position   (NestedSurface *self,
                                               int           *x,
                                               int           *y);

#define NESTED_TYPE_COMPOSITOR (nested_compositor_get_type ())
G_DECLARE_FINAL_TYPE (NestedCompositor, nested_compositor, NESTED, COMPOSITOR, GObject)

NestedCompositor *nested_compositor_new             (GError **error);
const char       *nested_compositor_get_socket_name (NestedCompositor *self);
/* The size to configure clients with, in the logical pixels of the embedding
 * widget. The device scale goes in separately: it is what
 * wp_fractional_scale_v1.preferred_scale advertises, so a client that supports
 * it renders a buffer large enough to come out 1:1 on the real display. */
void              nested_compositor_set_size        (NestedCompositor *self,
                                                     int width, int height);
void              nested_compositor_set_scale       (NestedCompositor *self,
                                                     double scale);
void              nested_compositor_get_size        (NestedCompositor *self,
                                                     int *width, int *height);
/* Bridges the nested wl_data_device_manager with a GTK clipboard in both
 * directions: a client's copy lands on @clipboard, and whatever @clipboard
 * holds is offered to clients as the selection. Pass NULL to disconnect, which
 * the embedding widget does when it is unmapped. Drag-and-drop is not part of
 * this: start_drag is ignored. */
void              nested_compositor_set_clipboard   (NestedCompositor *self,
                                                     GdkClipboard *clipboard);
/* Paces client frame callbacks with the embedding widget's display cycle: pass
 * gtk_widget_get_frame_clock() on map and NULL on unmap, like set_clipboard().
 * While attached, a client waiting for a frame is told to draw after a cycle
 * finishes painting, so it owns the idle gap before the next cycle and what it
 * commits is on screen one cycle later — and a hidden window pauses the
 * clients along with it. Without a clock (tests, headless runs) a plain 16 ms
 * timer keeps callbacks flowing instead. */
void              nested_compositor_set_frame_clock (NestedCompositor *self,
                                                     GdkFrameClock *clock);
NestedSurface    *nested_compositor_get_surface     (NestedCompositor *self);
uint64_t          nested_compositor_get_frame_count (NestedCompositor *self);
/* Everything the widget has to draw, bottommost first: the primary toplevel
 * and the popups placed over it. Each entry is a reference the caller owns, so
 * release the list with g_list_free_full (list, g_object_unref). Surfaces that
 * are not mapped, have no pixels yet, or hang off a different toplevel are left
 * out, so the stack is exactly what is on screen. */
GList            *nested_compositor_get_paint_stack (NestedCompositor *self);

/* The nested compositor has no devices of its own: the embedding widget owns
 * the real pointer and keyboard and forwards their events here. Coordinates
 * are surface-local logical pixels — the same units wl_pointer.motion uses, so
 * a GTK widget's own event coordinates go in unchanged. Buttons are BTN_*
 * codes from linux/input-event-codes.h and keys are evdev key codes, so the
 * same numbers a plain Wayland client would receive. A
 * nested_compositor_notify_keyboard_key() press for a key that is already down
 * is the host's auto-repeat: it reaches the clients as another press, which is
 * what they expect from a compositor that repeats, and it does not make the key
 * look held twice. */
void              nested_compositor_set_pointer_active  (NestedCompositor *self,
                                                         gboolean active);
void              nested_compositor_set_keyboard_active (NestedCompositor *self,
                                                         gboolean active);
void              nested_compositor_notify_pointer_motion (NestedCompositor *self,
                                                           double x, double y);
void              nested_compositor_notify_pointer_button (NestedCompositor *self,
                                                           uint32_t button,
                                                           gboolean pressed);
/* One scroll gesture of one input device, both axes at once: they belong to the
 * same wl_pointer.frame, and a client builds a fling out of frames. @dx and @dy
 * are surface pixels; @value120_x and @value120_y are the same movement in
 * 120ths of a wheel detent, zero when the device has no detents (a touchpad).
 * @source is a WL_POINTER_AXIS_SOURCE_* code, and @stop says this is the end of
 * the gesture, which is when the client may start its own momentum. It goes out
 * in a frame of its own: a stop in the frame of the axis events it stops makes
 * Chromium throw their delta away. */
void              nested_compositor_notify_pointer_axis   (NestedCompositor *self,
                                                           double dx, double dy,
                                                           int32_t value120_x,
                                                           int32_t value120_y,
                                                           uint32_t source,
                                                           gboolean stop);
void              nested_compositor_notify_keyboard_key   (NestedCompositor *self,
                                                           uint32_t evdev_code,
                                                           gboolean pressed);

/* Cursor identity. A client names the pointer it wants: wp_cursor_shape_manager
 * carries the CSS shapes a web page asks for, wl_pointer.set_cursor the image
 * an older client draws for itself. The nested compositor has no pointer to
 * put them on, so pass gtk_native_get_surface() on map and NULL on unmap, like
 * set_clipboard(): what a client asks for becomes the cursor of @surface, and
 * the widget's own pointer comes back when the page is no longer under it. */
void              nested_compositor_set_cursor_target     (NestedCompositor *self,
                                                           GdkSurface *surface);

/* Text input (zwp_text_input_v3). The nested compositor is the input-method
 * host, and the embedding widget is the only thing that can reach a real
 * engine, so the widget drives its own GtkIMContext and feeds the result back
 * here. While nested_compositor_text_input_active() is TRUE the widget must
 * give its keys to that input method and withhold them from
 * nested_compositor_notify_keyboard_key(): a client that is being composed for
 * expects text, not the raw keys behind it.
 *
 * NestedCompositor::text-input-state says when to look again. */
gboolean            nested_compositor_text_input_active   (NestedCompositor *self);

/* What the focused client reported about the field it composes into. Indices
 * are characters, not bytes, which is what a GtkIMContext wants, and
 * @surrounding is NULL for a password field and when the client says nothing.
 * The string stays owned by the compositor. */
void                nested_compositor_get_text_input_state (NestedCompositor *self,
                                                            const char **surrounding,
                                                            int *cursor, int *anchor,
                                                            GdkRectangle *cursor_rect);

/* Composition results for the client. @cursor is the caret inside @text, in
 * characters, or negative to give no hint; an empty @text ends composition. */
void                nested_compositor_set_preedit_text    (NestedCompositor *self,
                                                           const char *text,
                                                           int cursor);
void                nested_compositor_commit_text         (NestedCompositor *self,
                                                           const char *text);
/* Same arguments GtkIMContext::delete-surrounding gives: @offset is where the
 * range starts relative to the caret, in characters, and @n_chars its length. */
void                nested_compositor_delete_surrounding_text (NestedCompositor *self,
                                                               int offset,
                                                               int n_chars);

G_END_DECLS
