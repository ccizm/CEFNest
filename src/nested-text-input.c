#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include <string.h>

#include "text-input-server-protocol.h"

/* ------------------------------------------------------------------ *
 * zwp_text_input_manager_v3                                          *
 *                                                                    *
 * The nested compositor is the input-method host: a client describes  *
 * its caret, the text around it and what the field is for, and the     *
 * embedding widget's own GtkIMContext composes into preedit and        *
 * commit events. State is double-buffered as the protocol describes    *
 * it: the requests fill the pending copy, commit() makes it current,   *
 * and done() carries the commit count as the serial the client has to  *
 * see before it applies what we sent.                                  *
 * ------------------------------------------------------------------ */

#define NESTED_TEXT_INPUT_VERSION 1

/* Indices go over the wire in bytes; the widget side works in characters. */
struct _TextInputState {
  gboolean enabled;
  char *surrounding;                /* UTF-8, as the client reported it */
  gint32 cursor, anchor;            /* bytes into surrounding */
  guint32 change_cause;
  guint32 hint, purpose;            /* zwp_text_input_v3_content_* */
  gint32 x, y;
  gint32 width, height;
  gboolean has_rect;
};

typedef struct _TextInputState TextInputState;

struct _TextInput {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  NestedSurface *surface;           /* set between enter and leave */
  gboolean entered;
  TextInputState pending, current;
  char *preedit;                    /* the last preedit_string we sent */
  guint32 commits;                  /* the serial done() events use */
  struct wl_list link;
};

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl;
static const struct zwp_text_input_v3_interface text_input_impl;

static void
text_input_state_clear (TextInputState *state)
{
  g_clear_pointer (&state->surrounding, g_free);
  memset (state, 0, sizeof (*state));
}

static void
text_input_state_apply (TextInputState *to, const TextInputState *from)
{
  text_input_state_clear (to);
  *to = *from;
  to->surrounding = from->surrounding ? g_strdup (from->surrounding) : NULL;
}

/* A password field must not have its contents handed to an input method, which
 * is often a separate process with its own logging. The client's hint is the
 * only thing that says so, so it is honoured even where it costs features. */
static gboolean
text_input_state_is_sensitive (const TextInputState *state)
{
  return (state->hint & (ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT |
                         ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA)) != 0 ||
         state->purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
}

static void
text_input_done (TextInput *input)
{
  zwp_text_input_v3_send_done (input->resource, input->commits);
}

static void
text_input_update_active (NestedCompositor *self)
{
  TextInput *input;
  TextInput *found = NULL;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->entered && input->current.enabled)
        {
          found = input;
          break;
        }
    }

  self->active_input = found;

  /* The widget reads the new state through the getters when it is told about
   * it, so one signal covers "composition possible", "not any more" and
   * "the caret or the text around it moved". */
  nested_notify_text_input_state (self);
}

static void
text_input_enter (NestedCompositor *self, TextInput *input, NestedSurface *surface)
{
  input->entered = TRUE;
  g_set_object (&input->surface, surface);

  /* "After an enter event ... all state information is invalidated": the
   * client has to send it again, so throw the old copy away now. */
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);

  g_debug ("nested: text input entered on the keyboard-focused surface");

  zwp_text_input_v3_send_enter (input->resource, surface->wl_surface);
}

static void
text_input_leave (NestedCompositor *self, TextInput *input)
{
  if (!input->entered)
    return;

  zwp_text_input_v3_send_leave (input->resource,
                                input->surface ? input->surface->wl_surface : NULL);

  input->entered = FALSE;
  g_clear_object (&input->surface);
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);
}

/* Focus moved to @surface (or to nothing): every text input of the client that
 * owns the surface enters, the rest leave. */
void
text_input_retarget (NestedCompositor *self, NestedSurface *surface)
{
  TextInput *input;

  if (!self)
    return;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->entered && input->surface != surface)
        text_input_leave (self, input);

      if (!input->entered && surface &&
          wl_resource_get_client (input->resource) ==
            wl_resource_get_client (surface->wl_surface))
        text_input_enter (self, input, surface);
    }

  text_input_update_active (self);
}

/* The surface is gone from the protocol already, so its inputs can only be
 * dropped, not told goodbye. */
void
text_input_forget_surface (NestedCompositor *self, NestedSurface *surface)
{
  TextInput *input;

  if (!self)
    return;

  wl_list_for_each (input, &self->text_inputs, link)
    {
      if (input->surface != surface)
        continue;

      input->entered = FALSE;
      g_clear_object (&input->surface);
      text_input_state_clear (&input->pending);
      text_input_state_clear (&input->current);
      g_clear_pointer (&input->preedit, g_free);
    }

  text_input_update_active (self);
}

static TextInput *
text_input_from_resource (struct wl_resource *resource)
{
  return wl_resource_get_user_data (resource);
}

static void
text_input_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
text_input_enable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  if (!input->entered)
    {
      g_debug ("nested: a text input was enabled before it was entered: ignored");
      return;
    }

  /* "At most one instance of text input may be in enabled state per instance":
   * a second client cannot take the keyboard's composition away. */
  if (self->active_input && self->active_input != input &&
      self->active_input->current.enabled)
    {
      g_debug ("nested: a text input is already enabled on this seat: ignored");
      return;
    }

  /* enable() resets everything that came before it. */
  text_input_state_clear (&input->pending);
  input->pending.enabled = TRUE;
}

static void
text_input_disable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.enabled = FALSE;
}

static void
text_input_set_surrounding_text (struct wl_client *client,
                                 struct wl_resource *resource,
                                 const char *text, int32_t cursor, int32_t anchor)
{
  TextInput *input = text_input_from_resource (resource);
  gsize size;

  if (!input->entered)
    return;

  g_free (input->pending.surrounding);
  input->pending.surrounding = g_strdup (text && *text ? text : NULL);
  size = input->pending.surrounding ? strlen (input->pending.surrounding) : 0;

  /* The protocol puts the indices inside the buffer; a client that is off by
   * one must not talk the compositor into reading outside it. */
  if (cursor < 0)
    cursor = 0;
  if ((gsize) cursor > size)
    cursor = (gint32) size;
  if (anchor < 0)
    anchor = 0;
  if ((gsize) anchor > size)
    anchor = (gint32) size;

  input->pending.cursor = cursor;
  input->pending.anchor = anchor;
}

static void
text_input_set_text_change_cause (struct wl_client *client,
                                  struct wl_resource *resource, uint32_t cause)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.change_cause = cause;
}

static void
text_input_set_content_type (struct wl_client *client, struct wl_resource *resource,
                             uint32_t hint, uint32_t purpose)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.hint = hint;
  input->pending.purpose = purpose;
}

static void
text_input_set_cursor_rectangle (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x, int32_t y, int32_t width, int32_t height)
{
  TextInput *input = text_input_from_resource (resource);

  input->pending.x = x;
  input->pending.y = y;
  input->pending.width = width;
  input->pending.height = height;
  input->pending.has_rect = width > 0 && height > 0;
}

static void
text_input_commit (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  input->commits++;
  text_input_state_apply (&input->current, &input->pending);

  /* Never the text itself: a sensitive field's contents stay out of the log. */
  g_debug ("nested: text input commit #%u enabled=%d caret=%d anchor=%d "
           "surrounding=%zu bytes purpose=%u hint=%u",
           input->commits, input->current.enabled, input->current.cursor,
           input->current.anchor,
           input->current.surrounding ? strlen (input->current.surrounding) : (size_t) 0,
           input->current.purpose, input->current.hint);

  /* The change cause only describes this one commit. */
  input->pending.change_cause = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;

  text_input_done (input);
  text_input_update_active (self);
  nested_flush (self);
}

static void
text_input_resource_destroyed (struct wl_resource *resource)
{
  TextInput *input = text_input_from_resource (resource);
  NestedCompositor *self = input->compositor;

  wl_list_remove (&input->link);
  wl_list_init (&input->link);

  g_clear_object (&input->surface);
  text_input_state_clear (&input->pending);
  text_input_state_clear (&input->current);
  g_clear_pointer (&input->preedit, g_free);

  text_input_update_active (self);
  g_free (input);
}

static const struct zwp_text_input_v3_interface text_input_impl = {
  .destroy = text_input_destroy,
  .enable = text_input_enable,
  .disable = text_input_disable,
  .set_surrounding_text = text_input_set_surrounding_text,
  .set_text_change_cause = text_input_set_text_change_cause,
  .set_content_type = text_input_set_content_type,
  .set_cursor_rectangle = text_input_set_cursor_rectangle,
  .commit = text_input_commit,
};

static void
text_input_manager_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
text_input_manager_get_text_input (struct wl_client *client,
                                   struct wl_resource *resource, uint32_t id,
                                   struct wl_resource *seat)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *input_resource;
  TextInput *input;

  if (!seat_resource_is_seat (seat))
    {
      post_wrong_object (resource, seat, "wl_seat");
      return;
    }

  input_resource = wl_resource_create (client, &zwp_text_input_v3_interface,
                                       wl_resource_get_version (resource), id);
  input = g_new0 (TextInput, 1);
  input->resource = input_resource;
  input->compositor = self;
  wl_list_init (&input->link);
  wl_list_insert (&self->text_inputs, &input->link);

  wl_resource_set_implementation (input_resource, &text_input_impl, input,
                                  text_input_resource_destroyed);

  /* A client that binds late still has to be told which surface it composes
   * for, or it never gets an enter and stays silent. */
  if (self->keyboard_focus &&
      wl_resource_get_client (self->keyboard_focus->wl_surface) == client)
    text_input_enter (self, input, self->keyboard_focus);

  text_input_update_active (self);
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl = {
  .destroy = text_input_manager_destroy,
  .get_text_input = text_input_manager_get_text_input,
};

void
text_input_manager_bind (struct wl_client *client, void *data, uint32_t version,
                         uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_text_input_manager_v3_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &text_input_manager_impl, data, NULL);
}

/* ------------------------------------------------------------------ *
 * text input, widget side                                            *
 * ------------------------------------------------------------------ */

/* The protocol counts bytes, GTK counts characters, so both directions are
 * clamped against the string the client actually sent. */
static gsize
text_input_offset_to_bytes (const char *text, gint offset)
{
  gsize size = text ? strlen (text) : 0;
  const char *p;
  gsize bytes;

  if (!text)
    return 0;

  p = g_utf8_offset_to_pointer (text, MAX (offset, 0));
  bytes = (gsize) (p - text);

  return MIN (bytes, size);
}

static gint
text_input_bytes_to_offset (const char *text, gsize bytes)
{
  gsize size = text ? strlen (text) : 0;

  if (!text)
    return 0;

  return (gint) g_utf8_pointer_to_offset (text, text + MIN (bytes, size));
}

gboolean
nested_compositor_text_input_active (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);
  return self->active_input != NULL;
}

void
nested_compositor_get_text_input_state (NestedCompositor *self,
                                        const char **surrounding,
                                        int *cursor, int *anchor,
                                        GdkRectangle *cursor_rect)
{
  TextInput *input;
  const char *text;

  if (surrounding)
    *surrounding = NULL;
  if (cursor)
    *cursor = 0;
  if (anchor)
    *anchor = 0;
  if (cursor_rect)
    {
      cursor_rect->x = cursor_rect->y = 0;
      cursor_rect->width = cursor_rect->height = 0;
    }

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  /* A password field keeps its contents to itself: the input method gets the
   * caret and the purpose, never the text. */
  text = text_input_state_is_sensitive (&input->current) ? NULL : input->current.surrounding;

  if (!text && input->current.surrounding)
    g_debug ("nested: hiding the field's text from the input method "
             "(purpose=%u hint=%u)", input->current.purpose, input->current.hint);

  if (surrounding)
    *surrounding = text;
  if (text)
    {
      if (cursor)
        *cursor = text_input_bytes_to_offset (text, input->current.cursor);
      if (anchor)
        *anchor = text_input_bytes_to_offset (text, input->current.anchor);
    }
  if (cursor_rect && input->current.has_rect)
    {
      cursor_rect->x = input->current.x;
      cursor_rect->y = input->current.y;
      cursor_rect->width = (int) input->current.width;
      cursor_rect->height = (int) input->current.height;
    }
}

void
nested_compositor_set_preedit_text (NestedCompositor *self, const char *text,
                                    int cursor)
{
  TextInput *input;
  int32_t begin = -1;
  int32_t end = -1;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  if (cursor >= 0)
    {
      begin = end = (int32_t) text_input_offset_to_bytes (text, cursor);
    }

  /* Repeating an unchanged preedit makes the client redraw for nothing, and an
   * empty preedit after an empty one is pure noise. */
  if (g_strcmp0 (input->preedit, text ? text : "") == 0)
    return;

  g_free (input->preedit);
  input->preedit = g_strdup (text ? text : "");

  zwp_text_input_v3_send_preedit_string (input->resource, input->preedit, begin, end);
  text_input_done (input);
  nested_flush (self);
}

void
nested_compositor_commit_text (NestedCompositor *self, const char *text)
{
  TextInput *input;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input || !text || !*text)
    return;

  g_debug ("nested: committing %u characters to the client",
           (guint) g_utf8_strlen (text, -1));

  zwp_text_input_v3_send_commit_string (input->resource, text);

  /* The preedit the client still holds is the text this commit replaces. */
  if (input->preedit)
    {
      g_clear_pointer (&input->preedit, g_free);
      zwp_text_input_v3_send_preedit_string (input->resource, "", -1, -1);
    }

  text_input_done (input);
  nested_flush (self);
}

void
nested_compositor_delete_surrounding_text (NestedCompositor *self, int offset,
                                           int n_chars)
{
  TextInput *input;
  const char *text;
  gint cursor_chars, start_chars, end_chars;
  gsize cursor_bytes, start_bytes, end_bytes;
  guint32 before, after;

  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  input = self->active_input;
  if (!input)
    return;

  text = text_input_state_is_sensitive (&input->current) ? NULL : input->current.surrounding;
  if (!text)
    {
      g_debug ("nested: the input method wants text deleted, the client gave no context");
      return;
    }

  /* @offset is where the range starts relative to the cursor and @n_chars how
   * long it is; the protocol wants the two halves on either side of it. */
  cursor_chars = text_input_bytes_to_offset (text, input->current.cursor);
  start_chars = MAX (cursor_chars + MIN (offset, 0), 0);
  end_chars = MAX (cursor_chars + MAX (offset + n_chars, 0), start_chars);

  start_bytes = text_input_offset_to_bytes (text, start_chars);
  cursor_bytes = text_input_offset_to_bytes (text, cursor_chars);
  end_bytes = text_input_offset_to_bytes (text, end_chars);

  before = (guint32) (cursor_bytes - start_bytes);
  after = (guint32) (end_bytes - cursor_bytes);

  if (!before && !after)
    return;

  zwp_text_input_v3_send_delete_surrounding_text (input->resource, before, after);
  text_input_done (input);
  nested_flush (self);
}
