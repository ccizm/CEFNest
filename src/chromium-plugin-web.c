#define G_LOG_DOMAIN "chromium-plugin"

#include <string.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "chromium-plugin.h"
#include "chromium-plugin-private.h"

/* ------------------------------------------------------------------ *
 * The Web frontend: the host-side half of the chrome.* shim          *
 *                                                                    *
 * The helper injects the shim; everything it offers is adjudicated   *
 * here, against the same permission table the native frontend's      *
 * registry answers to. A call arrives as one plugin-message event,   *
 * runs through the manager's ::plugin-message (a host that returns   *
 * TRUE owns the call and answers with respond()), and falls through  *
 * to the built-in handlers otherwise. The answer is one              *
 * plugin-respond command; the reply id is the only thread between    *
 * the two.                                                           *
 *                                                                    *
 * Nothing here draws, and nothing here trusts the caller's identity  *
 * further than the plan's threat model does: the id on the wire is   *
 * what the shim put there, and a page that forges it gets the        *
 * permissions of the plugin it forges — the no-isolated-world        *
 * boundary docs/plugins.md writes down, not a hole this file opens.  *
 * ------------------------------------------------------------------ */

/* Storage saves are merged on this interval, not written per call: a script
 * that sets a hundred keys costs one file write. */
#define STORAGE_SAVE_INTERVAL_MS 500
/* Replies nobody collected. The page that asked may be gone; the table is
 * what lets a late respond() find the browser to answer through. */
#define PENDING_MAX 256

typedef struct {
  char *id;
  GHashTable *values;      /* key -> JSON text */
  gboolean dirty;
  guint save_source;
} PluginStorage;

typedef struct {
  int request_id;          /* the browsers-list request this answers */
  int reply_id;            /* the page's reply id to answer through */
  int browser;             /* the browser that asked */
} TabsQuery;

/* A reply id is only unique inside one page: every shim starts its counter at
 * 1. The browser has to travel with it, or two pages' first calls swallow
 * each other's answer. */
typedef struct {
  int browser;
  int reply_id;
} PendingCall;

typedef struct {
  GHashTable *storage;          /* plugin id -> PluginStorage* */
  GHashTable *pending;          /* browser id -> set of reply ids */
  GQueue *pending_order;        /* PendingCall*, oldest first — eviction order */
  GHashTable *web_slots;        /* menu slot (int) -> plugin id */
  GHashTable *action_browsers;  /* plugin id -> GArray of int browser ids */
  GHashTable *tabs_queries;     /* request id (int) -> TabsQuery* */
  int next_tabs_request;
  gboolean sync_warned;         /* the one storage.sync warning */
} WebState;

/* One per manager that has plugin support, same keying as the native
 * frontend's records. */
static GHashTable *g_web_states;

static void plugin_storage_free (gpointer data);
static void web_state_free (gpointer data);

static WebState *
web_state_for (ChromiumPluginManager *manager, gboolean create)
{
  WebState *state;

  if (!g_web_states)
    {
      if (!create)
        return NULL;
      g_web_states = g_hash_table_new_full (NULL, NULL, NULL,
                                            (GDestroyNotify) web_state_free);
    }

  state = g_hash_table_lookup (g_web_states, manager);
  if (!state && create)
    {
      state = g_new0 (WebState, 1);
      state->storage = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, plugin_storage_free);
      state->pending = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL,
                                              (GDestroyNotify)
                                              g_hash_table_unref);
      state->pending_order = g_queue_new ();
      state->web_slots = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                NULL, g_free);
      state->action_browsers =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                               (GDestroyNotify) g_array_unref);
      state->tabs_queries = g_hash_table_new_full (g_direct_hash,
                                                   g_direct_equal, NULL,
                                                   g_free);
      g_hash_table_insert (g_web_states, manager, state);
    }
  return state;
}

static void
web_state_free (gpointer data)
{
  WebState *state = data;

  g_clear_pointer (&state->storage, g_hash_table_unref);
  g_clear_pointer (&state->pending, g_hash_table_unref);
  if (state->pending_order)
    {
      g_queue_free_full (state->pending_order, g_free);
      state->pending_order = NULL;
    }
  g_clear_pointer (&state->web_slots, g_hash_table_unref);
  g_clear_pointer (&state->action_browsers, g_hash_table_unref);
  g_clear_pointer (&state->tabs_queries, g_hash_table_unref);
  g_free (state);
}

/* ------------------------------------------------------------------ *
 * JSON helpers                                                       *
 * ------------------------------------------------------------------ */

static char *
json_quoted (const char *text)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) node = json_node_new (JSON_NODE_VALUE);

  json_node_set_string (node, text ? text : "");
  json_generator_set_root (generator, node);
  return json_generator_to_data (generator, NULL);
}

static char *
json_value_text (JsonNode *node)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();

  json_generator_set_root (generator, node);
  return json_generator_to_data (generator, NULL);
}

/* The session's own event_string rule: a missing or non-string member is an
   empty answer, never a critical. */
static const char *
member_string (JsonObject *object, const char *key)
{
  JsonNode *node;

  if (!object || !json_object_has_member (object, key))
    return "";
  node = json_object_get_member (object, key);
  if (!JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return "";
  return json_node_get_string (node);
}

/* ------------------------------------------------------------------ *
 * Storage                                                            *
 * ------------------------------------------------------------------ */

static void
plugin_storage_free (gpointer data)
{
  PluginStorage *storage = data;

  if (storage->save_source)
    g_source_remove (storage->save_source);
  g_clear_pointer (&storage->id, g_free);
  g_clear_pointer (&storage->values, g_hash_table_unref);
  g_free (storage);
}

/* The file one plugin's storage lives in, or %NULL when no data directory
 * backs this session — the whole on-disk half of storage hangs off that one
 * path, exactly as the data directory's contract says. */
static char *
plugin_storage_path (ChromiumPluginManager *manager, const char *id)
{
  ChromiumSession *session = _chromium_plugin_manager_get_session (manager);
  const char *dir = session ? chromium_session_get_data_directory (session)
                            : NULL;

  if (!dir || !*dir || !g_path_is_absolute (dir))
    return NULL;

  return g_build_filename (dir, "cefnest-plugins", id, "storage.json", NULL);
}

static void
plugin_storage_write (PluginStorage *storage, const char *path)
{
  GString *text = g_string_new ("{");
  GHashTableIter iter;
  gpointer key, value;
  g_autofree char *tmp = NULL;
  gboolean first = TRUE;

  /* Values are JSON texts already; they are spliced in verbatim, so the file
     is exactly what a get() hands back for every key. */
  g_hash_table_iter_init (&iter, storage->values);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (!first)
        g_string_append_c (text, ',');
      first = FALSE;
      g_autofree char *quoted = json_quoted (key);
      g_string_append (text, quoted);
      g_string_append_c (text, ':');
      g_string_append (text, value ? (const char *) value : "null");
    }
  g_string_append_c (text, '}');

  /* The directory may not exist yet: storage lives under
     <data-dir>/cefnest-plugins/<id>/, and nothing else creates that — an
     install does, but a web plugin's storage can arrive first. */
  g_autofree char *dir = g_path_get_dirname (path);

  g_mkdir_with_parents (dir, 0700);

  tmp = g_strdup_printf ("%s.tmp", path);
  if (!g_file_set_contents (tmp, text->str, -1, NULL) ||
      g_rename (tmp, path) != 0)
    {
      g_debug ("plugin: storage for \"%s\" could not be written",
               storage->id);
      g_unlink (tmp);
    }
  g_string_free (text, TRUE);
}

/* The manager whose table holds this storage — the write only needs the
   data directory the session carries, and the session is reachable from the
   manager. */
static ChromiumPluginManager *
plugin_storage_manager (PluginStorage *storage)
{
  GHashTableIter iter;
  gpointer key, value;

  if (!g_web_states)
    return NULL;

  g_hash_table_iter_init (&iter, g_web_states);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      WebState *state = value;

      if (g_hash_table_contains (state->storage, storage->id))
        return CHROMIUM_PLUGIN_MANAGER (key);
    }
  return NULL;
}

static gboolean
plugin_storage_save (gpointer data)
{
  PluginStorage *storage = data;
  g_autofree char *path = NULL;

  storage->save_source = 0;
  if (!storage->dirty)
    return G_SOURCE_REMOVE;

  path = plugin_storage_path (plugin_storage_manager (storage), storage->id);
  if (path)
    plugin_storage_write (storage, path);
  storage->dirty = FALSE;
  return G_SOURCE_REMOVE;
}

static PluginStorage *
plugin_storage_get (ChromiumPluginManager *manager, WebState *state,
                    const char *id)
{
  PluginStorage *storage = g_hash_table_lookup (state->storage, id);

  if (storage)
    return storage;

  storage = g_new0 (PluginStorage, 1);
  storage->id = g_strdup (id);
  storage->values = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);
  g_hash_table_insert (state->storage, g_strdup (id), storage);

  /* One read at first use. The table holds JSON texts and the file splices
     them in verbatim, so each member is serialised back to text: a string
     keeps its quotes and an object comes back as an object, exactly as set()
     received it. A file that is not there is a plugin that has never stored
     anything; one that does not parse is treated the same way. */
  {
    g_autofree char *path = plugin_storage_path (manager, id);
    g_autofree char *text = NULL;
    g_autoptr (JsonNode) root = NULL;
    g_autoptr (GError) error = NULL;
    JsonObject *object;

    if (path && g_file_test (path, G_FILE_TEST_IS_REGULAR) &&
        g_file_get_contents (path, &text, NULL, NULL) &&
        (root = json_from_string (text, &error)) != NULL &&
        JSON_NODE_HOLDS_OBJECT (root))
      {
        object = json_node_get_object (root);
        for (GList *members = json_object_get_members (object); members;
             members = members->next)
          {
            const char *key = members->data;
            JsonNode *node = json_object_get_member (object, key);
            g_autofree char *raw = NULL;

            if (!node)
              continue;
            raw = json_value_text (node);
            g_hash_table_replace (storage->values, g_strdup (key),
                                  g_steal_pointer (&raw));
          }
      }
    g_clear_error (&error);
  }

  return storage;
}

static void
plugin_storage_mark_dirty (PluginStorage *storage)
{
  storage->dirty = TRUE;
  if (!storage->save_source)
    storage->save_source = g_timeout_add (STORAGE_SAVE_INTERVAL_MS,
                                          plugin_storage_save, storage);
}

/* ------------------------------------------------------------------ *
 * Responding                                                         *
 * ------------------------------------------------------------------ */

/* Take the record for one call out of both places it lives. @browser 0 means
 * "whoever is waiting": that is respond()'s path, which the host calls with a
 * reply id and nothing else, so the oldest call carrying that id answers.
 * Returns the browser the answer is owed to, or -1 for nobody. */
static int
pending_take (WebState *state, int browser, int reply_id)
{
  for (GList *link = state->pending_order->head; link; link = link->next)
    {
      PendingCall *call = link->data;
      GHashTable *ids;

      if (call->reply_id != reply_id)
        continue;
      if (browser != 0 && call->browser != browser)
        continue;

      ids = g_hash_table_lookup (state->pending,
                                 GINT_TO_POINTER (call->browser));
      if (ids)
        {
          g_hash_table_remove (ids, GINT_TO_POINTER (reply_id));
          if (g_hash_table_size (ids) == 0)
            g_hash_table_remove (state->pending,
                                 GINT_TO_POINTER (call->browser));
        }
      g_queue_delete_link (state->pending_order, link);
      browser = call->browser;
      g_free (call);
      return browser;
    }
  return -1;
}

static void
web_respond (WebState *state, ChromiumSession *session, int browser,
             int reply_id, gboolean ok, const char *value_json)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();

  browser = pending_take (state, browser, reply_id);
  if (browser < 0)
    {
      g_debug ("plugin: reply %d has nobody waiting", reply_id);
      return;
    }

  if (!value_json || !*value_json)
    value_json = "null";

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "cmd");
  json_builder_add_string_value (builder, "plugin-respond");
  json_builder_set_member_name (builder, "browser");
  json_builder_add_int_value (builder, browser);
  json_builder_set_member_name (builder, "reply_id");
  json_builder_add_int_value (builder, reply_id);
  json_builder_set_member_name (builder, "ok");
  json_builder_add_boolean_value (builder, ok);
  json_builder_set_member_name (builder, "value");
  json_builder_add_string_value (builder, value_json);
  json_builder_end_object (builder);
  _chromium_session_send_plugin_command (session, builder);
}

static void
pending_record (WebState *state, int reply_id, int browser)
{
  GHashTable *ids = g_hash_table_lookup (state->pending,
                                         GINT_TO_POINTER (browser));
  PendingCall *call;

  /* Two shims in one page share the browser id and still count from 1. That
     pair is as fine-grained as delivery gets (window.__cefnestDeliver is the
     last shim's), so the second call is refused here rather than answered
     through the wrong shim. */
  if (ids && g_hash_table_contains (ids, GINT_TO_POINTER (reply_id)))
    return;

  while (g_queue_get_length (state->pending_order) >= PENDING_MAX)
    {
      PendingCall *oldest = g_queue_peek_head (state->pending_order);

      pending_take (state, oldest->browser, oldest->reply_id);
    }

  /* After the evictions, not before: dropping the last call for this browser
     takes the per-browser table with it. */
  ids = g_hash_table_lookup (state->pending, GINT_TO_POINTER (browser));
  if (!ids)
    {
      ids = g_hash_table_new (g_direct_hash, g_direct_equal);
      g_hash_table_insert (state->pending, GINT_TO_POINTER (browser), ids);
    }

  call = g_new (PendingCall, 1);
  call->reply_id = reply_id;
  call->browser = browser;
  g_hash_table_add (ids, GINT_TO_POINTER (reply_id));
  g_queue_push_tail (state->pending_order, call);
}

/* ------------------------------------------------------------------ *
 * The built-in handlers                                              *
 * ------------------------------------------------------------------ */

static gboolean
plugin_has_permission (ChromiumPluginManager *manager, const char *id,
                       const char *permission)
{
  g_autoptr (ChromiumPlugin) plugin =
    chromium_plugin_manager_lookup (manager, id);
  const char *const *granted;

  if (!plugin)
    return FALSE;

  granted = chromium_plugin_get_granted_permissions (plugin);
  for (guint i = 0; granted && granted[i]; i++)
    if (g_strcmp0 (granted[i], permission) == 0)
      return TRUE;

  return FALSE;
}

/* The refusal face the storage handler inlines: a plugin-error naming the
   table's verdict, and the shim's promise rejection. tabs and contextMenus
   enumerate and steer every browser on the host, so they ride the same
   table — docs/plugins.md's permission vocabulary names them for exactly
   this. */
static void
respond_permission_denied (ChromiumPluginManager *manager, WebState *state,
                           ChromiumSession *session, int browser,
                           int reply_id, const char *id,
                           const char *permission)
{
  g_autofree char *token = g_strdup_printf ("permission-denied:%s",
                                            permission);
  g_autofree char *text = g_strdup_printf (
    "\"%s was not granted to this plugin\"", permission);

  g_signal_emit_by_name (manager, "plugin-error", id, token);
  web_respond (state, session, browser, reply_id, FALSE, text);
}

/* chrome.storage's get: null for everything, a string for one key, an array
   for a set, an object whose members are defaults. All four land here. */
static void
storage_get_respond (WebState *state, ChromiumSession *session, int browser,
                     int reply_id, PluginStorage *storage, JsonNode *arg)
{
  GString *out = g_string_new ("{");
  gboolean first = TRUE;

  if (arg && JSON_NODE_HOLDS_VALUE (arg) &&
      json_node_get_value_type (arg) == G_TYPE_STRING)
    {
      const char *key = json_node_get_string (arg);
      const char *value = g_hash_table_lookup (storage->values, key);
      g_autofree char *quoted = json_quoted (key);

      g_string_append_printf (out, "%s%s:%s", first ? "" : ",", quoted,
                              value ? value : "null");
      first = FALSE;
    }
  else if (arg && JSON_NODE_HOLDS_ARRAY (arg))
    {
      JsonArray *keys = json_node_get_array (arg);

      for (guint i = 0; i < json_array_get_length (keys); i++)
        {
          JsonNode *node = json_array_get_element (keys, i);

          if (!node || !JSON_NODE_HOLDS_VALUE (node) ||
              json_node_get_value_type (node) != G_TYPE_STRING)
            continue;
          const char *key = json_node_get_string (node);
          const char *value = g_hash_table_lookup (storage->values, key);
          g_autofree char *quoted = json_quoted (key);

          g_string_append_printf (out, "%s%s:%s", first ? "" : ",", quoted,
                                  value ? value : "null");
          first = FALSE;
        }
    }
  else if (arg && JSON_NODE_HOLDS_OBJECT (arg))
    {
      JsonObject *defaults = json_node_get_object (arg);
      GList *members = json_object_get_members (defaults);

      for (; members; members = members->next)
        {
          const char *key = members->data;
          const char *value = g_hash_table_lookup (storage->values, key);
          g_autofree char *quoted = json_quoted (key);

          if (value)
            {
              g_string_append_printf (out, "%s%s:%s", first ? "" : ",",
                                      quoted, value);
            }
          else
            {
              JsonNode *default_value =
                json_object_get_member (defaults, key);
              g_autofree char *text = default_value ?
                json_value_text (default_value) : g_strdup ("null");

              g_string_append_printf (out, "%s%s:%s", first ? "" : ",",
                                      quoted, text);
            }
          first = FALSE;
        }
    }
  else
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, storage->values);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          g_autofree char *quoted = json_quoted (key);

          g_string_append_printf (out, "%s%s:%s", first ? "" : ",", quoted,
                                  value ? (const char *) value : "null");
          first = FALSE;
        }
    }

  g_string_append_c (out, '}');
  web_respond (state, session, browser, reply_id, TRUE, out->str);
  g_string_free (out, TRUE);
}

static void
handle_storage (WebState *state, ChromiumPluginManager *manager,
                ChromiumSession *session, int browser, int reply_id,
                const char *id, const char *api, JsonArray *args)
{
  const char *after_prefix = api + strlen ("storage.");
  const char *dot = strchr (after_prefix, '.');
  char area_name[16] = {0};
  const char *verb = "";
  JsonNode *arg = NULL;

  if (dot)
    {
      gsize n = MIN ((gsize) (dot - after_prefix), sizeof (area_name) - 1);

      memcpy (area_name, after_prefix, n);
      verb = dot + 1;
    }

  if (args && json_array_get_length (args) > 0)
    arg = json_array_get_element (args, 0);

  /* local is the one area, sync the aliased twin; anything else a porting
     plugin names (session, managed, …) has no twin here and silently reading
     it as local would hide the mistake. */
  if (area_name[0] != '\0' && g_strcmp0 (area_name, "local") != 0 &&
      g_strcmp0 (area_name, "sync") != 0)
    {
      g_signal_emit_by_name (manager, "plugin-error", id,
                             "unknown-storage-area");
      web_respond (state, session, browser, reply_id, FALSE,
                   "\"this host answers only storage.local and storage.sync\"");
      return;
    }

  if (g_strcmp0 (area_name, "sync") == 0)
    {
      /* The plan's ruling: sync is local's twin here, and the one thing a
         person debugging a sync-aware plugin needs to see is that it is not
         Chrome's sync. Warned once, answered every time. */
      if (!state->sync_warned)
        {
          state->sync_warned = TRUE;
          g_signal_emit_by_name (manager, "plugin-error", id,
                                 "unsupported-field:storage.sync");
        }
    }

  if (!plugin_has_permission (manager, id, "storage"))
    {
      g_signal_emit_by_name (manager, "plugin-error", id,
                             "permission-denied:storage");
      web_respond (state, session, browser, reply_id, FALSE,
                   "\"storage was not granted to this plugin\"");
      return;
    }

  PluginStorage *storage = plugin_storage_get (manager, state, id);

  if (g_strcmp0 (verb, "get") == 0)
    {
      storage_get_respond (state, session, browser, reply_id, storage, arg);
      return;
    }

  if (g_strcmp0 (verb, "set") == 0 && arg && JSON_NODE_HOLDS_OBJECT (arg))
    {
      JsonObject *items = json_node_get_object (arg);
      GList *members = json_object_get_members (items);

      for (; members; members = members->next)
        {
          const char *key = members->data;
          JsonNode *value = json_object_get_member (items, key);
          g_autofree char *text = json_value_text (value);

          g_hash_table_replace (storage->values, g_strdup (key),
                                g_steal_pointer (&text));
        }
      plugin_storage_mark_dirty (storage);
      web_respond (state, session, browser, reply_id, TRUE, "null");
      return;
    }

  if (g_strcmp0 (verb, "remove") == 0)
    {
      if (arg && JSON_NODE_HOLDS_VALUE (arg) &&
          json_node_get_value_type (arg) == G_TYPE_STRING)
        g_hash_table_remove (storage->values, json_node_get_string (arg));
      else if (arg && JSON_NODE_HOLDS_ARRAY (arg))
        {
          JsonArray *keys = json_node_get_array (arg);

          for (guint i = 0; i < json_array_get_length (keys); i++)
            {
              JsonNode *node = json_array_get_element (keys, i);

              if (node && JSON_NODE_HOLDS_VALUE (node) &&
                  json_node_get_value_type (node) == G_TYPE_STRING)
                g_hash_table_remove (storage->values,
                                     json_node_get_string (node));
            }
        }
      plugin_storage_mark_dirty (storage);
      web_respond (state, session, browser, reply_id, TRUE, "null");
      return;
    }

  if (g_strcmp0 (verb, "clear") == 0)
    {
      g_hash_table_remove_all (storage->values);
      plugin_storage_mark_dirty (storage);
      web_respond (state, session, browser, reply_id, TRUE, "null");
      return;
    }

  web_respond (state, session, browser, reply_id, FALSE, "\"unknown storage verb\"");
}

/* runtime.sendMessage: delivered to the plugin's own extension pages that
 * are open (their onMessage listeners hear it), and answered with the
 * message echoed back. Chrome's version waits for the receiver's
 * sendResponse; no receiver of ours can answer yet, so the echo is the
 * documented shape of the round trip. */
static void
handle_runtime_send_message (WebState *state, ChromiumSession *session,
                             int browser, int reply_id, const char *id,
                             JsonArray *args)
{
  JsonNode *arg = args && json_array_get_length (args) > 0 ?
    json_array_get_element (args, 0) : NULL;
  g_autofree char *msg_json = arg ? json_value_text (arg) : g_strdup ("null");

  _chromium_session_deliver_plugin_message (session, id, msg_json);
  web_respond (state, session, browser, reply_id, TRUE, msg_json);
}

static void
handle_context_menus (WebState *state, ChromiumSession *session,
                      int browser, int reply_id, const char *id,
                      const char *api, JsonArray *args)
{
  const char *verb = strrchr (api, '.');

  verb = verb ? verb + 1 : "";

  if (g_strcmp0 (verb, "create") == 0)
    {
      /* Slot allocation walks down from the top so a web plugin's rows never
         look like the small numbers a native plugin's author would pick;
         both share the one 0–199 space the helper keys its table by. */
      int slot = 199;
      const char *title = NULL;
      JsonNode *arg = args && json_array_get_length (args) > 0 ?
        json_array_get_element (args, 0) : NULL;

      while (slot >= 0 &&
             g_hash_table_contains (state->web_slots,
                                    GINT_TO_POINTER (slot)))
        slot--;
      if (slot < 0)
        {
          web_respond (state, session, browser, reply_id, FALSE,
                       "\"no free menu slot\"");
          return;
        }

      if (arg && JSON_NODE_HOLDS_OBJECT (arg))
        {
          JsonNode *node = json_object_get_member (json_node_get_object (arg),
                                                   "title");

          if (node && JSON_NODE_HOLDS_VALUE (node) &&
              json_node_get_value_type (node) == G_TYPE_STRING)
            title = json_node_get_string (node);
        }
      if (!title || !*title)
        {
          web_respond (state, session, browser, reply_id, FALSE,
                       "\"contextMenus.create needs a title\"");
          return;
        }

      g_hash_table_insert (state->web_slots, GINT_TO_POINTER (slot),
                           g_strdup (id));

      {
        g_autoptr (JsonBuilder) builder = json_builder_new ();

        json_builder_begin_object (builder);
        json_builder_set_member_name (builder, "cmd");
        json_builder_add_string_value (builder, "menu-add-user-item");
        json_builder_set_member_name (builder, "slot");
        json_builder_add_int_value (builder, slot);
        json_builder_set_member_name (builder, "label");
        json_builder_add_string_value (builder, title);
        json_builder_end_object (builder);
        _chromium_session_send_plugin_command (session, builder);
      }
      {
        g_autofree char *slot_json = g_strdup_printf ("%d", slot);

        web_respond (state, session, browser, reply_id, TRUE, slot_json);
      }
      return;
    }

  if (g_strcmp0 (verb, "remove") == 0)
    {
      JsonNode *arg = args && json_array_get_length (args) > 0 ?
        json_array_get_element (args, 0) : NULL;
      int slot = arg && JSON_NODE_HOLDS_VALUE (arg) ?
        (int) json_node_get_int (arg) : -1;

      if (g_hash_table_contains (state->web_slots, GINT_TO_POINTER (slot)))
        {
          g_hash_table_remove (state->web_slots, GINT_TO_POINTER (slot));

          g_autoptr (JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "cmd");
          json_builder_add_string_value (builder, "menu-remove-user-item");
          json_builder_set_member_name (builder, "slot");
          json_builder_add_int_value (builder, slot);
          json_builder_end_object (builder);
          _chromium_session_send_plugin_command (session, builder);
        }
      web_respond (state, session, browser, reply_id, TRUE, "null");
      return;
    }

  web_respond (state, session, browser, reply_id, FALSE,
               "\"unknown contextMenus verb\"");
}

static void
handle_tabs_query (WebState *state, ChromiumSession *session, int browser,
                   int reply_id)
{
  TabsQuery *query = g_new0 (TabsQuery, 1);
  int request_id = ++state->next_tabs_request;
  g_autoptr (JsonBuilder) builder = json_builder_new ();

  query->request_id = request_id;
  query->reply_id = reply_id;
  query->browser = browser;
  g_hash_table_insert (state->tabs_queries, GINT_TO_POINTER (request_id),
                       query);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "cmd");
  json_builder_add_string_value (builder, "browsers-list");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, request_id);
  json_builder_end_object (builder);
  _chromium_session_send_plugin_command (session, builder);
}

static void
handle_tabs_update (WebState *state, ChromiumSession *session, int browser,
                    int reply_id, JsonArray *args)
{
  JsonNode *id_node = args && json_array_get_length (args) > 0 ?
    json_array_get_element (args, 0) : NULL;
  JsonNode *props = args && json_array_get_length (args) > 1 ?
    json_array_get_element (args, 1) : NULL;
  int tab = id_node && JSON_NODE_HOLDS_VALUE (id_node) ?
    (int) json_node_get_int (id_node) : 0;

  if (tab <= 0)
    {
      web_respond (state, session, browser, reply_id, FALSE,
                   "\"tabs.update needs an id\"");
      return;
    }

  if (props && JSON_NODE_HOLDS_OBJECT (props))
    {
      JsonObject *object = json_node_get_object (props);
      JsonNode *url = json_object_get_member (object, "url");
      JsonNode *active = json_object_get_member (object, "active");
      JsonNode *muted = json_object_get_member (object, "muted");

      if (url && JSON_NODE_HOLDS_VALUE (url) &&
          json_node_get_value_type (url) == G_TYPE_STRING)
        _chromium_session_load_uri_in_browser (session, tab,
                                               json_node_get_string (url));
      if (active && json_node_get_boolean (active))
        {
          g_autoptr (JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "cmd");
          json_builder_add_string_value (builder, "select-browser");
          json_builder_set_member_name (builder, "browser");
          json_builder_add_int_value (builder, tab);
          json_builder_end_object (builder);
          _chromium_session_send_plugin_command (session, builder);
        }
      if (muted && JSON_NODE_HOLDS_VALUE (muted))
        {
          g_autoptr (JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "cmd");
          json_builder_add_string_value (builder, "mute");
          json_builder_set_member_name (builder, "browser");
          json_builder_add_int_value (builder, tab);
          json_builder_set_member_name (builder, "on");
          json_builder_add_boolean_value (builder,
                                          json_node_get_boolean (muted));
          json_builder_end_object (builder);
          _chromium_session_send_plugin_command (session, builder);
        }
    }

  {
    g_autofree char *result = g_strdup_printf ("{\"id\":%d}", tab);

    web_respond (state, session, browser, reply_id, TRUE, result);
  }
}

static void
handle_action_listen (WebState *state, ChromiumSession *session, int browser,
                      int reply_id, const char *id)
{
  GArray *browsers = g_hash_table_lookup (state->action_browsers, id);

  if (!browsers)
    {
      browsers = g_array_new (FALSE, FALSE, sizeof (int));
      g_hash_table_insert (state->action_browsers, g_strdup (id), browsers);
    }
  for (guint i = 0; i < browsers->len; i++)
    if (g_array_index (browsers, int, i) == browser)
      {
        web_respond (state, session, browser, reply_id, TRUE, "null");
        return;
      }
  g_array_append_val (browsers, browser);
  web_respond (state, session, browser, reply_id, TRUE, "null");
}

/* ------------------------------------------------------------------ *
 * Entry points                                                       *
 * ------------------------------------------------------------------ */

void
chromium_plugin_web_attach (ChromiumPluginManager *manager,
                            ChromiumSession *session)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));
  g_return_if_fail (CHROMIUM_IS_SESSION (session));

  web_state_for (manager, TRUE);
}

void
chromium_plugin_web_detach (ChromiumPluginManager *manager)
{
  g_return_if_fail (manager == NULL || CHROMIUM_IS_PLUGIN_MANAGER (manager));

  if (!manager || !g_web_states)
    return;

  /* Anything dirty gets its one write before the state goes. */
  WebState *state = g_hash_table_lookup (g_web_states, manager);

  if (state)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, state->storage);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          PluginStorage *storage = value;
          g_autofree char *path = plugin_storage_path (manager, storage->id);

          if (storage->save_source)
            {
              g_source_remove (storage->save_source);
              storage->save_source = 0;
            }
          if (storage->dirty && path)
            {
              plugin_storage_write (storage, path);
              storage->dirty = FALSE;
            }
        }
    }
  g_hash_table_remove (g_web_states, manager);
}

void
chromium_plugin_web_handle_message (ChromiumPluginManager *manager,
                                    int browser, int reply_id,
                                    const char *id, const char *api,
                                    const char *args_json)
{
  WebState *state;
  ChromiumSession *session;
  gboolean handled = FALSE;
  g_autoptr (JsonNode) args = NULL;
  JsonArray *array;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));

  state = web_state_for (manager, TRUE);
  session = _chromium_plugin_manager_get_session (manager);
  pending_record (state, reply_id, browser);

  /* The host's chance to own the call. TRUE stops the emission — and means
     respond() is the handler's job now. */
  g_signal_emit_by_name (manager, "plugin-message", id, api,
                         args_json ? args_json : "null", reply_id, &handled);
  if (handled)
    return;

  args = json_from_string (args_json ? args_json : "null", NULL);
  array = args && JSON_NODE_HOLDS_ARRAY (args) ?
    json_node_get_array (args) : NULL;

  if (g_str_has_prefix (api, "storage."))
    handle_storage (state, manager, session, browser, reply_id, id, api,
                    array);
  else if (g_str_has_prefix (api, "contextMenus.") &&
           !plugin_has_permission (manager, id, "contextMenus"))
    respond_permission_denied (manager, state, session, browser, reply_id, id,
                               "contextMenus");
  else if (g_str_has_prefix (api, "contextMenus."))
    handle_context_menus (state, session, browser, reply_id, id, api, array);
  else if (g_str_has_prefix (api, "tabs.") &&
           !plugin_has_permission (manager, id, "tabs"))
    respond_permission_denied (manager, state, session, browser, reply_id, id,
                               "tabs");
  else if (g_strcmp0 (api, "runtime.sendMessage") == 0)
    handle_runtime_send_message (state, session, browser, reply_id, id, array);
  else if (g_strcmp0 (api, "runtime.onMessage") == 0)
    /* Listeners are registered shim-side; nothing delivers to them yet. */
    web_respond (state, session, browser, reply_id, TRUE, "null");
  else if (g_strcmp0 (api, "tabs.query") == 0)
    handle_tabs_query (state, session, browser, reply_id);
  else if (g_strcmp0 (api, "tabs.update") == 0)
    handle_tabs_update (state, session, browser, reply_id, array);
  else if (g_strcmp0 (api, "action.onClicked.listen") == 0)
    handle_action_listen (state, session, browser, reply_id, id);
  else
    {
      g_signal_emit_by_name (manager, "plugin-error", id, "unknown-api");
      g_autofree char *value = g_strdup_printf ("\"unknown plugin api: %s\"",
                                                api);
      web_respond (state, session, browser, reply_id, FALSE, value);
    }
}

/* respond()'s routing half: find the browser the reply belongs to and send
 * the command. A reply nobody recorded is dropped. */
void
_chromium_plugin_web_respond (ChromiumPluginManager *manager, int reply_id,
                              gboolean ok, const char *value_json)
{
  WebState *state;
  ChromiumSession *session;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));

  state = web_state_for (manager, FALSE);
  if (!state)
    return;
  session = _chromium_plugin_manager_get_session (manager);
  /* respond() carries no browser: the oldest call with this id answers. */
  web_respond (state, session, 0, reply_id, ok, value_json);
}

/* uninstall()'s cleanup half: the plugin's storage table (its file is being
 * removed — a future install of the same id must not inherit it), its menu
 * rows, and its action listeners. */
void
_chromium_plugin_web_forget (ChromiumPluginManager *manager, const char *id)
{
  WebState *state;
  ChromiumSession *session;
  GHashTableIter iter;
  gpointer key, value;
  guint i;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));

  state = web_state_for (manager, FALSE);
  if (!state)
    return;

  session = _chromium_plugin_manager_get_session (manager);

  g_hash_table_remove (state->storage, id);

  /* Walk a snapshot: the removals below run while the iterator would still
     be standing in the table. */
  g_autoptr (GPtrArray) slots = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, state->web_slots);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (g_strcmp0 (value, id) == 0)
      g_ptr_array_add (slots, key);
  for (i = 0; i < slots->len; i++)
    {
      int slot = GPOINTER_TO_INT (g_ptr_array_index (slots, i));

      g_hash_table_remove (state->web_slots, GINT_TO_POINTER (slot));
      if (session)
        {
          g_autoptr (JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "cmd");
          json_builder_add_string_value (builder, "menu-remove-user-item");
          json_builder_set_member_name (builder, "slot");
          json_builder_add_int_value (builder, slot);
          json_builder_end_object (builder);
          _chromium_session_send_plugin_command (session, builder);
        }
    }

  g_hash_table_remove (state->action_browsers, id);
}

/* The helper's answer to a tabs.query. @request_id is what the query sent; a
 * lookup that finds nothing means the query is already gone. */
void
chromium_plugin_web_handle_browsers (ChromiumPluginManager *manager,
                                     int request_id, JsonArray *browsers)
{
  WebState *state;
  ChromiumSession *session;
  TabsQuery *query;
  GString *out;
  gboolean first = TRUE;
  int reply_id, owner;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));

  state = web_state_for (manager, FALSE);
  if (!state)
    return;
  query = g_hash_table_lookup (state->tabs_queries,
                               GINT_TO_POINTER (request_id));
  if (!query)
    return;
  /* The removal destroys the value: read what this answer needs first. */
  reply_id = query->reply_id;
  owner = query->browser;
  g_hash_table_remove (state->tabs_queries, GINT_TO_POINTER (request_id));

  session = _chromium_plugin_manager_get_session (manager);
  out = g_string_new ("[");
  for (guint i = 0; browsers && i < json_array_get_length (browsers); i++)
    {
      JsonObject *browser = json_array_get_object_element (browsers, i);
      const char *url = member_string (browser, "url");
      const char *title = member_string (browser, "title");
      JsonNode *node;
      gint64 id = 0;
      double zoom = 0.0;
      gboolean active = FALSE, windowed = FALSE, muted = FALSE;
      g_autofree char *url_q = json_quoted (url);
      g_autofree char *title_q = json_quoted (title);

      node = json_object_has_member (browser, "id") ?
        json_object_get_member (browser, "id") : NULL;
      if (node && JSON_NODE_HOLDS_VALUE (node))
        id = json_node_get_int (node);
      node = json_object_has_member (browser, "active") ?
        json_object_get_member (browser, "active") : NULL;
      if (node && JSON_NODE_HOLDS_VALUE (node))
        active = json_node_get_boolean (node);
      node = json_object_has_member (browser, "windowed") ?
        json_object_get_member (browser, "windowed") : NULL;
      if (node && JSON_NODE_HOLDS_VALUE (node))
        windowed = json_node_get_boolean (node);
      node = json_object_has_member (browser, "muted") ?
        json_object_get_member (browser, "muted") : NULL;
      if (node && JSON_NODE_HOLDS_VALUE (node))
        muted = json_node_get_boolean (node);
      node = json_object_has_member (browser, "zoom") ?
        json_object_get_member (browser, "zoom") : NULL;
      if (node && JSON_NODE_HOLDS_VALUE (node))
        zoom = json_node_get_double (node);

      /* The helper read muted and zoom off the CefBrowserHost directly, so
       * they are facts, not cached state; windowed is how the browser was
       * created. pinned and discarded stay out: there is no source for them,
       * and inventing values would be lying. */
      g_string_append_printf (out,
                              "%s{\"id\":%" G_GINT64_FORMAT ",\"url\":%s,"
                              "\"title\":%s,\"active\":%s,"
                              "\"windowed\":%s,\"muted\":%s,"
                              "\"zoom\":%.4f}",
                              first ? "" : ",", id, url_q, title_q,
                              active ? "true" : "false",
                              windowed ? "true" : "false",
                              muted ? "true" : "false",
                              zoom);
      first = FALSE;
    }
  g_string_append_c (out, ']');

  web_respond (state, session, owner, reply_id, TRUE, out->str);
  g_string_free (out, TRUE);
}

gboolean
chromium_plugin_web_deliver_menu (ChromiumPluginManager *manager,
                                  int browser, int slot, JsonObject *params)
{
  WebState *state;
  ChromiumSession *session;
  g_autofree char *params_json = NULL;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager), FALSE);

  state = web_state_for (manager, FALSE);
  if (!state ||
      !g_hash_table_contains (state->web_slots, GINT_TO_POINTER (slot)))
    return FALSE;

  session = _chromium_plugin_manager_get_session (manager);
  if (!session)
    return TRUE;

  if (params)
    {
      g_autoptr (JsonGenerator) generator = json_generator_new ();
      g_autoptr (JsonNode) root = json_node_new (JSON_NODE_OBJECT);

      json_node_set_object (root, params);
      json_generator_set_root (generator, root);
      params_json = json_generator_to_data (generator, NULL);
    }
  else
    params_json = g_strdup ("null");

  {
    g_autofree char *code =
      g_strdup_printf ("window.__cefnestDispatchMenu&&"
                       "__cefnestDispatchMenu(%d,%s);", slot, params_json);

    _chromium_session_run_javascript_in_browser (session, browser, code);
  }
  return TRUE;
}

void
chromium_plugin_web_deliver_action (ChromiumPluginManager *manager,
                                    const char *id)
{
  WebState *state;
  ChromiumSession *session;
  GArray *browsers;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));

  state = web_state_for (manager, FALSE);
  if (!state)
    return;

  browsers = g_hash_table_lookup (state->action_browsers, id);
  if (!browsers || !browsers->len)
    return;

  session = _chromium_plugin_manager_get_session (manager);
  if (!session)
    return;

  for (guint i = 0; i < browsers->len; i++)
    _chromium_session_run_javascript_in_browser (
      session, g_array_index (browsers, int, i),
      "window.__cefnestActionClicked&&__cefnestActionClicked();");
}
