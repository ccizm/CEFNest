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
 * the embedding widget does when it is unmapped. A drag is not a copy: one that
 * leaves the widget is handed to the host through ::drag-out, not to @clipboard. */
void              nested_compositor_set_clipboard   (NestedCompositor *self,
                                                     GdkClipboard *clipboard);
/* The primary selection (middle-click) bridge. Same shape as @clipboard, but
 * text-only and independent of the ordinary clipboard: a client's selection is
 * mirrored to @primary and the host's primary is offered to clients. Pass NULL
 * on unmap. */
void              nested_compositor_set_primary_clipboard (NestedCompositor *self,
                                                           GdkClipboard *primary);
/* Carrying a page's drag outside the window. While a drag is in flight its
 * payload lives in the client's wl_data_source, and this compositor is the only
 * thing that can read it: the destination it would go to is not one of ours. So
 * the host asks, and the compositor takes the best type it can pick from the
 * offered ones, calls the page's drag off (its destination was never in this
 * window), and emits ::drag-out with the mime, the bytes and every type the page
 * offered. The return value only says a drag was there to take — nothing is in
 * the host's hands until the signal arrives, and no signal at all means the
 * client answered nothing.
 *
 * The embedding widget asks on the host's behalf when its pointer leaves the
 * widget over a live drag and a host is listening to ::drag-out; a host with no
 * such handler leaves the page's drag alone, exactly as it was before this
 * existed. */
gboolean          nested_compositor_drag_take_out   (NestedCompositor *self);
/* The mime types the drag in flight offers, as a %NULL-terminated string array
 * the caller owns, or an empty array when no drag is live. A host uses this to
 * decide before ::drag-out names the one type that actually came across. */
char            **nested_compositor_drag_get_types  (NestedCompositor *self);
/* The other direction: a drag the host picked up somewhere else and is now
 * carrying over the widget. @mime_types is the %NULL-terminated list the host's
 * own drop offers and @payload the bytes it holds for the best one of them, so
 * the host has to have read it before the drag arrives — which is what
 * gdk_drop_prepare_async() is for. The compositor becomes the source, enters
 * whichever surface the pointer is over with an offer of those types, and lets
 * the page negotiate the rest exactly as it negotiates its own drags: motion
 * then goes through nested_compositor_notify_pointer_motion(), and the end comes
 * from nested_compositor_drag_from_host_drop() or ..._cancel().
 *
 * Returns FALSE when nothing came of it: a drag already has the pointer, or the
 * payload is empty. A page that accepts a type it then cannot read is the same
 * kind of failure as a copy of the wrong name, and answering the read with no
 * bytes is how that is left. */
gboolean          nested_compositor_drag_from_host    (NestedCompositor *self,
                                                       const char *const *mime_types,
                                                       GBytes *payload,
                                                       gboolean can_move);
/* The host let go over the page: the destination is told through
 * wl_data_device.drop and reads the payload from there. Whether it kept the
 * payload is not decided here — only its wl_data_offer.finish says that, and
 * that is what ::drag-in-end reports. */
void              nested_compositor_drag_from_host_drop (NestedCompositor *self);
/* The host's drag ended outside this window, or was called off: the page gets a
 * leave and no drop. */
void              nested_compositor_drag_from_host_cancel (NestedCompositor *self);
/* Paces client frame callbacks with the embedding widget's display cycle: pass
 * gtk_widget_get_frame_clock() on map and NULL on unmap, like set_clipboard().
 * While attached, a client waiting for a frame is told to draw after a cycle
 * finishes painting, so it owns the idle gap before the next cycle and what it
 * commits is on screen one cycle later — and a hidden window pauses the
 * clients along with it. Without a clock (tests, headless runs) a plain 16 ms
 * timer keeps callbacks flowing instead. */
/* Reports the platform window's real fullscreen state back to the compositor,
 * so the states array a client sees in configure matches the window the page
 * is displayed in. The widget keeps this in sync from its root window's
 * notify::fullscreened; a host may drive it instead. */
void              nested_compositor_set_window_fullscreen (NestedCompositor *self,
                                                           gboolean fullscreen);
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
/* Every mapped top-level surface, bottom-to-top in the compositor's own list
 * order. The caller owns the list and its references. */
GList            *nested_compositor_get_toplevels    (NestedCompositor *self);
/* Route pointer/keyboard input to @surface instead of the primary. A NULL
 * clears the override. This is what lets two GTK windows share one nested
 * compositor: each view names the CEF toplevel it displays when it takes focus. */
void              nested_compositor_set_input_surface (NestedCompositor *self,
                                                       NestedSurface *surface);
/* Hand the input back when the window that asked for it goes away: a view
 * unmaps when its GTK window closes or hides. Nothing happens unless @surface
 * is the one currently named, because a second window closing later must not
 * take the routing away from the window that has since taken over. Clearing it
 * is not the same as leaving a dead surface behind: the override is dropped and
 * the seat retargets in this call rather than on the next event. */
void              nested_compositor_clear_input_surface (NestedCompositor *self,
                                                         NestedSurface *surface);
/* Hit-test inside one toplevel's own paint stack. Used when input is routed to
 * a host window's surface rather than to the compositor's primary. */
NestedSurface    *nested_compositor_surface_at_for     (NestedCompositor *self,
                                                        NestedSurface *root,
                                                        double x, double y);
/* The paint stack belonging to one toplevel (itself and its popups), for a host
 * that shows one CEF window per GTK window. Pass NULL for the primary stack. */
GList            *nested_compositor_get_paint_stack_for (NestedCompositor *self,
                                                         NestedSurface *surface);

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
