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
/* One step of the page's own focus traversal — entering the view from the
 * focus chain does this once so the page's first (backwards: last) focusable
 * element takes focus immediately. A step that runs past the edge of the
 * document is reported by the page and comes back to the host as
 * chromium_view_release_chain_focus() instead. */
void              chromium_view_advance_page_focus (ChromiumView *self,
                                                    gboolean forward);
/* Hands the keyboard focus back to the GTK chain, to the widget after
 * (backwards: before) the view. The session calls this when the page's
 * traversal ran off its edge and the walk belongs to GTK again. */
void              chromium_view_release_chain_focus (ChromiumView *self,
                                                     gboolean forward);

G_END_DECLS
