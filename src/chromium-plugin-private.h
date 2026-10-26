/* The parts of the plugin support that never reach an installed header: what
 * the kernel (chromium-plugin.c) tells the session and the two frontends about
 * a manifest it read, and what the session tells the frontends about the
 * channel. A host has no use for any of it — everything here is either
 * derivable from the public API or would let a host reach past a frontend. */

#pragma once

#include <json-glib/json-glib.h>

#include "chromium-plugin.h"
#include "chromium-session.h"

G_BEGIN_DECLS

/* --- kernel → session / frontends ---------------------------------------- */

/* The install root the session derives from its data directory:
 * <data-dir>/cefnest-plugins. %NULL for a session without one — a host that
 * never asked for a profile gets no default plugin location at all, which is
 * the same "nothing on disk" contract the data directory itself keeps. The
 * root is optional at scan time: until something is installed the directory
 * does not exist, and that is an empty list rather than a failed scan. */
void _chromium_plugin_manager_set_install_root (ChromiumPluginManager *self,
                                                const char *dir);

/* The manifest's cefnest.library ("plugin.so"), or %NULL. Only the native
 * frontend loads anything with it. */
const char *_chromium_plugin_get_library (ChromiumPlugin *self);
/* The manifest's cefnest.symbol, defaulting to "cefnest_plugin_register". */
const char *_chromium_plugin_get_symbol (ChromiumPlugin *self);
/* The manifest's content_scripts array, borrowed — the session copies the
 * entries into a plugins-load command, which is the only consumer. */
JsonArray  *_chromium_plugin_get_content_scripts (ChromiumPlugin *self);

/* --- session → frontends -------------------------------------------------- */

/* Load every enabled native plugin, unload every one that is no longer
 * enabled, and send the menu rows of anything newly loaded. Called on the
 * manager's ::changed and on start(); needs no channel. */
void chromium_plugin_native_sync (ChromiumPluginManager *manager,
                                  ChromiumSession *session);
/* Re-announce every loaded plugin's menu rows. The helper's menu table dies
 * with the helper, so this is what the session's ::ready calls. */
void chromium_plugin_native_announce (ChromiumPluginManager *manager);
/* ::closed or dispose: unload everything and drop every connection the
 * registries recorded, before the .so files are unmapped out from under
 * handlers the session might still emit. */
void chromium_plugin_native_unload_all (ChromiumPluginManager *manager);
/* A plugin-menu-command arrived for @slot; route it to whichever loaded
 * native registered it. Unknown slots are dropped — the helper can report a
 * row this process has already unloaded. */
void chromium_plugin_native_dispatch_menu (ChromiumPluginManager *manager,
                                           int slot, JsonObject *params);

/* --- the Web frontend ------------------------------------------------------ */

/* Attach the chrome.* interpreter to a manager/session pair and load whatever
 * storage the data directory holds. detach() saves what is dirty and drops
 * the rest. */
void chromium_plugin_web_attach (ChromiumPluginManager *manager,
                                 ChromiumSession *session);
void chromium_plugin_web_detach (ChromiumPluginManager *manager);

/* One plugin-message from the helper: the manager's ::plugin-message runs
 * first (a TRUE answer means the host owns the call and will respond()),
 * then the built-in handlers — storage, menus, tabs, runtime. */
void chromium_plugin_web_handle_message (ChromiumPluginManager *manager,
                                         int browser, int reply_id,
                                         const char *id, const char *api,
                                         const char *args_json);
/* uninstall()'s cleanup half: drop a plugin's storage, menu rows and action
 * listeners. */
void _chromium_plugin_web_forget (ChromiumPluginManager *manager,
                                  const char *id);

/* chromium_plugin_manager_respond()'s routing half. */
void _chromium_plugin_web_respond (ChromiumPluginManager *manager,
                                   int reply_id, gboolean ok,
                                   const char *value_json);

/* The helper's answer to a tabs.query; @request_id is what the query sent. */
void chromium_plugin_web_handle_browsers (ChromiumPluginManager *manager,
                                          int request_id,
                                          JsonArray *browsers);
/* A plugin's menu row was picked. FALSE when no web plugin owns the slot —
 * the caller then hands it to the native frontend. */
gboolean chromium_plugin_web_deliver_menu (ChromiumPluginManager *manager,
                                           int browser, int slot,
                                           JsonObject *params);
/* activate_action's delivery half: run the registered onClicked listeners. */
void chromium_plugin_web_deliver_action (ChromiumPluginManager *manager,
                                         const char *id);

/* The manifest's background.service_worker path, relative to the plugin root,
 * or NULL. The session reads it in the plugins-state diff: an enabled plugin
 * carrying one gets the resident hidden browser. */
const char *_chromium_plugin_get_background_page (ChromiumPlugin *self);

/* --- session → kernel ------------------------------------------------------- */

/* The manager's back-pointer to the session that owns it. The registry
 * verbs send their commands through it, and the session routes helper
 * events back in; the session clears it from its own dispose, before the
 * manager goes, so it never dangles. */
void _chromium_plugin_manager_set_session (ChromiumPluginManager *self,
                                           ChromiumSession *session);
ChromiumSession *_chromium_plugin_manager_get_session (ChromiumPluginManager *self);

/* --- channel plumbing ------------------------------------------------------ */

/* Send one command object. The builder holds a complete command (cmd + its
 * fields); this side owns the running guard, because a registry verb that
 * fires between stop() and the next start() has nowhere to send. Returns
 * whether the command went out — the native frontend marks a menu row
 * announced only when it did, which is what keeps announce() from re-sending
 * rows a running helper already has. */
gboolean _chromium_session_send_plugin_command (ChromiumSession *self,
                                                JsonBuilder *builder);

/* The Web frontend's three session-side needs: script in one named browser
 * (the shim delivery paths), a navigation in one named browser (tabs.update),
 * and runtime.sendMessage's delivery to every extension page of one plugin
 * that is open. All drop silently while no helper runs, like every other
 * command. */
void _chromium_session_run_javascript_in_browser (ChromiumSession *self,
                                                  int browser,
                                                  const char *code);
void _chromium_session_load_uri_in_browser (ChromiumSession *self,
                                            int browser, const char *url);
void _chromium_session_deliver_plugin_message (ChromiumSession *self,
                                               const char *plugin_id,
                                               const char *message_json);

G_END_DECLS
