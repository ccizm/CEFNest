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

G_END_DECLS
