#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* src/source-window.c — the component's own View Source window. The session
 * owns one and reuses it across requests; this header is library-internal
 * and not installed. */

GtkWidget *cefnest_source_window_new  (void);
/* Re-points the window at @parent (a GtkWindow or NULL), replaces the text,
 * picks a language from @url and presents the window. */
void       cefnest_source_window_show (GtkWidget *window, GtkWindow *parent,
                                       const char *url, const char *title,
                                       const char *text, gssize text_len,
                                       gboolean truncated);

G_END_DECLS
