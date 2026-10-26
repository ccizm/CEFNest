#define G_LOG_DOMAIN "chromium-plugin"

#include <string.h>

#include <gmodule.h>

#include "chromium-plugin.h"
#include "chromium-plugin-private.h"

/* ------------------------------------------------------------------ *
 * The native frontend: one .so per enabled plugin, loaded into this  *
 * process                                                            *
 *                                                                    *
 * A native plugin owns nothing it does not hand over through the     *
 * registry. That is the whole unload story: every verb records what  *
 * it did — menu rows here, handler ids on the session, the widget    *
 * the plugin set — and unload walks the record backwards before      *
 * g_module_close() unmaps the code those callbacks live in. A plugin *
 * that worked outside this ledger would leave a dangling handler on  *
 * the session, and the next emission would be a call into a page     *
 * that is no longer mapped.                                          *
 *                                                                    *
 * The load is deliberately not GTypeModule: the plugin registers     *
 * callbacks, never GTypes, and a type that reached the type table    *
 * could not be taken back out.                                       *
 * ------------------------------------------------------------------ */

/* What the loader hands the plugin, and what every verb below receives back.
 * The plugin never sees inside it. */
struct _ChromiumPluginRegistry {
  /* The record this registry belongs to. The registry is created before the
   * plugin's register function runs, so verbs called during registration
   * already know who is asking. */
  gpointer owner;
};

typedef void (*PluginRegisterFunc) (ChromiumPluginRegistry *registry,
                                    guint api_version);

typedef struct {
  int slot;
  char *label;
  GCallback callback;
  gpointer user_data;
  /* Set once the row's add command actually reached a running helper.
     announce() re-sends only what never did, which keeps a load that happens
     at ::ready from announcing the same row twice. */
  gboolean announced;
} NativeMenuItem;

typedef struct {
  gulong handler_id;             /* 0 for a registry-delivered subscription */
  char *detailed_signal;
  GCallback callback;
  gpointer user_data;
} NativeSubscription;

typedef struct {
  char *id;
  GModule *module;
  ChromiumPlugin *plugin;        /* strong while loaded */
  ChromiumSession *session;      /* borrowed: the session drives sync */
  GPtrArray *menu_items;         /* NativeMenuItem* */
  GPtrArray *subscriptions;      /* NativeSubscription* */
  GtkWidget *action;             /* held while the plugin is loaded */
  GHashTable *storage;           /* key -> JSON text, in-memory this version */
} NativePlugin;

/* Loaded plugins, per manager. The session is the only caller of the sync
 * entry points, so a manager without an entry here has no native plugins. */
static GHashTable *g_native_loaded;

static void
native_item_free (gpointer data)
{
  NativeMenuItem *item = data;

  g_clear_pointer (&item->label, g_free);
  g_slice_free (NativeMenuItem, item);
}

static void
native_subscription_free (gpointer data)
{
  NativeSubscription *subscription = data;

  g_clear_pointer (&subscription->detailed_signal, g_free);
  g_slice_free (NativeSubscription, subscription);
}

/* A widget the plugin handed over is held with a ref, not a parent — the host
 * that shows it parents it itself. */
static void
action_release (GtkWidget **action)
{
  if (*action)
    {
      g_object_unref (*action);
      *action = NULL;
    }
}

static void
native_free (gpointer data)
{
  NativePlugin *native = data;

  /* Everything the plugin registered is already undone by the unload paths;
     this is the last look at the record, not the place to do it. */
  g_clear_pointer (&native->id, g_free);
  g_clear_pointer (&native->menu_items, g_ptr_array_unref);
  g_clear_pointer (&native->subscriptions, g_ptr_array_unref);
  g_clear_pointer (&native->storage, g_hash_table_unref);
  action_release (&native->action);
  g_clear_object (&native->plugin);
  g_clear_pointer (&native->module, g_module_close);
  g_free (native);
}

static GPtrArray *
loaded_array (ChromiumPluginManager *manager, gboolean create)
{
  GPtrArray *array;

  if (!g_native_loaded)
    {
      if (!create)
        return NULL;
      g_native_loaded = g_hash_table_new_full (NULL, NULL, NULL,
                                               (GDestroyNotify)
                                               g_ptr_array_unref);
    }

  array = g_hash_table_lookup (g_native_loaded, manager);
  if (!array && create)
    {
      array = g_ptr_array_new_with_free_func (native_free);
      g_hash_table_insert (g_native_loaded, manager, array);
    }
  return array;
}

static NativePlugin *
loaded_find (GPtrArray *loaded, const char *id)
{
  for (guint i = 0; i < loaded->len; i++)
    {
      NativePlugin *native = g_ptr_array_index (loaded, i);

      if (g_strcmp0 (native->id, id) == 0)
        return native;
    }
  return NULL;
}

static void
native_disconnect_all (NativePlugin *native)
{
  for (guint i = 0; i < native->subscriptions->len; i++)
    {
      NativeSubscription *subscription =
        g_ptr_array_index (native->subscriptions, i);

      if (subscription->handler_id != 0)
        g_signal_handler_disconnect (native->session,
                                     subscription->handler_id);
    }
  g_ptr_array_set_size (native->subscriptions, 0);
}

/* One menu-add-user-item command. The helper keeps its table keyed by slot,
 * so a re-send replaces rather than duplicates. Sent through the session,
 * which drops it while no helper runs — announce() at ::ready is what makes
 * the rows appear for a plugin enabled before start(). */
static void
native_send_menu_item (NativePlugin *native, NativeMenuItem *item)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "cmd");
  json_builder_add_string_value (builder, "menu-add-user-item");
  json_builder_set_member_name (builder, "slot");
  json_builder_add_int_value (builder, item->slot);
  json_builder_set_member_name (builder, "label");
  json_builder_add_string_value (builder, item->label);
  json_builder_end_object (builder);
  if (_chromium_session_send_plugin_command (native->session, builder))
    item->announced = TRUE;
}

/* ------------------------------------------------------------------ *
 * The five registry verbs — the whole ABI                            *
 * ------------------------------------------------------------------ */

void
chromium_plugin_registry_add_menu_item (ChromiumPluginRegistry *registry,
                                        const char *label, int slot,
                                        GCallback callback,
                                        gpointer user_data)
{
  NativePlugin *native;
  NativeMenuItem *item;

  g_return_if_fail (registry != NULL);
  g_return_if_fail (label != NULL && *label);
  g_return_if_fail (slot >= 0 && slot <= 199);
  g_return_if_fail (callback != NULL);

  native = registry->owner;
  item = g_slice_new0 (NativeMenuItem);
  item->slot = slot;
  item->label = g_strdup (label);
  item->callback = callback;
  item->user_data = user_data;

  /* The helper keys its table by slot, so a second row on the same slot from
     the same plugin is a replacement here too — the dropped one must not come
     back on the next announce. */
  for (guint i = 0; i < native->menu_items->len; i++)
    {
      NativeMenuItem *previous = g_ptr_array_index (native->menu_items, i);

      if (previous->slot == slot)
        {
          g_ptr_array_remove_index (native->menu_items, i);
          break;
        }
    }
  g_ptr_array_add (native->menu_items, item);

  native_send_menu_item (native, item);
}

void
chromium_plugin_registry_subscribe (ChromiumPluginRegistry *registry,
                                    const char *detailed_signal,
                                    GCallback callback, gpointer user_data)
{
  NativePlugin *native;
  NativeSubscription *subscription;

  g_return_if_fail (registry != NULL);
  g_return_if_fail (detailed_signal != NULL && *detailed_signal);
  g_return_if_fail (callback != NULL);

  native = registry->owner;
  subscription = g_slice_new0 (NativeSubscription);
  subscription->detailed_signal = g_strdup (detailed_signal);
  subscription->callback = callback;
  subscription->user_data = user_data;

  /* "plugin-storage" is not a session signal; it is how the registry hands
     storage_get() results back. A g_signal_connect() on that name would be a
     critical about a signal that does not exist. */
  if (g_strcmp0 (detailed_signal, "plugin-storage") == 0)
    {
      subscription->handler_id = 0;
    }
  else
    {
      subscription->handler_id =
        g_signal_connect (native->session, detailed_signal, callback,
                          user_data);
      if (subscription->handler_id == 0)
        {
          /* A signal name that resolves to nothing is a plugin bug worth
             stopping on: the alternative is a plugin that silently never
             hears the one thing it asked to hear. */
          g_error ("plugin \"%s\" subscribed to \"%s\", which is not a "
                   "session signal", native->id, detailed_signal);
        }
    }

  g_ptr_array_add (native->subscriptions, subscription);
}

void
chromium_plugin_registry_set_action (ChromiumPluginRegistry *registry,
                                     GtkWidget *button)
{
  NativePlugin *native;

  g_return_if_fail (registry != NULL);
  g_return_if_fail (GTK_IS_WIDGET (button));

  native = registry->owner;
  /* One action per plugin: a second set replaces the first, which is also
     how a plugin hands over a rebuilt widget without leaking the old one. */
  action_release (&native->action);
  native->action = g_object_ref (button);
}

void
chromium_plugin_registry_storage_get (ChromiumPluginRegistry *registry,
                                      const char *key)
{
  NativePlugin *native;
  const char *json = NULL;

  g_return_if_fail (registry != NULL);
  g_return_if_fail (key != NULL);

  native = registry->owner;
  json = g_hash_table_lookup (native->storage, key);

  for (guint i = 0; i < native->subscriptions->len; i++)
    {
      NativeSubscription *subscription =
        g_ptr_array_index (native->subscriptions, i);

      if (subscription->handler_id != 0 ||
          g_strcmp0 (subscription->detailed_signal, "plugin-storage") != 0)
        continue;

      ((void (*) (const char *, const char *, gpointer))
         subscription->callback) (key, json, subscription->user_data);
    }
}

void
chromium_plugin_registry_storage_set (ChromiumPluginRegistry *registry,
                                      const char *key, const char *json)
{
  NativePlugin *native;

  g_return_if_fail (registry != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (json != NULL);

  native = registry->owner;
  g_hash_table_replace (native->storage, g_strdup (key), g_strdup (json));
}

/* ------------------------------------------------------------------ *
 * Load, unload, dispatch                                             *
 * ------------------------------------------------------------------ */

/* A load failure is not a refusal — the manifest is fine, the code behind it
   is not reachable right now — so the row stays enabled and the reason goes
   out on ::plugin-error for the host's row to show. */
static void
native_load_failed (ChromiumPluginManager *manager, ChromiumPlugin *plugin,
                    const char *token, const char *detail)
{
  g_autofree char *message = NULL;

  message = g_strdup_printf ("%s%s", token, detail ? detail : "");
  g_signal_emit_by_name (manager, "plugin-error",
                         chromium_plugin_get_id (plugin), message);
}

static void
native_load (ChromiumPluginManager *manager, ChromiumSession *session,
             ChromiumPlugin *plugin, GPtrArray *loaded)
{
  g_autofree char *path = NULL;
  NativePlugin *native;
  ChromiumPluginRegistry *registry;
  PluginRegisterFunc register_func = NULL;

  path = g_build_filename (chromium_plugin_get_root (plugin),
                           _chromium_plugin_get_library (plugin), NULL);
  g_message ("plugin: native \"%s\" loading %s",
             chromium_plugin_get_id (plugin), path);

  native = g_new0 (NativePlugin, 1);
  native->id = g_strdup (chromium_plugin_get_id (plugin));
  native->session = session;
  native->plugin = g_object_ref (plugin);
  native->menu_items = g_ptr_array_new_with_free_func (native_item_free);
  native->subscriptions =
    g_ptr_array_new_with_free_func (native_subscription_free);
  native->storage = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);

  native->module = g_module_open (path, G_MODULE_BIND_LAZY |
                                          G_MODULE_BIND_LOCAL);
  if (!native->module)
    {
      g_autofree char *detail = g_strdup_printf (": %s", g_module_error ());

      native_free (native);
      native_load_failed (manager, plugin, "library-open-failed", detail);
      return;
    }

  if (!g_module_symbol (native->module,
                        _chromium_plugin_get_symbol (plugin),
                        (gpointer *) &register_func) || !register_func)
    {
      g_autofree char *detail =
        g_strdup_printf (": %s", _chromium_plugin_get_symbol (plugin));

      native_free (native);
      native_load_failed (manager, plugin, "library-symbol-missing", detail);
      return;
    }

  /* In the array from here on: the unload paths find it by the same id, and
     native_free() runs whenever the array drops it. */
  g_ptr_array_add (loaded, native);

  registry = g_new0 (ChromiumPluginRegistry, 1);
  registry->owner = native;

  /* The verbs the registration calls already know their owner. What the
     plugin does not register is not undone on unload — that is the deal the
     header states. */
  register_func (registry, CEFNEST_PLUGIN_API_VERSION);
  g_free (registry);
}

/* One plugin's rows off the helper's menu, then its connections, then — on
   the caller's side — the record and the code itself. @tell_helper is FALSE
   when the channel is already gone. */
static void
native_unload_one (NativePlugin *native, gboolean tell_helper)
{
  g_message ("plugin: native \"%s\" unloading (%u menu rows, %u connections)",
             native->id, native->menu_items->len,
             native->subscriptions->len);

  if (tell_helper)
    {
      for (guint i = 0; i < native->menu_items->len; i++)
        {
          NativeMenuItem *item = g_ptr_array_index (native->menu_items, i);
          g_autoptr (JsonBuilder) builder = json_builder_new ();

          json_builder_begin_object (builder);
          json_builder_set_member_name (builder, "cmd");
          json_builder_add_string_value (builder, "menu-remove-user-item");
          json_builder_set_member_name (builder, "slot");
          json_builder_add_int_value (builder, item->slot);
          json_builder_end_object (builder);
          _chromium_session_send_plugin_command (native->session, builder);
        }
    }

  /* Connections go before the close: after g_module_close() the handlers are
     pages that are no longer mapped, and any emission the unload path itself
     triggers would call into one. */
  native_disconnect_all (native);
  g_ptr_array_set_size (native->menu_items, 0);
  action_release (&native->action);
}

static void
native_unload (ChromiumPluginManager *manager, NativePlugin *native,
               gboolean tell_helper)
{
  GPtrArray *loaded = loaded_array (manager, FALSE);

  native_unload_one (native, tell_helper);
  /* g_ptr_array_remove() runs the element free func: the record goes, and
     g_module_close() with it. */
  if (loaded)
    g_ptr_array_remove (loaded, native);
}

void
chromium_plugin_native_sync (ChromiumPluginManager *manager,
                             ChromiumSession *session)
{
  GPtrArray *loaded;
  GListModel *model;
  guint n, i;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (manager));
  g_return_if_fail (CHROMIUM_IS_SESSION (session));

  loaded = loaded_array (manager, TRUE);

  /* Out first: a plugin that is gone from the model, no longer enabled, or
     no longer native unloads before anything below could call into it. */
  for (i = loaded->len; i > 0; i--)
    {
      NativePlugin *native = g_ptr_array_index (loaded, i - 1);
      g_autoptr (ChromiumPlugin) plugin =
        chromium_plugin_manager_lookup (manager, native->id);
      gboolean keep = plugin != NULL &&
        chromium_plugin_get_state (plugin) == CHROMIUM_PLUGIN_STATE_ENABLED &&
        g_strcmp0 (chromium_plugin_get_kind (plugin), "native") == 0;

      if (!keep)
        native_unload (manager, native, TRUE);
    }

  model = chromium_plugin_manager_get_plugins (manager);
  n = g_list_model_get_n_items (model);
  for (i = 0; i < n; i++)
    {
      ChromiumPlugin *plugin = g_list_model_get_item (model, i);

      if (chromium_plugin_get_state (plugin) ==
            CHROMIUM_PLUGIN_STATE_ENABLED &&
          g_strcmp0 (chromium_plugin_get_kind (plugin), "native") == 0 &&
          !loaded_find (loaded, chromium_plugin_get_id (plugin)))
        native_load (manager, session, plugin, loaded);

      g_object_unref (plugin);
    }
}

void
chromium_plugin_native_announce (ChromiumPluginManager *manager)
{
  GPtrArray *loaded = loaded_array (manager, FALSE);

  if (!loaded)
    return;

  /* The helper's menu table starts empty after every start(), so the
     announce is the only thing that puts the rows back — but only the rows
     whose own send was dropped for having nowhere to go. */
  for (guint i = 0; i < loaded->len; i++)
    {
      NativePlugin *native = g_ptr_array_index (loaded, i);

      for (guint j = 0; j < native->menu_items->len; j++)
        {
          NativeMenuItem *item = g_ptr_array_index (native->menu_items, j);

          if (!item->announced)
            native_send_menu_item (native, item);
        }
    }
}

void
chromium_plugin_native_unload_all (ChromiumPluginManager *manager)
{
  GPtrArray *loaded;

  g_return_if_fail (manager == NULL || CHROMIUM_IS_PLUGIN_MANAGER (manager));

  if (!manager || !g_native_loaded)
    return;

  loaded = g_hash_table_lookup (g_native_loaded, manager);
  if (!loaded)
    return;

  /* No channel: the helper that owned the rows is gone, and a command sent
     now would go nowhere or worse. */
  for (guint i = 0; i < loaded->len; i++)
    native_unload_one (g_ptr_array_index (loaded, i), FALSE);
  g_hash_table_remove (g_native_loaded, manager);
}

void
chromium_plugin_native_dispatch_menu (ChromiumPluginManager *manager,
                                      int slot, JsonObject *params)
{
  GPtrArray *loaded;
  NativeMenuItem *item = NULL;
  g_autofree char *params_json = NULL;

  g_return_if_fail (manager == NULL || CHROMIUM_IS_PLUGIN_MANAGER (manager));

  if (!manager || !g_native_loaded)
    return;

  loaded = g_hash_table_lookup (g_native_loaded, manager);
  if (!loaded)
    return;

  /* Last match wins, the same rule the helper's own table follows: the row
     the user clicked belongs to whoever registered the slot last. */
  for (guint i = 0; i < loaded->len; i++)
    {
      NativePlugin *native = g_ptr_array_index (loaded, i);

      for (guint j = 0; j < native->menu_items->len; j++)
        {
          NativeMenuItem *candidate =
            g_ptr_array_index (native->menu_items, j);

          if (candidate->slot == slot)
            item = candidate;
        }
    }

  if (!item)
    {
      g_debug ("plugin: menu slot %d has no loaded native behind it", slot);
      return;
    }

  if (params)
    {
      g_autoptr (JsonGenerator) generator = json_generator_new ();
      g_autoptr (JsonNode) root = json_node_new (JSON_NODE_OBJECT);

      /* Borrowed on purpose: the serialization happens now, and the params
         object belongs to the event that carried it. */
      json_node_set_object (root, params);
      json_generator_set_root (generator, root);
      params_json = json_generator_to_data (generator, NULL);
    }
  else
    params_json = g_strdup ("null");

  ((ChromiumPluginMenuCallback) item->callback) (slot, params_json,
                                                 item->user_data);
}
