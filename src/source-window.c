#define G_LOG_DOMAIN "chromium-session"

/* The component's View Source window.
 *
 * The helper streams a frame's HTML as view-source-begin/chunk/end events;
 * the session assembles them and this window is what the component shows by
 * default: read-only GtkSourceView text with line numbers, syntax
 * highlighting, a find bar, wrap toggle and copy-all. One window per
 * session, reused — a second request replaces the text rather than stacking
 * another window. The source lives in memory only: nothing is written to a
 * temp file, which is the whole point of not shipping CEF's ViewSource()
 * default.
 *
 * The text is the DOM as GetSource() serialized it — script changes included
 * — not the network's original bytes; chromium-session.h says so and this
 * window does not pretend otherwise.
 */

#include <string.h>

#include <gtksourceview/gtksource.h>

#include "source-window.h"

/* Everything the callbacks need. Attached to the window with
 * set_data_full, so it dies with the widget and no callback can outlive it. */
typedef struct {
  GtkWidget *window;
  GtkSourceBuffer *buffer;
  GtkSourceView *view;
  GtkLabel *heading;
  GtkSearchBar *search_bar;
  GtkSearchEntry *search_entry;
  GtkLabel *match_label;
  GtkWidget *truncated_banner;
  GtkSourceSearchContext *search;
  gboolean sized;
} SourceWindow;

static void
search_count_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  SourceWindow *state = data;
  int count = gtk_source_search_context_get_occurrences_count (state->search);

  if (count < 0)
    gtk_label_set_text (state->match_label, "统计中…");
  else if (count == 0)
    gtk_label_set_text (state->match_label, "无匹配");
  else
    {
      char *text = g_strdup_printf ("%d 处匹配", count);

      gtk_label_set_text (state->match_label, text);
      g_free (text);
    }
}

static void
search_done (GObject *source, GAsyncResult *result, gpointer data)
{
  SourceWindow *state = data;
  GtkTextIter match_start, match_end;
  gboolean wrapped = FALSE;
  GError *error = NULL;

  if (!gtk_source_search_context_forward_finish (state->search, result,
                                                 &match_start, &match_end,
                                                 &wrapped, &error))
    {
      /* Not-found is reported as a NULL match, not an error; an error here
       * means the search context changed under us, which a read-only buffer
       * only does through a text replacement — and that resets the search
       * anyway. */
      g_error_free (error);
      return;
    }

  gtk_text_buffer_select_range (GTK_TEXT_BUFFER (state->buffer),
                                &match_start, &match_end);
  gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (state->view),
                                gtk_text_buffer_get_insert
                                  (GTK_TEXT_BUFFER (state->buffer)),
                                0.0, TRUE, 0.5, 0.3);
}

static void
search_step (SourceWindow *state, gboolean backward)
{
  GtkTextIter iter;
  GtkTextMark *insert = gtk_text_buffer_get_insert
                          (GTK_TEXT_BUFFER (state->buffer));

  gtk_text_buffer_get_iter_at_mark (GTK_TEXT_BUFFER (state->buffer),
                                    &iter, insert);
  if (backward)
    gtk_source_search_context_backward_async (state->search, &iter, NULL,
                                              search_done, state);
  else
    gtk_source_search_context_forward_async (state->search, &iter, NULL,
                                             search_done, state);
}

static void
on_search_next (GtkSearchEntry *entry, gpointer data)
{
  search_step (data, FALSE);
}

static void
on_search_previous (GtkSearchEntry *entry, gpointer data)
{
  search_step (data, TRUE);
}

static void
on_search_stop (GtkSearchEntry *entry, gpointer data)
{
  SourceWindow *state = data;

  gtk_search_bar_set_search_mode (state->search_bar, FALSE);
  gtk_widget_grab_focus (GTK_WIDGET (state->view));
}

static gboolean
on_focus_search (GtkWidget *widget, GVariant *args, gpointer data)
{
  SourceWindow *state = data;
  gboolean open = !gtk_search_bar_get_search_mode (state->search_bar);

  gtk_search_bar_set_search_mode (state->search_bar, open);
  if (open)
    gtk_widget_grab_focus (GTK_WIDGET (state->search_entry));
  else
    gtk_widget_grab_focus (GTK_WIDGET (state->view));
  return TRUE;
}

static void
on_wrap_toggled (GtkToggleButton *button, gpointer data)
{
  SourceWindow *state = data;

  /* Word-char keeps a long minified line readable instead of half-empty;
   * none is for the wide-screen layout a host user may prefer. */
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (state->view),
                               gtk_toggle_button_get_active (button)
                                 ? GTK_WRAP_WORD_CHAR
                                 : GTK_WRAP_NONE);
}

static void
on_copy_all (GtkButton *button, gpointer data)
{
  SourceWindow *state = data;
  GtkTextIter start, end;
  char *text;

  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (state->buffer), &start, &end);
  text = gtk_text_buffer_get_text (GTK_TEXT_BUFFER (state->buffer),
                                   &start, &end, FALSE);
  gdk_clipboard_set_text (gtk_widget_get_clipboard (state->window), text);
  g_free (text);
}

/* html unless the URL's tail names something GtkSourceView knows better:
 * ".svg" is xml, ".js" javascript, and so on. A pathless URL guesses NULL
 * and falls back to html too. */
static GtkSourceLanguage *
guess_language (const char *url)
{
  GtkSourceLanguageManager *manager =
    gtk_source_language_manager_get_default ();
  const char *slash = url ? strrchr (url, '/') : NULL;
  const char *basename = slash ? slash + 1 : url;
  GtkSourceLanguage *language = NULL;

  if (basename && *basename && strchr (basename, '.'))
    language = gtk_source_language_manager_guess_language (manager,
                                                           basename, NULL);
  return language
    ? language
    : gtk_source_language_manager_get_language (manager, "html");
}

static void
apply_style_scheme (GtkSourceBuffer *buffer)
{
  GtkSourceStyleSchemeManager *manager =
    gtk_source_style_scheme_manager_get_default ();
  GtkSettings *settings = gtk_settings_get_default ();
  gboolean prefer_dark = FALSE;
  GtkSourceStyleScheme *scheme;

  if (settings)
    g_object_get (settings, "gtk-application-prefer-dark-theme",
                  &prefer_dark, NULL);
  /* NULL when the scheme files are not installed: the default scheme is the
   * fallback, and highlighting still works without colours chosen for it. */
  scheme = gtk_source_style_scheme_manager_get_scheme
             (manager, prefer_dark ? "Adwaita-dark" : "Adwaita");
  gtk_source_buffer_set_style_scheme (buffer, scheme);
}

GtkWidget *
cefnest_source_window_new (void)
{
  SourceWindow *state = g_new0 (SourceWindow, 1);
  GtkWidget *window, *box, *header, *label, *scrolled, *search_box;
  GtkWidget *button, *shortcut_controller;
  GtkSourceSearchSettings *settings;

  window = gtk_window_new ();
  state->window = window;
  gtk_window_set_title (GTK_WINDOW (window), "查看网页源代码");
  gtk_window_set_default_size (GTK_WINDOW (window), 860, 640);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (window), TRUE);

  header = gtk_header_bar_new ();
  label = gtk_label_new ("");
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_CENTER);
  gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), label);
  gtk_header_bar_set_show_title_buttons (GTK_HEADER_BAR (header), TRUE);
  state->heading = GTK_LABEL (label);

  button = gtk_toggle_button_new_with_label ("自动换行");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
  gtk_widget_set_tooltip_text (button, "切换自动换行");
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), button);
  g_signal_connect (button, "toggled", G_CALLBACK (on_wrap_toggled), state);

  button = gtk_button_new_with_label ("复制全部");
  gtk_widget_set_tooltip_text (button, "把整个源码复制到剪贴板");
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), button);
  g_signal_connect (button, "clicked", G_CALLBACK (on_copy_all), state);

  button = gtk_toggle_button_new_with_label ("查找");
  gtk_widget_set_tooltip_text (button, "在源码中查找（Ctrl+F）");
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), button);
  g_object_set_data (G_OBJECT (window), "cefnest-search-toggle", button);

  state->buffer = gtk_source_buffer_new (NULL);
  gtk_source_buffer_set_highlight_syntax (state->buffer, TRUE);
  /* GtkSourceView 5 has no undo stack for a plain buffer any more, which is
   * exactly what a read-only viewer wants. */
  apply_style_scheme (state->buffer);

  state->view = GTK_SOURCE_VIEW (gtk_source_view_new_with_buffer
                                   (state->buffer));
  gtk_text_view_set_editable (GTK_TEXT_VIEW (state->view), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (state->view),
                               GTK_WRAP_WORD_CHAR);
  gtk_source_view_set_show_line_numbers (state->view, TRUE);
  gtk_source_view_set_highlight_current_line (state->view, TRUE);

  settings = gtk_source_search_settings_new ();
  g_object_set (settings, "wrap-around", TRUE, NULL);
  state->search = gtk_source_search_context_new (state->buffer, settings);
  gtk_source_search_context_set_highlight (state->search, TRUE);
  g_signal_connect (state->search, "notify::occurrences-count",
                    G_CALLBACK (search_count_changed), state);

  state->search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  g_object_bind_property (state->search_entry, "text",
                          settings, "search-text",
                          G_BINDING_DEFAULT);
  g_signal_connect (state->search_entry, "next-match",
                    G_CALLBACK (on_search_next), state);
  g_signal_connect (state->search_entry, "previous-match",
                    G_CALLBACK (on_search_previous), state);
  g_signal_connect (state->search_entry, "stop-search",
                    G_CALLBACK (on_search_stop), state);

  state->match_label = GTK_LABEL (gtk_label_new (""));
  search_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (search_box), GTK_WIDGET (state->search_entry));
  gtk_box_append (GTK_BOX (search_box), GTK_WIDGET (state->match_label));
  gtk_widget_set_hexpand (GTK_WIDGET (state->search_entry), TRUE);

  state->search_bar = GTK_SEARCH_BAR (gtk_search_bar_new ());
  gtk_search_bar_set_child (state->search_bar, search_box);
  g_object_bind_property (button, "active", state->search_bar, "search-mode",
                          G_BINDING_DEFAULT);
  /* Typing anywhere in the window starts a search once the bar is open. */
  gtk_search_bar_set_key_capture_widget (state->search_bar, window);

  /* Ctrl+F opens and closes the bar from anywhere in the window. */
  shortcut_controller = GTK_WIDGET (gtk_shortcut_controller_new ());
  gtk_shortcut_controller_add_shortcut
    (GTK_SHORTCUT_CONTROLLER (shortcut_controller),
     gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_f, GDK_CONTROL_MASK),
                       gtk_callback_action_new (on_focus_search, state, NULL)));
  gtk_widget_add_controller (window, GTK_EVENT_CONTROLLER (shortcut_controller));

  state->truncated_banner = gtk_revealer_new ();
  label = gtk_label_new ("源码过大，已截断（上限 16 MiB）");
  gtk_revealer_set_child (GTK_REVEALER (state->truncated_banner), label);
  gtk_revealer_set_reveal_child (GTK_REVEALER (state->truncated_banner), FALSE);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                 GTK_WIDGET (state->view));
  gtk_widget_set_vexpand (scrolled, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), header);
  gtk_box_append (GTK_BOX (box), state->truncated_banner);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (state->search_bar));
  gtk_box_append (GTK_BOX (box), scrolled);
  gtk_window_set_child (GTK_WINDOW (window), box);

  g_object_set_data_full (G_OBJECT (window), "cefnest-source-window",
                          state, g_free);
  return window;
}

void
cefnest_source_window_show (GtkWidget *window, GtkWindow *parent,
                            const char *url, const char *title,
                            const char *text, gssize text_len,
                            gboolean truncated)
{
  SourceWindow *state = g_object_get_data (G_OBJECT (window),
                                           "cefnest-source-window");
  const char *label = title && *title ? title : (url ? url : "");
  GtkToggleButton *toggle;
  gsize text_bytes;

  g_return_if_fail (state != NULL);

  text_bytes = text_len >= 0 ? (gsize) text_len
                             : strlen (text ? text : "");

  gtk_label_set_text (state->heading, label);

  gtk_window_set_transient_for (GTK_WINDOW (window),
                                GTK_IS_WINDOW (parent) ? parent : NULL);

  if (!state->sized && parent)
    {
      int width, height;

      gtk_window_get_default_size (parent, &width, &height);
      if (width > 0 && height > 0)
        gtk_window_set_default_size (GTK_WINDOW (window),
                                     MAX (720, width * 3 / 4),
                                     MAX (480, height * 3 / 4));
      state->sized = TRUE;
    }

  gtk_source_buffer_set_language (state->buffer, guess_language (url));
  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (state->buffer),
                            text ? text : "",
                            (gint) text_bytes);
  {
    GtkTextIter start;

    gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (state->buffer), &start);
    gtk_text_buffer_place_cursor (GTK_TEXT_BUFFER (state->buffer), &start);
  }

  gtk_revealer_set_reveal_child (GTK_REVEALER (state->truncated_banner),
                                 truncated);

  /* A new text resets the search: the counts and the highlights belong to
   * the previous document. */
  gtk_editable_set_text (GTK_EDITABLE (state->search_entry), "");
  toggle = g_object_get_data (G_OBJECT (window), "cefnest-search-toggle");
  gtk_toggle_button_set_active (toggle, FALSE);

  g_debug ("source window: shown url=%s bytes=%zu truncated=%d",
           url ? url : "", text_bytes, truncated);

  gtk_window_present (GTK_WINDOW (window));
  gtk_widget_grab_focus (GTK_WIDGET (state->view));
}
