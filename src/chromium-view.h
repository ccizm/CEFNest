#pragma once

#include <gtk/gtk.h>

#include "nested.h"

G_BEGIN_DECLS

#define CHROMIUM_TYPE_VIEW (chromium_view_get_type ())
G_DECLARE_FINAL_TYPE (ChromiumView, chromium_view, CHROMIUM, VIEW, GtkWidget)

/* The socket name the inner Chromium process has to be started with, as the
 * value of WAYLAND_DISPLAY. */
const char       *chromium_view_get_socket_name (ChromiumView *self);
NestedCompositor *chromium_view_get_compositor  (ChromiumView *self);
/* A second view over another view's compositor. The first view keeps the
 * keyboard/clipboard/frame-clock ownership; @target selects which nested
 * toplevel this view paints (NULL means the compositor's primary). Used by the
 * demo to give each standalone CEF window its own GTK window. */
ChromiumView     *chromium_view_new_shared       (NestedCompositor *compositor,
                                                  NestedSurface *target);
void              chromium_view_set_target_surface (ChromiumView *self,
                                                    NestedSurface *target);

G_END_DECLS
