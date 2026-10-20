#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "chromium-session.h"

#ifndef HELPER_PATH
#define HELPER_PATH "cefnest-helper"
#endif

/* The helper cmake built in the tree this was compiled in, which is what lets a
 * development run work without an install step — and what must not quietly win
 * after an install, because then the component runs the wrong build. */
#ifndef HELPER_DEVELOPMENT
#define HELPER_DEVELOPMENT ""
#endif

/* The session is the host half of the control channel. It listens on a private
 * unix socket, hands the path to the helper through the environment, and keeps
 * the page state the helper reports: URL, title, loading, history. The widget
 * stays a pure compositor — none of this touches Wayland. */

enum {
  SIG_CHANGED,
  SIG_READY,
  SIG_LOAD_END,
  SIG_LOAD_ERROR,
  SIG_RENDERER_EXITED,
  SIG_SCRIPT_RESULT,
  SIG_DOWNLOAD_BEGIN,
  SIG_DOWNLOAD_PROGRESS,
  SIG_DOWNLOAD_END,
  SIG_FIND_RESULT,
  SIG_CONTEXT_MENU,
  SIG_PERMISSION_REQUEST,
  SIG_PERMISSION_DISMISSED,
  SIG_CLOSED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void chromium_session_dispose  (GObject *object);
static void chromium_session_finalize (GObject *object);

static void handle_context_menu         (ChromiumSession *self, JsonObject *object);
static void handle_context_menu_dismissed (ChromiumSession *self);
static void handle_permission_request   (ChromiumSession *self, JsonObject *object);
static void handle_permission_dismissed (ChromiumSession *self, JsonObject *object);

struct _ChromiumSession {
  GObject parent_instance;

  char *helper_path;
  char *socket_path;
  ChromiumView *view;

  GSocketListener *listener;
  GSocketConnection *connection;
  GDataInputStream *input;
  GOutputStream *output;
  GCancellable *cancellable;
  GSubprocess *child;

  char *uri;
  char *title;
  char *startup_url;
  gboolean loading;
  gboolean can_go_back;
  gboolean can_go_forward;
  gboolean muted;
  gboolean running;
  gboolean stopping;
  gboolean closed_emitted;

  /* The context menu the helper last asked about: pending until the host answers
   * it, and the two widgets that draw it while this side is the one that does. */
  gboolean menu_pending;
  GtkWidget *menu_popover;
  GSimpleActionGroup *menu_actions;
  guint menu_release_source;

  /* Permission requests this side has asked about and not yet answered. Unlike a
   * menu, several are open at once, so it is a set of ids rather than a flag —
   * and an id leaves the set as soon as the helper dismisses it under the host. */
  GHashTable *permission_held;
};

G_DEFINE_TYPE (ChromiumSession, chromium_session, G_TYPE_OBJECT)

static void
chromium_session_set_state (ChromiumSession *self,
                            const char *uri,
                            const char *title,
                            gboolean loading,
                            gboolean can_go_back,
                            gboolean can_go_forward)
{
  const gboolean changed = loading != self->loading ||
                           can_go_back != self->can_go_back ||
                           can_go_forward != self->can_go_forward ||
                           (uri && strcmp (uri, self->uri)) ||
                           (title && strcmp (title, self->title));

  if (uri)
    {
      g_free (self->uri);
      self->uri = g_strdup (uri);
    }
  if (title)
    {
      g_free (self->title);
      self->title = g_strdup (title);
    }
  self->loading = loading;
  self->can_go_back = can_go_back;
  self->can_go_forward = can_go_forward;

  if (changed)
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
mark_closed (ChromiumSession *self, const char *why)
{
  /* Both halves of a stop watch the helper go: whoever asked for it, and the
   * pending child_waited once it is reaped. ::closed is the host's cue to tear
   * down a toolbar, so it goes out once per start — and never for a session
   * that was stillborn or is already being disposed. */
  if (self->closed_emitted || (!self->running && !self->child))
    return;

  self->closed_emitted = TRUE;
  self->running = FALSE;

  g_message ("session: helper stopped (%s)", why);
  chromium_session_set_state (self, NULL, NULL, FALSE, FALSE, FALSE);
  g_signal_emit (self, signals[SIG_CLOSED], 0);
}

static const char *
event_string (JsonObject *object, const char *key)
{
  /* The helper is a separate process, and a signal that documents a const char *
   * must not be handed a NULL because one field went missing on the way. */
  return json_object_has_member (object, key) ?
           json_object_get_string_member (object, key) : "";
}

static gint64
event_bytes (JsonObject *object, const char *key)
{
  /* CefDictionaryValue has no 64-bit setter, so the helper writes its byte
   * counts as JSON doubles, exact to 2^53 — a file larger than that is not what
   * this channel is for. -1 is CEF's own "the length was never told to me", and
   * a missing field reports the same rather than inventing a zero. */
  return json_object_has_member (object, key) ?
           (gint64) json_object_get_double_member (object, key) : (gint64) -1;
}

static void
handle_event (ChromiumSession *self, const char *line)
{
  g_autoptr(JsonNode) root = NULL;
  JsonObject *object;
  const char *event;
  GError *error = NULL;

  root = json_from_string (line, &error);
  if (!root)
    {
      g_warning ("session: unreadable event: %s",
                 error ? error->message : "not JSON");
      g_clear_error (&error);
      return;
    }
  if (json_node_get_node_type (root) != JSON_NODE_OBJECT)
    {
      g_warning ("session: event is not an object: %s", line);
      return;
    }

  object = json_node_get_object (root);
  event = json_object_has_member (object, "event") ?
            json_object_get_string_member (object, "event") : NULL;
  if (!event)
    {
      g_warning ("session: event without a name: %s", line);
      return;
    }

  if (!strcmp (event, "ready"))
    {
      g_message ("session: helper ready, pid=%d cef=%s",
                 (int) json_object_get_int_member (object, "pid"),
                 json_object_get_string_member (object, "cef"));

      /* The first navigation goes through the channel, not the command line:
       * Chromium rewrites its own argv during start-up, so a URL handed over
       * as an argument may not survive until the browser is created. */
      if (self->startup_url && *self->startup_url)
        {
          chromium_session_load_uri (self, self->startup_url);
          g_clear_pointer (&self->startup_url, g_free);
        }

      g_signal_emit (self, signals[SIG_READY], 0);
      return;
    }

  if (!strcmp (event, "address"))
    {
      /* Keep whatever the helper last reported as loading state: address and
       * loading arrive independently and either may come first. */
      chromium_session_set_state (self,
                                  json_object_get_string_member (object, "url"),
                                  NULL, self->loading, self->can_go_back,
                                  self->can_go_forward);
      return;
    }

  if (!strcmp (event, "title"))
    {
      chromium_session_set_state (self, NULL,
                                  json_object_get_string_member (object, "title"),
                                  self->loading, self->can_go_back,
                                  self->can_go_forward);
      return;
    }

  if (!strcmp (event, "loading"))
    {
      gboolean loading = json_object_get_boolean_member (object, "loading");
      gboolean can_go_back = json_object_get_boolean_member (object, "can_go_back");
      gboolean can_go_forward = json_object_get_boolean_member (object, "can_go_forward");

      /* Measured: a menu this side owns is not closed when the page navigates —
       * no context-menu-dismissed arrives, the popover stays up over the new
       * page, and the unanswered request also swallows the next right click. So
       * the menu goes away with the page it was asked about. */
      if (loading && self->menu_pending)
        {
          g_debug ("session: context menu dismissed by a navigation");
          chromium_session_context_menu_dismiss (self);
        }

      chromium_session_set_state (self, NULL, NULL,
                                  loading, can_go_back, can_go_forward);
      return;
    }

  if (!strcmp (event, "load-end"))
    {
      g_signal_emit (self, signals[SIG_LOAD_END], 0,
                     json_object_get_string_member (object, "url"),
                     (int) json_object_get_int_member (object, "http_code"));
      return;
    }

  if (!strcmp (event, "load-error"))
    {
      g_warning ("session: load error %d for %s: %s",
                 (int) json_object_get_int_member (object, "error_code"),
                 json_object_get_string_member (object, "url"),
                 json_object_get_string_member (object, "message"));

      /* A failed navigation also arrives as ::load-end with HTTP code 0, so a
       * host that only listens there cannot tell "loaded" from "did not": this
       * signal is the only place the reason is. */
      g_signal_emit (self, signals[SIG_LOAD_ERROR], 0,
                     (int) json_object_get_int_member (object, "error_code"),
                     json_object_get_string_member (object, "url"),
                     json_object_get_string_member (object, "message"));
      return;
    }

  if (!strcmp (event, "renderer-exited"))
    {
      /* Nothing else reports this. The surface keeps showing the last frame and
       * every command after it goes unanswered, so a host that does not listen
       * here sees a page that stopped moving rather than a dead process. */
      g_signal_emit (self, signals[SIG_RENDERER_EXITED], 0,
                     (int) json_object_get_int_member (object, "status"),
                     (int) json_object_get_int_member (object, "error_code"),
                     json_object_get_string_member (object, "message"));
      return;
    }

  if (!strcmp (event, "muted"))
    {
      /* The helper, not the caller, is the one that read CefBrowserHost back.
       * Nothing about the page changes when this happens, so ::changed is the
       * only place a host can see it land. */
      gboolean muted = json_object_get_boolean_member (object, "muted");

      if (muted != self->muted)
        {
          self->muted = muted;
          g_signal_emit (self, signals[SIG_CHANGED], 0);
        }
      return;
    }

  if (!strcmp (event, "js-result"))
    {
      JsonNode *value = json_object_has_member (object, "value") ?
                          json_object_get_member (object, "value") : NULL;
      /* The result keeps its JSON shape rather than being flattened into a
       * string: callers who want the text can parse it, and a script returning
       * an object or array still arrives whole. */
      g_autofree char *json_text =
          value ? json_to_string (value, FALSE) : g_strdup ("null");

      g_signal_emit (self, signals[SIG_SCRIPT_RESULT], 0,
                     (int) json_object_get_int_member (object, "id"),
                     json_object_get_boolean_member (object, "ok"),
                     json_text,
                     json_object_has_member (object, "message") ?
                       json_object_get_string_member (object, "message") : "");
      return;
    }

  if (!strcmp (event, "download-begin"))
    {
      /* The one thing the helper cannot decide for itself: nothing reaches the
       * disk until a host names a path with chromium_session_accept_download(),
       * and a download that is never answered stays parked with no file. */
      g_signal_emit (self, signals[SIG_DOWNLOAD_BEGIN], 0,
                     (int) json_object_get_int_member (object, "id"),
                     event_string (object, "url"),
                     event_string (object, "suggested_name"),
                     event_string (object, "mime_type"),
                     event_bytes (object, "total_bytes"));
      return;
    }

  if (!strcmp (event, "download-progress"))
    {
      /* One event per whole percent, so a large file is a hundred signals and
       * not a firehose. @percent is -1 while the length is unknown, in which
       * case only the byte counts move. */
      g_signal_emit (self, signals[SIG_DOWNLOAD_PROGRESS], 0,
                     (int) json_object_get_int_member (object, "id"),
                     event_bytes (object, "received_bytes"),
                     event_bytes (object, "total_bytes"),
                     (int) json_object_get_int_member (object, "percent"),
                     event_bytes (object, "speed"));
      return;
    }

  if (!strcmp (event, "download-end"))
    {
      /* @state is "complete", "cancelled" or "interrupted", and @path is what
       * CEF says it wrote rather than what was asked for — which is also the
       * only way a host learns the name got uniquified on the way to disk. A
       * download refused before it started ends here too, with an empty path. */
      g_signal_emit (self, signals[SIG_DOWNLOAD_END], 0,
                     (int) json_object_get_int_member (object, "id"),
                     event_string (object, "state"),
                     event_string (object, "path"),
                     event_bytes (object, "received_bytes"));
      return;
    }

  if (!strcmp (event, "find-result"))
    {
      /* Measured shape: a request that finds matches answers twice — a counting
       * update, then the same numbers with final set — and a request that finds
       * nothing answers once, count 0 with final set, since CEF 154 has no
       * separate no-match flag. @identifier is fresh for every request, so it
       * pairs the two updates of one request rather than naming a search. */
      g_signal_emit (self, signals[SIG_FIND_RESULT], 0,
                     (int) json_object_get_int_member (object, "identifier"),
                     (int) json_object_get_int_member (object, "count"),
                     (int) json_object_get_int_member (object, "active_ordinal"),
                     json_object_get_boolean_member (object, "final"));
      return;
    }

  if (!strcmp (event, "context-menu"))
    {
      /* The helper drew nothing and keeps nothing: this side owes the answer to
       * the one request Chromium has open, so a host that neither picks nor
       * dismisses gets the default menu rather than a frozen right button. */
      handle_context_menu (self, object);
      return;
    }

  if (!strcmp (event, "context-menu-dismissed"))
    {
      /* The page moved or CEF dropped the request itself. Nothing to answer, but
       * the menu on screen has to come down: it would otherwise go on picking
       * against a request that no longer exists. */
      handle_context_menu_dismissed (self);
      return;
    }

  if (!strcmp (event, "permission-request"))
    {
      /* A question the page is waiting on: an unanswered ask leaves the promise
       * pending forever, so this side always answers it — the host if it takes
       * the signal, a deny if it does not. */
      handle_permission_request (self, object);
      return;
    }

  if (!strcmp (event, "permission-dismissed"))
    {
      /* Chromium closed a prompt this side still held. Nothing to answer any
       * more, and the host has to hear that its own prompt is now dead. */
      handle_permission_dismissed (self, object);
      return;
    }

  if (!strcmp (event, "status"))
    {
      g_message ("session: helper status: %s",
                 json_object_get_string_member (object, "message"));
      return;
    }

  if (!strcmp (event, "closed"))
    {
      mark_closed (self, "helper closed the browser");
      return;
    }

  /* The raw line rather than just the name: an event this version does not
   * know is the normal state of a helper newer than its host, and the only way
   * to find out what arrived is to see it. */
  g_warning ("session: unknown event: %s", line);
}

static void
read_line_done (GObject *source, GAsyncResult *result, gpointer data)
{
  ChromiumSession *self = data;
  g_autofree char *line = NULL;
  gsize length = 0;
  GError *error = NULL;

  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source),
                                               result, &length, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !self->stopping)
        g_warning ("session: control read failed: %s", error->message);
      g_clear_error (&error);
    }
  else if (line)
    {
      handle_event (self, line);
      if (self->input)
        g_data_input_stream_read_line_async (self->input, G_PRIORITY_DEFAULT,
                                             self->cancellable,
                                             read_line_done,
                                             g_object_ref (self));
    }
  else
    {
      mark_closed (self, "control channel closed");
    }

  g_object_unref (self);
}

static void
drop_connection (ChromiumSession *self)
{
  if (self->input)
    g_object_unref (self->input);
  self->input = NULL;

  if (self->output)
    g_object_unref (self->output);
  self->output = NULL;

  g_clear_object (&self->connection);
}

static void
attach_connection (ChromiumSession *self, GSocketConnection *connection)
{
  self->connection = g_object_ref (connection);
  self->output = g_object_ref (g_io_stream_get_output_stream (G_IO_STREAM (connection)));
  self->input = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));

  /* The helper numbers its permission requests from one again, so an id left over
   * from a previous helper would answer a different request than it belongs to. */
  g_hash_table_remove_all (self->permission_held);

  g_data_input_stream_read_line_async (self->input, G_PRIORITY_DEFAULT,
                                       self->cancellable, read_line_done,
                                       g_object_ref (self));
}

static void accept_one (ChromiumSession *self);

static void
accepted (GObject *source, GAsyncResult *result, gpointer data)
{
  ChromiumSession *self = data;
  GSocketConnection *connection;
  GError *error = NULL;

  connection = g_socket_listener_accept_finish (G_SOCKET_LISTENER (source),
                                                result, NULL, &error);
  if (!connection)
    {
      if (!self->stopping &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("session: accept failed: %s", error->message);
      g_clear_error (&error);
    }
  else if (self->connection)
    {
      /* One page per widget: a second helper would fight the first over the
       * compositor's single toplevel slot. */
      g_warning ("session: ignoring a second control connection");
      g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
      g_object_unref (connection);
    }
  else
    attach_connection (self, connection);

  accept_one (self);
  g_object_unref (self);
}

static void
accept_one (ChromiumSession *self)
{
  if (self->listener && !self->stopping && !self->connection)
    g_socket_listener_accept_async (self->listener, self->cancellable,
                                    accepted, g_object_ref (self));
}

static void
send_json (ChromiumSession *self, JsonNode *root)
{
  g_autoptr (JsonGenerator) generator = NULL;
  g_autofree char *line = NULL;
  GError *error = NULL;
  gssize written;

  if (!self->output)
    {
      g_warning ("session: no control channel to send on");
      return;
    }

  generator = json_generator_new ();
  json_generator_set_root (generator, root);

  /* NDJSON: the helper's reader only dispatches on a newline. */
  line = g_strconcat (json_generator_to_data (generator, NULL), "\n", NULL);

  written = g_output_stream_write (self->output, line, strlen (line), NULL, &error);
  if (written < 0)
    {
      g_warning ("session: control write failed: %s", error->message);
      g_clear_error (&error);
      return;
    }
  if (!g_output_stream_flush (self->output, NULL, &error))
    {
      g_warning ("session: control flush failed: %s", error->message);
      g_clear_error (&error);
      return;
    }
}

static void
command_add_string (JsonBuilder *builder, const char *key, const char *value)
{
  json_builder_set_member_name (builder, key);
  json_builder_add_string_value (builder, value ? value : "");
}

static void
command_add_boolean (JsonBuilder *builder, const char *key, gboolean value)
{
  json_builder_set_member_name (builder, key);
  json_builder_add_boolean_value (builder, value);
}

static void
send_command (ChromiumSession *self, const char *cmd)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", cmd);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* ------------------------------------------------------------------ *
 * The page's context menu                                            *
 *                                                                    *
 * CEF builds the model and this side draws it, which is the whole of *
 * what makes the menu native: the items, what they say and whether   *
 * they are enabled are all Chromium's decision about the node under  *
 * the pointer, and Continue() runs Chromium's own command handling   *
 * for whichever one is picked.                                       *
 *                                                                    *
 * Answering is not optional. Chromium keeps one menu request open and  *
 * will not build a second model until the first is done, so a menu    *
 * nobody answers costs every later right click as well. That is why   *
 * this file draws a menu of its own when the host does not claim the  *
 * signal.                                                            *
 * ------------------------------------------------------------------ */

static void
menu_item_free (gpointer data)
{
  ChromiumMenuItem *item = data;

  g_free ((char *) item->label);
  g_free ((char *) item->type);
  g_free (item);
}

/* CEF spells a mnemonic "(&B)" and a GMenu label spells it "_B"; left alone, the
 * ampersand form prints literally next to the word. */
static char *
menu_label (const char *cef_label)
{
  GString *out = g_string_new (NULL);
  const char *p;

  for (p = cef_label; p && *p; p++)
    {
      if (*p == '_')
        g_string_append (out, "__");      /* a real underscore, said twice */
      else if (*p == '&' && p[1] == '(' && g_ascii_isalnum (p[2]) && p[3] == ')')
        {
          g_string_append_printf (out, "(_%c)", p[2]);
          p += 3;
        }
      else
        g_string_append_c (out, *p);
    }

  return g_string_free (out, FALSE);
}

static void menu_release      (ChromiumSession *self);
static void menu_release_soon (ChromiumSession *self);
static void menu_popover_closed (GtkPopover *popover, gpointer data);

/* Put the menu's widgets back. The popover is reached both from the answer path
 * and from its own ::closed, so this has to survive being called twice. */
static void
menu_release (ChromiumSession *self)
{
  GtkWidget *popover = self->menu_popover;

  self->menu_popover = NULL;

  if (self->menu_release_source > 0)
    {
      g_source_remove (self->menu_release_source);
      self->menu_release_source = 0;
      g_object_unref (self);      /* the reference menu_release_soon () took */
    }

  if (popover)
    {
      /* This side is the one putting the menu down, so the answer to the helper is
       * already decided and ::closed must not also send one. */
      g_signal_handlers_disconnect_by_func (popover,
                                            G_CALLBACK (menu_popover_closed), self);
      if (GTK_IS_WIDGET (gtk_widget_get_parent (popover)))
        gtk_widget_unparent (popover);
      g_object_unref (popover);
    }

  g_clear_object (&self->menu_actions);
}

/* Not from inside the popover's own ::closed, and not from inside the activate of
 * an action this menu owns: both would free the widget GTK is still emitting on,
 * so the teardown waits for one idle. */
static void
menu_release_later (gpointer data)
{
  ChromiumSession *self = data;

  self->menu_release_source = 0;
  menu_release (self);
  g_object_unref (self);
}

static void
menu_release_soon (ChromiumSession *self)
{
  if (self->menu_release_source > 0 || (!self->menu_popover && !self->menu_actions))
    return;

  self->menu_release_source =
    g_timeout_add_once (0, menu_release_later, g_object_ref (self));
}

static void
menu_popover_closed (GtkPopover *popover, gpointer data)
{
  ChromiumSession *self = data;

  /* Escape, a click outside, or the window going away: all three are a
   * dismissal as far as CEF is concerned, and the only way it learns. */
  if (self->menu_pending)
    {
      self->menu_pending = FALSE;
      send_command (self, "context-menu-dismiss");
    }

  menu_release_soon (self);
}

static void
menu_action_activate (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  ChromiumSession *self = data;
  int command = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (action), "cefnest-command"));

  chromium_session_context_menu_select (self, command);
}

/* A popover on Wayland is its own surface, so a grab of the view never shows it.
 * This is how a run finds out that the menu really got an allocation rather than
 * being built and dropped. */
static void
menu_report_size (gpointer data)
{
  GtkWidget *popover = data;

  if (GTK_IS_WIDGET (popover))
    g_debug ("session: context menu mapped=%d size=%dx%d",
             gtk_widget_get_mapped (popover),
             gtk_widget_get_width (popover), gtk_widget_get_height (popover));

  g_object_unref (popover);
}

static void
session_menu_show (ChromiumSession *self, int x, int y, GList *items)
{
  GList *l;
  GMenu *menu;
  GMenu *section;
  GtkWidget *popover;
  GdkRectangle point;
  char name[32];
  guint index = 0;
  if (!self->view)
    {
      /* Nothing to draw over. Saying so with a dismissal is what keeps the
       * helper's one open request from eating the next right click too. */
      g_debug ("session: no view to draw a context menu over");
      chromium_session_context_menu_dismiss (self);
      menu_release (self);
      return;
    }

  self->menu_actions = g_simple_action_group_new ();
  menu = g_menu_new ();
  /* Items between two separators become one section, because a section break is
   * how a GtkPopoverMenu draws a line. */
  section = g_menu_new ();

  for (l = items; l; l = l->next, index++)
    {
      ChromiumMenuItem *item = l->data;
      g_autofree char *label = NULL;
      GSimpleAction *action;

      if (!strcmp (item->type, "separator"))
        {
          if (g_menu_model_get_n_items (G_MENU_MODEL (section)) > 0)
            {
              g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
              g_object_unref (section);
              section = g_menu_new ();
            }
          continue;
        }

      g_snprintf (name, sizeof (name), "cmd-%u", index);
      label = menu_label (item->label);

      action = g_simple_action_new (name, NULL);
      g_simple_action_set_enabled (action, item->enabled);
      g_object_set_data (G_OBJECT (action), "cefnest-command",
                         GINT_TO_POINTER (item->id));
      g_signal_connect (action, "activate", G_CALLBACK (menu_action_activate), self);
      g_action_map_add_action (G_ACTION_MAP (self->menu_actions), G_ACTION (action));
      g_object_unref (action);

      /* A stateless action, even for a check or radio item: the mark CEF would
       * have shown is the menu's own state, and this menu is rebuilt from a new
       * model after every command anyway. */
      {
        g_autofree char *detailed = g_strdup_printf ("menu.%s", name);
        GMenuItem *row = g_menu_item_new (label, NULL);

        g_menu_item_set_detailed_action (row, detailed);
        g_menu_append_item (section, row);
        g_object_unref (row);
      }
    }

  if (g_menu_model_get_n_items (G_MENU_MODEL (section)) > 0)
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  if (g_menu_model_get_n_items (G_MENU_MODEL (menu)) == 0)
    {
      /* An empty menu is not a menu; dismissing is what frees the request. */
      g_object_unref (menu);
      chromium_session_context_menu_dismiss (self);
      menu_release (self);
      return;
    }

  popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  g_object_unref (menu);

  /* The view owns it as a child; this side keeps a reference of its own because
   * it has to be able to put it down from three different places. */
  gtk_widget_set_parent (popover, GTK_WIDGET (self->view));
  self->menu_popover = g_object_ref (popover);

  gtk_widget_insert_action_group (popover, "menu", G_ACTION_GROUP (self->menu_actions));

  point.x = x;
  point.y = y;
  point.width = 1;
  point.height = 1;
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &point);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  g_signal_connect (popover, "closed", G_CALLBACK (menu_popover_closed), self);

  gtk_popover_popup (GTK_POPOVER (popover));
  g_debug ("session: context menu with %u items drawn at %d,%d", index, x, y);
  g_timeout_add_once (300, menu_report_size, g_object_ref (popover));
}

static void
handle_context_menu (ChromiumSession *self, JsonObject *object)
{
  GList *items = NULL;
  gboolean handled = FALSE;
  int x, y;

  if (self->menu_pending)
    {
      /* Chromium should not have asked twice. Put the old one down without
       * answering it — the reply would land on this request instead. */
      g_debug ("session: a second context menu arrived before the first was answered");
      self->menu_pending = FALSE;
      menu_release (self);
    }

  x = (int) json_object_get_int_member (object, "x");
  y = (int) json_object_get_int_member (object, "y");

  if (json_object_has_member (object, "items") &&
      json_node_get_node_type (json_object_get_member (object, "items")) == JSON_NODE_ARRAY)
    {
      JsonArray *array = json_object_get_array_member (object, "items");
      guint i;

      for (i = 0; i < json_array_get_length (array); i++)
        {
          JsonObject *row = json_array_get_object_element (array, i);
          ChromiumMenuItem *item;

          if (!row)
            continue;

          item = g_new0 (ChromiumMenuItem, 1);
          item->id = (int) json_object_get_int_member (row, "id");
          item->label = g_strdup (event_string (row, "label"));
          item->type = g_strdup (event_string (row, "type"));
          item->enabled = json_object_has_member (row, "enabled") ?
                            json_object_get_boolean_member (row, "enabled") : TRUE;
          item->checked = json_object_has_member (row, "checked") ?
                            json_object_get_boolean_member (row, "checked") : FALSE;
          items = g_list_append (items, item);
        }
    }

  self->menu_pending = TRUE;
  g_signal_emit (self, signals[SIG_CONTEXT_MENU], 0, x, y, items, &handled);

  g_debug ("session: context menu at %d,%d with %u rows, host took it: %d",
           x, y, g_list_length (items), handled);

  if (!handled)
    session_menu_show (self, x, y, items);

  g_list_free_full (items, menu_item_free);
}

static void
handle_context_menu_dismissed (ChromiumSession *self)
{
  /* CEF closed the request itself — the page navigated, or its own code decided.
   * Nothing goes back: the answer slot is already empty over there. */
  if (!self->menu_pending)
    return;

  g_debug ("session: the helper closed the context menu");
  self->menu_pending = FALSE;

  if (self->menu_popover)
    gtk_popover_popdown (GTK_POPOVER (self->menu_popover));
  else
    menu_release (self);
}

void
chromium_session_context_menu_select (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!self->menu_pending)
    {
      g_debug ("session: no context menu to answer");
      return;
    }

  self->menu_pending = FALSE;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "context-menu-select");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);

  if (self->menu_popover)
    gtk_popover_popdown (GTK_POPOVER (self->menu_popover));
  else
    menu_release (self);
}

void
chromium_session_context_menu_dismiss (ChromiumSession *self)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!self->menu_pending)
    return;

  self->menu_pending = FALSE;
  send_command (self, "context-menu-dismiss");

  if (self->menu_popover)
    gtk_popover_popdown (GTK_POPOVER (self->menu_popover));
  else
    menu_release (self);
}

/* ------------------------------------------------------------------------ *
 * Permissions
 *
 * A page's ask for a microphone, a location or the clipboard is a question CEF
 * only lets the host answer, and — unlike a download or a search — the page is
 * standing on the answer. Measured against real Chromium (docs/permission-prompt.md):
 * an unanswered media ask leaves getUserMedia pending *and* stops the next ask
 * from ever arriving, and a prompt left held is dismissed with "ignore" the
 * moment the page asks for anything else. So this side always answers: the host's
 * decision when it takes the signal, and a deny when it does not, which is what a
 * page with no host at all sees today.
 * ------------------------------------------------------------------------ */

static const char *
permission_result_name (ChromiumPermissionResult result)
{
  switch (result)
    {
    case CHROMIUM_PERMISSION_ACCEPT:
      return "accept";
    case CHROMIUM_PERMISSION_DISMISS:
      return "dismiss";
    default:
      return "deny";
    }
}

static void
permission_respond_send (ChromiumSession *self, int id, ChromiumPermissionResult result)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "permission-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "result", permission_result_name (result));
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_permission_request (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  GList *names = NULL;
  int id;
  int raw = 0;
  const char *kind;
  const char *origin;
  gsize i;

  id = (int) json_object_get_int_member (object, "id");
  kind = event_string (object, "kind");
  origin = event_string (object, "origin");
  if (json_object_has_member (object, "raw"))
    raw = (int) json_object_get_int_member (object, "raw");

  if (json_object_has_member (object, "permissions") &&
      json_node_get_node_type (json_object_get_member (object, "permissions")) == JSON_NODE_ARRAY)
    {
      JsonArray *array = json_object_get_array_member (object, "permissions");

      for (i = 0; i < json_array_get_length (array); i++)
        names = g_list_append (names,
                               g_strdup (json_array_get_string_element (array, i)));
    }

  /* Held before the emission rather than after: a host that answers from inside
   * the signal has to find the id still there, and one that never answers has to
   * leave an id this side can still deny. */
  g_hash_table_add (self->permission_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_PERMISSION_REQUEST], 0,
                 id, kind, origin, names, raw, &handled);

  g_debug ("session: permission %d (%s) from %s with %u names, host took it: %d",
           id, kind, origin, g_list_length (names), handled);

  g_list_free_full (names, g_free);

  if (!handled)
    {
      permission_respond_send (self, id, CHROMIUM_PERMISSION_DENY);
      g_hash_table_remove (self->permission_held, GINT_TO_POINTER (id));
    }
}

static void
handle_permission_dismissed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) json_object_get_int_member (object, "id");
  const char *result = event_string (object, "result");

  /* An id this side already answered is Chromium closing up shop on the way out,
   * and an id it never held belongs to a request older than this session. Both
   * are silent, because the host has nothing left to take down. */
  if (!g_hash_table_remove (self->permission_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: permission %d dismissed as %s", id, result);
  g_signal_emit (self, signals[SIG_PERMISSION_DISMISSED], 0, id, result);
}

void
chromium_session_permission_respond (ChromiumSession *self, int id,
                                     ChromiumPermissionResult result)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->permission_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no permission request %d to answer", id);
      return;
    }

  permission_respond_send (self, id, result);
}

static void
child_waited (GObject *source, GAsyncResult *result, gpointer data)
{
  ChromiumSession *self = data;
  GError *error = NULL;

  g_subprocess_wait_finish (G_SUBPROCESS (source), result, &error);
  if (error)
    {
      if (!self->stopping)
        g_warning ("session: %s", error->message);
      g_clear_error (&error);
    }
  else if (g_subprocess_get_if_exited (G_SUBPROCESS (source)))
    g_message ("session: helper exited with status %d",
               g_subprocess_get_exit_status (G_SUBPROCESS (source)));
  else if (g_subprocess_get_if_signaled (G_SUBPROCESS (source)))
    g_message ("session: helper killed by signal %d",
               g_subprocess_get_term_sig (G_SUBPROCESS (source)));

  mark_closed (self, "process exited");
  g_clear_object (&self->child);
  drop_connection (self);
  g_object_unref (self);
}

static gboolean
open_listener (ChromiumSession *self, GError **error)
{
  g_autoptr (GSocket) socket = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  GError *local = NULL;

  g_unlink (self->socket_path);
  g_message ("session: listening for the helper at %s", self->socket_path);

  socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                         G_SOCKET_PROTOCOL_DEFAULT, error);
  if (!socket)
    return FALSE;

  address = g_unix_socket_address_new (self->socket_path);
  if (!address)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "cannot make a socket address for %s", self->socket_path);
      return FALSE;
    }
  if (!g_socket_bind (socket, address, TRUE, &local))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "cannot bind %s: %s", self->socket_path, local->message);
      g_clear_error (&local);
      return FALSE;
    }
  if (!g_socket_listen (socket, &local))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "cannot listen on %s: %s", self->socket_path, local->message);
      g_clear_error (&local);
      return FALSE;
    }

  /* The name is guessable and the protocol can drive a browser: keep the
   * socket to this user alone. */
  if (g_chmod (self->socket_path, 0600) < 0)
    g_warning ("session: cannot restrict %s: %s", self->socket_path,
               g_strerror (errno));

  self->listener = g_socket_listener_new ();
  g_socket_listener_set_backlog (self->listener, 1);
  if (!g_socket_listener_add_socket (self->listener, socket, NULL, &local))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "cannot listen on %s: %s", self->socket_path, local->message);
      g_clear_error (&local);
      g_clear_object (&self->listener);
      return FALSE;
    }

  accept_one (self);
  return TRUE;
}

static void
close_listener (ChromiumSession *self)
{
  if (self->listener)
    {
      g_signal_handlers_disconnect_by_data (self->listener, self);
      g_socket_listener_close (self->listener);
      g_clear_object (&self->listener);
    }
  if (self->socket_path)
    {
      g_unlink (self->socket_path);
      g_clear_pointer (&self->socket_path, g_free);
    }
}

gboolean
chromium_session_start (ChromiumSession *self, const char *initial_url,
                        GError **error)
{
  GSubprocessLauncher *launcher;
  const char *socket_name;
  const char *argv[4];
  int n = 0;
  GError *local = NULL;

  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->child)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "the helper is already running");
      return FALSE;
    }
  if (!self->view)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "no view set: the helper would have no compositor to draw on");
      return FALSE;
    }

  socket_name = chromium_view_get_socket_name (self->view);
  if (!socket_name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "the nested compositor has no socket");
      return FALSE;
    }

  if (!g_file_test (self->helper_path, G_FILE_TEST_IS_EXECUTABLE))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "no helper binary at %s", self->helper_path);
      return FALSE;
    }

  g_free (self->socket_path);
  {
    g_autofree char *uuid = g_uuid_string_random ();
    g_autofree char *name = g_strconcat ("cefnest-helper-", uuid, ".sock", NULL);

    /* Unique per run: two windows on the same name would let a helper attach
     * to the wrong widget. */
    self->socket_path = g_build_filename (g_get_user_runtime_dir (), name, NULL);
  }

  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  if (!open_listener (self, error))
    return FALSE;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);

  /* Native Wayland only, and only on our compositor: hand the helper the
   * private socket and hide the X11 display so Ozone cannot pick XWayland.
   *
   * GDK_BACKEND has to be pinned, not just inferred from an absent DISPLAY:
   * GTK reads it before it looks at anything else, so a host environment of
   * GDK_BACKEND=x11 makes the helper open the X11 backend on a display we just
   * removed, gdk_get_display() returns NULL, and Chromium's portal lookup
   * aborts on it before any window exists. */
  g_subprocess_launcher_setenv (launcher, "WAYLAND_DISPLAY", socket_name, TRUE);
  g_subprocess_launcher_setenv (launcher, "GDK_BACKEND", "wayland", TRUE);
  g_subprocess_launcher_setenv (launcher, "XDG_SESSION_TYPE", "wayland", TRUE);
  g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_SOCKET",
                                self->socket_path, TRUE);
  g_subprocess_launcher_unsetenv (launcher, "DISPLAY");
  g_subprocess_launcher_unsetenv (launcher, "WAYLAND_SOCKET");

  /* No URL on the command line: Chromium rewrites its own argv while starting,
   * so the first navigation is sent over the channel once the helper reports
   * ready. */
  argv[n++] = self->helper_path;
  argv[n++] = "--ozone-platform=wayland";
  argv[n++] = NULL;

  self->child = g_subprocess_launcher_spawnv (launcher, argv, &local);
  g_object_unref (launcher);

  if (!self->child)
    {
      close_listener (self);
      g_set_error (error, local->domain, local->code,
                   "cannot start the helper: %s", local->message);
      g_error_free (local);
      return FALSE;
    }

  g_free (self->startup_url);
  self->startup_url = g_strdup (initial_url);

  self->running = TRUE;
  self->stopping = FALSE;
  self->closed_emitted = FALSE;
  /* A fresh helper is a fresh browser: it starts unmuted whatever the previous
   * one was told. */
  self->muted = FALSE;
  chromium_session_set_state (self, initial_url && *initial_url ? initial_url : "",
                              "", FALSE, FALSE, FALSE);

  g_subprocess_wait_async (self->child, NULL, child_waited, g_object_ref (self));
  return TRUE;
}

void
chromium_session_stop (ChromiumSession *self)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  self->stopping = TRUE;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  if (self->child)
    g_subprocess_force_exit (self->child);

  close_listener (self);
  drop_connection (self);
  mark_closed (self, "stopped by the host");
  self->stopping = FALSE;
}

void
chromium_session_set_view (ChromiumSession *self, ChromiumView *view)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (self->view == view)
    return;
  if (view)
    g_object_ref (view);
  if (self->view)
    g_object_unref (self->view);
  self->view = view;
}

void
chromium_session_load_uri (ChromiumSession *self, const char *uri)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (uri != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "load");
  command_add_string (builder, "url", uri);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_go_back (ChromiumSession *self)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command (self, "back");
}

void
chromium_session_go_forward (ChromiumSession *self)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command (self, "forward");
}

void
chromium_session_reload (ChromiumSession *self, gboolean hard)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "reload");
  command_add_boolean (builder, "hard", hard);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_stop_loading (ChromiumSession *self)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command (self, "stop");
}

void
chromium_session_set_muted (ChromiumSession *self, gboolean muted)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "mute");
  command_add_boolean (builder, "on", muted);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_run_javascript (ChromiumSession *self, const char *code, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (code != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "js");
  command_add_string (builder, "code", code);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_accept_download (ChromiumSession *self, int id, const char *path)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  /* An empty path is a real answer in CEF's own terms — "the suggested name in
   * the default temp directory" — which is a file the host never chose, in a
   * place it does not know about. Refuse to send one. */
  g_return_if_fail (path != NULL && *path != '\0');

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "download-accept");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "path", path);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_reject_download (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "download-reject");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_find (ChromiumSession *self, const char *pattern,
                       gboolean forward, gboolean match_case, gboolean next)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  /* Chromium reads an empty search text as "stop searching". That is a separate
   * command here, so a host that means to stop says so instead of sending a
   * pattern that looks like a typo. */
  g_return_if_fail (pattern != NULL && *pattern != '\0');

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "find");
  command_add_string (builder, "pattern", pattern);
  json_builder_set_member_name (builder, "forward");
  json_builder_add_boolean_value (builder, forward != FALSE);
  json_builder_set_member_name (builder, "match_case");
  json_builder_add_boolean_value (builder, match_case != FALSE);
  /* The pattern rides along on a step as well: the helper keeps no find state,
   * so what a host asks for next is always the search it is currently in. */
  json_builder_set_member_name (builder, "next");
  json_builder_add_boolean_value (builder, next != FALSE);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_stop_finding (ChromiumSession *self, gboolean clear_selection)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "find-stop");
  json_builder_set_member_name (builder, "clear");
  json_builder_add_boolean_value (builder, clear_selection != FALSE);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

const char *
chromium_session_get_helper_path (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->helper_path;
}

const char *
chromium_session_get_uri (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), "");
  return self->uri;
}

const char *
chromium_session_get_title (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), "");
  return self->title;
}

gboolean
chromium_session_get_loading (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->loading;
}

gboolean
chromium_session_get_can_go_back (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->can_go_back;
}

gboolean
chromium_session_get_can_go_forward (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->can_go_forward;
}

gboolean
chromium_session_get_muted (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->muted;
}

gboolean
chromium_session_get_running (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->running;
}

static void
chromium_session_class_init (ChromiumSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = chromium_session_dispose;
  object_class->finalize = chromium_session_finalize;

  signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_READY] =
    g_signal_new ("ready", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_LOAD_END] =
    g_signal_new ("load-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_INT);
  signals[SIG_LOAD_ERROR] =
    g_signal_new ("load-error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIG_RENDERER_EXITED] =
    g_signal_new ("renderer-exited", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
  signals[SIG_SCRIPT_RESULT] =
    g_signal_new ("script-result", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIG_DOWNLOAD_BEGIN] =
    g_signal_new ("download-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_INT64);
  signals[SIG_DOWNLOAD_PROGRESS] =
    g_signal_new ("download-progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                  G_TYPE_INT, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT,
                  G_TYPE_INT64);
  signals[SIG_DOWNLOAD_END] =
    g_signal_new ("download-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64);
  signals[SIG_FIND_RESULT] =
    g_signal_new ("find-result", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN);
  /* The items list is a GList of ChromiumMenuItem * that belongs to the session
   * for the duration of the emission: a host that wants one kept has to copy it.
   * A handler that draws the menu answers TRUE; FALSE leaves it to the session. */
  signals[SIG_CONTEXT_MENU] =
    g_signal_new ("context-menu", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 3,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_POINTER);
  /* The names list is a GList of char * owned by the session for the duration of
   * the emission. A handler that shows UI answers TRUE, and then owes exactly one
   * reply; FALSE leaves the request to the session, which denies it. */
  signals[SIG_PERMISSION_REQUEST] =
    g_signal_new ("permission-request", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 5,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER,
                  G_TYPE_INT);
  signals[SIG_PERMISSION_DISMISSED] =
    g_signal_new ("permission-dismissed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);
  signals[SIG_CLOSED] =
    g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/* Where the helper is, in the order the ways of being wrong about it are worth
 * ruling out. CEFNEST_HELPER is the path written down on purpose;
 * CHROMIUM_HELPER flips the demo into session mode as often as it names a
 * binary, so only a value that looks like a path counts as one there. After
 * those two, both remaining candidates are guesses, and the configured install
 * path goes first: a run that quietly picked up the development build after an
 * install is a run of the wrong Chromium. */
static char *
session_find_helper (void)
{
  const char *env = g_getenv ("CEFNEST_HELPER");

  if (env && strchr (env, '/'))
    return g_strdup (env);

  env = g_getenv ("CHROMIUM_HELPER");
  if (env && strchr (env, '/'))
    return g_strdup (env);

  if (g_file_test (HELPER_PATH, G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup (HELPER_PATH);

  if (*HELPER_DEVELOPMENT &&
      g_file_test (HELPER_DEVELOPMENT, G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup (HELPER_DEVELOPMENT);

  /* Nothing to start. Name the path this build was configured with, because
   * that is the one a packager can compare against where they actually put it. */
  return g_strdup (HELPER_PATH);
}

static void
chromium_session_init (ChromiumSession *self)
{
  self->helper_path = session_find_helper ();
  self->uri = g_strdup ("");
  self->title = g_strdup ("");
  self->cancellable = g_cancellable_new ();
  self->permission_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  /* Nothing has started, so there is nothing to report as closed yet. */
  self->closed_emitted = TRUE;
}

static void
chromium_session_dispose (GObject *object)
{
  ChromiumSession *self = CHROMIUM_SESSION (object);

  self->stopping = TRUE;

  /* Whoever disposes us is already taking us down: a ::closed from the reaping
   * child_waited would reach a host that has moved on. */
  self->closed_emitted = TRUE;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  if (self->child)
    {
      g_subprocess_force_exit (self->child);
      g_clear_object (&self->child);
    }

  close_listener (self);
  drop_connection (self);

  /* The menu is the view's child, so it goes with the view; asking is already
   * off, and its ::closed must not answer a helper that is going away. */
  self->menu_pending = FALSE;
  menu_release (self);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->view);

  G_OBJECT_CLASS (chromium_session_parent_class)->dispose (object);
}

static void
chromium_session_finalize (GObject *object)
{
  ChromiumSession *self = CHROMIUM_SESSION (object);

  g_free (self->helper_path);
  g_free (self->socket_path);
  g_free (self->uri);
  g_free (self->title);
  g_free (self->startup_url);
  g_hash_table_destroy (self->permission_held);

  G_OBJECT_CLASS (chromium_session_parent_class)->finalize (object);
}

ChromiumSession *
chromium_session_new (const char *helper_path)
{
  ChromiumSession *self = g_object_new (CHROMIUM_TYPE_SESSION, NULL);

  if (helper_path && *helper_path)
    {
      g_free (self->helper_path);
      self->helper_path = g_strdup (helper_path);
    }

  return self;
}
