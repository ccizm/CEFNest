#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* What a plugin is, as the host's UI needs to know it: a directory on disk with
 * a manifest in it, its enabled state, and the permissions the host has granted.
 *
 * This object is the model half of plugin support; the runtime half (content
 * scripts, the native registry, the chrome.* shim) drives it and is described
 * in docs/plugins.md.
 *
 * Nothing in this file draws. An options page and a popup page are, to the
 * component, one URI plus a size — see chromium_plugin_get_popup_uri() — and
 * which window, popover or notebook tab they end up in is the embedding
 * application's decision, because no two applications can agree on it. */

/* The origin every plugin resource lives on. The scheme is registered inside
 * the helper process, so these URIs are for handing to a browser or for a page
 * to resolve — this process cannot fetch them. A host that wants an icon for
 * its own list has to read the file: chromium_plugin_icon_path(). */
#define CHROMIUM_EXTENSION_URI_PREFIX "cefnest-extension://"
/* The file scan() looks for inside each candidate directory. */
#define CHROMIUM_PLUGIN_MANIFEST_NAME "cefnest-plugin.json"

/* The popup size the manifest leaves unspecified, and the box a specified one
 * is clamped into. The clamp is what keeps a page that asks for 4000x4000 from
 * turning the host's window into a scroll; exceeding it is not an error and
 * still gives a working plugin, so it is reported as ::plugin-error
 * "popup-size-clamped" and the clamped number is what the getters return. */
#define CHROMIUM_PLUGIN_POPUP_DEFAULT_WIDTH  320
#define CHROMIUM_PLUGIN_POPUP_DEFAULT_HEIGHT 480
#define CHROMIUM_PLUGIN_POPUP_MAX_WIDTH     800
#define CHROMIUM_PLUGIN_POPUP_MAX_HEIGHT    600

typedef enum {
  /* The directory was read and its manifest refused. The object still says what
   * was on disk, error-message is the reason, and enable() can never succeed:
   * a refused package stays in the list so a management page can show the
   * person why the extension they just unpacked does nothing. */
  CHROMIUM_PLUGIN_STATE_REFUSED,
  /* A valid manifest that nobody has enabled. */
  CHROMIUM_PLUGIN_STATE_SCANNED,
  /* Enabled, which for this model means every permission the manifest declares
   * has been granted. The runtime step that makes the page see it is P1. */
  CHROMIUM_PLUGIN_STATE_ENABLED
} ChromiumPluginState;

#define CHROMIUM_TYPE_PLUGIN_STATE (chromium_plugin_state_get_type ())
GType            chromium_plugin_state_get_type (void);

#define CHROMIUM_TYPE_PLUGIN (chromium_plugin_get_type ())
G_DECLARE_FINAL_TYPE (ChromiumPlugin, chromium_plugin, CHROMIUM, PLUGIN, GObject)

/* Every getter below returns strings owned by the plugin and valid while it is,
 * which is what lets a GtkListItemFactory bind them directly. */
const char       *chromium_plugin_get_id           (ChromiumPlugin *self);
const char       *chromium_plugin_get_name         (ChromiumPlugin *self);
const char       *chromium_plugin_get_version      (ChromiumPlugin *self);
const char       *chromium_plugin_get_description  (ChromiumPlugin *self);
/* The absolute directory the manifest was read from. */
const char       *chromium_plugin_get_root         (ChromiumPlugin *self);
/* "web" or "native", straight off the manifest's cefnest.type — NULL for a
 * refused package whose type could not be read. */
const char       *chromium_plugin_get_kind         (ChromiumPlugin *self);
ChromiumPluginState chromium_plugin_get_state       (ChromiumPlugin *self);
const char       *chromium_plugin_get_error_message (ChromiumPlugin *self);
/* The manifest's "permissions" list, and the subset the host has granted. Both
 * are %NULL-terminated arrays owned by the plugin; the first is never %NULL, an
 * empty manifest list is an array with no entries in it. */
const char *const *chromium_plugin_get_permissions (ChromiumPlugin *self);
const char *const *chromium_plugin_get_granted_permissions (ChromiumPlugin *self);

/* The two extension pages, already resolved to URIs and %NULL when the manifest
 * asks for neither. Nothing is opened by them and nothing schedules them: the
 * host gets ::plugin-action from its own button, reads these, and decides. */
const char       *chromium_plugin_get_popup_uri    (ChromiumPlugin *self);
const char       *chromium_plugin_get_options_uri  (ChromiumPlugin *self);
void              chromium_plugin_get_popup_size   (ChromiumPlugin *self,
                                                    int *width, int *height);
/* options_ui.open_in_tab, which is FALSE unless the manifest said otherwise. A
 * host that shows the options page inline can ignore it; it is the plugin
 * author's hint that the page wants a window of its own. */
gboolean          chromium_plugin_get_options_open_in_tab (ChromiumPlugin *self);
/* The manifest's background.service_worker, promoted to a resident page: the
 * URI of the hidden browser the session keeps running for as long as this
 * plugin is enabled, or %NULL when the manifest names none. The session
 * creates and closes that browser itself — a host that wants its own
 * always-on page uses chromium_session_new_background_browser() directly. */
const char       *chromium_plugin_get_background_uri (ChromiumPlugin *self);

/* A path from the manifest's "icons" map, as an absolute file in the plugin's
 * own directory, or %NULL when the map has no such key. The key is the string
 * the manifest uses ("16", "128", "scalable"), not a size, because picking the
 * best size for a widget is the painter's business and not the model's.
 * Caller owns the string. */
char             *chromium_plugin_icon_path        (ChromiumPlugin *self,
                                                    const char *key);
/* cefnest-extension://<id>/<relative_path>, or %NULL with @error set when the
 * path is not one this plugin can be asked for. Rules, all of them refused:
 * absolute, empty, containing a "." or ".." segment, containing a backslash or
 * a percent sign. The last one is because a caller passes a filesystem-relative
 * path here, not a half-built URI: percent-encoding is what let "..%2F" through
 * URL normalisation into the handler (measured 2026-10-25; docs/plugins.md),
 * and a builder that accepted an encoded path would be a second door with a
 * weaker lock than the one the helper keeps.
 *
 * The check here is the same one the helper re-runs on every request against
 * realpath; neither side assumes the other did it. Caller owns the string. */
char             *chromium_plugin_build_uri        (ChromiumPlugin *self,
                                                    const char *relative_path,
                                                    GError **error);
/* enabled as a property is the host's switch: writing it runs exactly the
 * enable()/revoke() machine below, through the manager that owns this plugin.
 * A plugin kept alive past its manager has nothing to enable itself through, so
 * the write does nothing and state stays as it was. */
gboolean          chromium_plugin_get_enabled      (ChromiumPlugin *self);
void              chromium_plugin_set_enabled      (ChromiumPlugin *self,
                                                    gboolean enabled);

#define CHROMIUM_TYPE_PLUGIN_MANAGER (chromium_plugin_manager_get_type ())
G_DECLARE_FINAL_TYPE (ChromiumPluginManager, chromium_plugin_manager,
                      CHROMIUM, PLUGIN_MANAGER, GObject)

ChromiumPluginManager *chromium_plugin_manager_new (void);

/* The list, as a GListModel bound straight into GtkListBox, GtkGridView or a
 * GtkColumnView, ordered by id. Owned by the manager; plugins live in it.
 * ::items-changed is the same news as ::changed, so a host binding a model
 * needs no signal of its own. */
GListModel       *chromium_plugin_manager_get_plugins  (ChromiumPluginManager *self);
/* A refused plugin is found by name too — enable() has to be able to say why it
 * said no. The first row with that id wins, which is the one scan() kept when
 * two search paths held the same name. Caller owns the reference. */
ChromiumPlugin   *chromium_plugin_manager_lookup       (ChromiumPluginManager *self,
                                                        const char *id);
/* A directory to scan for plugins, validated now: absolute, existing, readable.
 * Failing to accept a search path is a configuration mistake, and the place to
 * say so is where it was made rather than several scans later. */
gboolean          chromium_plugin_manager_add_search_path (ChromiumPluginManager *self,
                                                           const char *dir,
                                                           GError **error);
/* Re-read every search path and rebuild the model. Enabled state and grants
 * carry over by id, so a rescan after an unpack does not silently un-grant the
 * plugin the user just turned on. Plugins whose manifest now fails come back
 * refused, and keeping their grants would be granting them to a package that
 * no longer describes itself.
 *
 * Returns FALSE only when a path could not be read at all, in which case the
 * model is still refreshed from the paths that could be. Per-plugin failures
 * are not scan failures: they are refused entries plus ::plugin-error. */
gboolean          chromium_plugin_manager_scan          (ChromiumPluginManager *self,
                                                         GError **error);

/* All four verbs take an id, because the caller is a UI that picked one out of
 * the model, and an unknown id has to be distinguishable from a refusal —
 * "nothing was asked" versus "that was asked and could not be had".
 *
 * enable() refuses when the manifest was refused, and when a declared
 * permission has not been granted. In that case it first emits ::grant-request
 * (id, permissions, request). A host that answers the emission by granting (or
 * by calling accept() on the request) gets an enabled plugin out of this same
 * call; one that answers later leaves the enable parked — enable() returns
 * FALSE with #G_IO_ERROR_PENDING — until the request is settled, and the
 * manager's ::changed is the news then. No handler connected keeps the old
 * immediate rule: the table decides, and an ungranted request is a deny.
 * Nothing here blocks, and the permission table remains the only source of
 * truth.
 *
 * revoke() takes a plugin back to SCANNED when the permission it loses was one
 * the manifest declares: leaving it enabled would mean a plugin running with a
 * permission it no longer has. */
gboolean          chromium_plugin_manager_enable        (ChromiumPluginManager *self,
                                                         const char *id,
                                                         GError **error);
void              chromium_plugin_manager_disable       (ChromiumPluginManager *self,
                                                         const char *id);
gboolean          chromium_plugin_manager_grant         (ChromiumPluginManager *self,
                                                         const char *id,
                                                         const char *permission,
                                                         GError **error);
gboolean          chromium_plugin_manager_revoke        (ChromiumPluginManager *self,
                                                         const char *id,
                                                         const char *permission,
                                                         GError **error);

/* The answer to a ::plugin-message. @value is the response as one JSON text
 * ("null" when there is nothing to say); @ok FALSE with an explanation in
 * @value is how an API call is refused. A reply_id nothing is waiting on is
 * dropped — the page that asked is gone.
 *
 * respond() is what makes ::plugin-message a two-way door: a handler that
 * returns TRUE owns the call and answers here; the component's own API
 * handlers (storage, menus, tabs, runtime) answer everything nobody
 * claimed. */
void               chromium_plugin_manager_respond       (ChromiumPluginManager *self,
                                                          int reply_id,
                                                          gboolean ok,
                                                          const char *value);

/* The host's action button for @id was pressed. ::plugin-action goes out to
 * the host (whose popup policy is its own — read popup-uri and size off the
 * plugin), and the plugin's own chrome.action.onClicked listeners, if a page
 * registered one, hear it too. A plugin without an action is a no-op. */
void               chromium_plugin_manager_activate_action (ChromiumPluginManager *self,
                                                            const char *id);

/* ------------------------------------------------------------------ *
 * Permission requests                                                *
 *                                                                    *
 * A parked enable. When ::grant-request is emitted with a request,   *
 * the enable that caused it waits until one of the three verbs below *
 * settles the request — or until the request is invalidated, which  *
 * makes all three safe no-ops. The request is refcounted: a host    *
 * that keeps it across the dialog answering it holds a reference.   *
 *                                                                    *
 * The permission table stays the only source of truth. accept()      *
 * grants the pending set and then completes the enable; deny()      *
 * leaves the table alone and fails it; settle() decides from         *
 * whatever the host granted (or revoked) meanwhile, which is the     *
 * per-permission path: grant() the ones the person ticked, then      *
 * settle().                                                          *
 *                                                                    *
 * A request is invalidated when its plugin is disabled, when a       *
 * rescan drops its row, and when the manager is disposed. Nothing    *
 * is emitted for an invalidated request — the row it belonged to is  *
 * gone, and a request nobody can answer is not news.                 *
 * ------------------------------------------------------------------ */

#define CHROMIUM_TYPE_PLUGIN_PERMISSION_REQUEST \
  (chromium_plugin_permission_request_get_type ())
G_DECLARE_FINAL_TYPE (ChromiumPluginPermissionRequest,
                      chromium_plugin_permission_request,
                      CHROMIUM, PLUGIN_PERMISSION_REQUEST, GObject)

void               chromium_plugin_permission_request_accept (ChromiumPluginPermissionRequest *self);
void               chromium_plugin_permission_request_deny   (ChromiumPluginPermissionRequest *self);
void               chromium_plugin_permission_request_settle (ChromiumPluginPermissionRequest *self);
/* The plugin the request belongs to and the pending set as it was when the
 * request was made — borrowed, valid while the request is. */
const char       *chromium_plugin_permission_request_get_id (ChromiumPluginPermissionRequest *self);
const char *const *chromium_plugin_permission_request_get_permissions (ChromiumPluginPermissionRequest *self);

/* Installing is disk work and unzipping, so it runs on a GTask thread and
 * answers through install_finish(). @package is a .zip or a .crx file (a CRX3
 * shell is skipped; there is no signature scheme behind it — this component
 * has none, see docs/plugins.md), @id is the directory the package installs
 * under and therefore the plugin's id: [a-z0-9_-]{1,64}. Installs go into the
 * manager's install root, which a session without a data directory does not
 * have — the error says so. A refused manifest still installs: it lands in
 * the model as state=refused with the reason, the same as anything scanned
 * from disk. An @id that already exists refuses; uninstall it first. */
void               chromium_plugin_manager_install_async (ChromiumPluginManager *self,
                                                          const char *package,
                                                          const char *id,
                                                          int io_priority,
                                                          GCancellable *cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer user_data);
gboolean           chromium_plugin_manager_install_finish (ChromiumPluginManager *self,
                                                           GAsyncResult *result,
                                                           GError **error);
/* Removes <install-root>/<id> and rescans. An id that is not there is a
 * successful no-op — "gone" is what was asked for. */
void               chromium_plugin_manager_uninstall_async (ChromiumPluginManager *self,
                                                            const char *id,
                                                            int io_priority,
                                                            GCancellable *cancellable,
                                                            GAsyncReadyCallback callback,
                                                            gpointer user_data);
gboolean           chromium_plugin_manager_uninstall_finish (ChromiumPluginManager *self,
                                                             GAsyncResult *result,
                                                             GError **error);
/* Re-reads @id's directory and refreshes the model — what a host calls after
 * unpacking a new manifest over an installed plugin. */
void               chromium_plugin_manager_reload_async  (ChromiumPluginManager *self,
                                                          const char *id,
                                                          int io_priority,
                                                          GCancellable *cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer user_data);
gboolean           chromium_plugin_manager_reload_finish (ChromiumPluginManager *self,
                                                          GAsyncResult *result,
                                                          GError **error);

/* ::changed () — the set, or one plugin's state.
 * ::grant-request (id, permissions, request) — enable() emits it when the
 *   manifest declares names the table does not have yet. @request is a
 *   ChromiumPluginPermissionRequest: accept(), deny() or settle() it, now or
 *   later, and the enable that asked is settled with it. No return value,
 *   because the grant table is the only answer; a boolean here would be a
 *   second answer channel that the runtime then had to choose between. With no
 *   handler connected the emission decides from the table at once — the
 *   default-deny rule, unchanged; with a handler that has not answered yet the
 *   enable is parked until the request is settled or invalidated.
 *   enable() returns %FALSE with #G_IO_ERROR_PENDING for a parked request, and
 *   the manager's ::changed is the news when it later settles.
 * ::plugin-error (id, message) — the machine-readable reason a manifest was
 *   refused or a field was dropped, documented with the manifest rules in
 *   docs/plugins.md ("The manifest"): "no-manifest", "bad-id",
 *   "bad-manifest", "unsupported-type", "unsupported-field:<name>" and
 *   "popup-size-clamped". @id is the directory name even when the manifest was
 *   unreadable, so the message can always be shown next to the right row.
 *   Helper-side failures arrive here too: a script file that could not be
 *   read ("script-missing:<path>"), and the storage area a manifest never
 *   asked for ("unsupported-field:storage.sync").
 * ::plugin-message (id, api, args, reply_id) → gboolean — one call from a
 *   page through the chrome.* shim. @args is JSON text. A handler that will
 *   answer with respond() returns TRUE and the component's own handlers step
 *   aside; FALSE — and no handler at all — leaves the call to them. The
 *   reply may come long after the emission: respond() keys on @reply_id.
 * ::plugin-action (id) — the host's button for @id was pressed. What shows
 *   is the host's policy; the plugin's own listeners hear it as well. */

/* ------------------------------------------------------------------ *
 * The native plugin ABI                                              *
 *                                                                    *
 * A native plugin is a .so the manager loads into the host process,   *
 * one exported symbol, called once per enable:                       *
 *                                                                    *
 *   void cefnest_plugin_register (ChromiumPluginRegistry *registry,  *
 *                                 guint api_version);                *
 *                                                                    *
 * @api_version is what this header documents; a plugin that wants a  *
 * different one returns without registering, and the load reports    *
 * ::plugin-error "library-version". Everything a plugin can ask the  *
 * host to do is one of the five verbs below — a sixth would be an    *
 * ABI debt, because every verb here is one the unload path has to    *
 * know how to take back.                                             *
 *                                                                    *
 * Every callback a plugin hands over runs on the host's main thread. *
 * A plugin that starts a thread and touches GTK from it is outside   *
 * this contract, and taking the host down with it is what that       *
 * looks like.                                                        *
 *                                                                    *
 * Unloading runs the other way with no cooperation from the plugin:  *
 * disable() unmaps the .so, and everything registered through the    *
 * registry — menu rows, signal connections — is undone by the host   *
 * from its own bookkeeping first. A plugin keeps no global state it  *
 * cannot afford to lose between one enable() and the next.           *
 * ------------------------------------------------------------------ */

#define CEFNEST_PLUGIN_API_VERSION 1

/* Opaque: the plugin holds the pointer it was given and calls the verbs
 * below with it. What it points at is the loader's per-plugin record. */
typedef struct _ChromiumPluginRegistry ChromiumPluginRegistry;

/* A menu row the plugin wants in the page's context menu. @slot is the
 * plugin's own small number, 0–199; the host maps it to the menu id
 * 26600 + slot, inside CEF's user range and clear of the rows this
 * component already uses. The slot space is shared by every plugin: two
 * that pick the same slot are one row, and the last registration wins.
 * @label is copied. @callback runs when the row is picked, with the
 * ChromiumPluginMenuCallback signature below. */
void chromium_plugin_registry_add_menu_item (ChromiumPluginRegistry *registry,
                                             const char *label,
                                             int slot,
                                             GCallback callback,
                                             gpointer user_data);

/* The signature @callback has for add_menu_item(). @params is a JSON object
 * text describing the node the menu was opened on — link and source URLs, the
 * selection — or "null" when there is nothing to say. It is only valid for
 * the call; copy it to keep it. */
typedef void (*ChromiumPluginMenuCallback) (int slot,
                                            const char *params_json,
                                            gpointer user_data);

/* Subscribe to a signal of the session hosting the plugin — "load-end",
 * "browser-created", "changed", and the rest of the names in
 * chromium-session.h, spelled the way g_signal_connect() takes them. The
 * handler signature is that signal's own, and the first argument it receives
 * is the session. The connection is recorded; disable() or the session
 * closing disconnects it, and the plugin does nothing. */
void chromium_plugin_registry_subscribe (ChromiumPluginRegistry *registry,
                                         const char *detailed_signal,
                                         GCallback callback,
                                         gpointer user_data);

/* A widget the plugin built for the host to place — the button in a toolbar,
 * the row in a panel. The host decides whether to show it and where; a host
 * that ignores it leaves the plugin with a working menu. The widget is held
 * by the registry until the plugin unloads, so the plugin does not keep it
 * alive itself. */
void chromium_plugin_registry_set_action (ChromiumPluginRegistry *registry,
                                          GtkWidget *button);

/* Plugin storage, namespaced per plugin. set stores a JSON text under @key;
 * get delivers the current one back through the same subscription channel —
 * a handler registered with subscribe() on "plugin-storage", called as
 * (const char *key, const char *json, gpointer user_data). Until a data
 * directory is backing this (the plan's storage step) the table lives in
 * memory and dies with the process, which is the contract "no data directory
 * means nothing on disk" already made. */
void chromium_plugin_registry_storage_get (ChromiumPluginRegistry *registry,
                                           const char *key);
void chromium_plugin_registry_storage_set (ChromiumPluginRegistry *registry,
                                           const char *key,
                                           const char *json);

/* The one symbol a plugin exports. */

G_END_DECLS
