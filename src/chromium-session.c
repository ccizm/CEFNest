#define G_LOG_DOMAIN "chromium-session"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "chromium-plugin.h"
#include "chromium-plugin-private.h"
#include "chromium-session.h"
#include "source-window.h"

#ifndef HELPER_PATH
#define HELPER_PATH "cefnest-helper"
#endif

/* The helper cmake built in the tree this was compiled in, which is what lets a
 * development run work without an install step — and what must not quietly win
 * after an install, because then the component runs the wrong build. */
#ifndef HELPER_DEVELOPMENT
#define HELPER_DEVELOPMENT ""
#endif

/* The helper numbers its own lines: a control event is one JSON object and a
 * newline, and a stream that has lost its newline is not a control stream any
 * more. read_line_async() on GDataInputStream grows its buffer until a newline
 * arrives, which makes the size of a peer's imagination the size of an allocation
 * here; framing the same rule over fixed chunks puts a ceiling on it. The ceiling
 * has to sit above the largest line worth having, and that is a `js` result — as
 * big as whatever the page's script returned. */
#define CONTROL_LINE_MAX (4u << 20)
#define CONTROL_CHUNK 8192u

/* The session is the host half of the control channel. It listens on a private
 * unix socket, hands the path to the helper through the environment, and keeps
 * the page state the helper reports: URL, title, loading, history. The widget
 * stays a pure compositor — none of this touches Wayland. */

enum {
  SIG_CHANGED,
  SIG_READY,
  SIG_LOAD_STARTED,
  SIG_LOAD_END,
  SIG_LOAD_ERROR,
  SIG_RENDERER_EXITED,
  SIG_SCRIPT_RESULT,
  SIG_POPUP_REQUESTED,
  SIG_PAGE_MESSAGE,
  SIG_PRIMARY_SELECTION,
  SIG_BROWSER_CREATED,
  SIG_BROWSER_SELECTED,
  SIG_BROWSER_CLOSED,
  SIG_BROWSER_SURFACE,
  SIG_BROWSERS_CHANGED,
  SIG_DOWNLOAD_BEGIN,
  SIG_DOWNLOAD_PROGRESS,
  SIG_DOWNLOAD_END,
  SIG_FIND_RESULT,
  SIG_CONTEXT_MENU,
  SIG_PERMISSION_REQUEST,
  SIG_PERMISSION_DISMISSED,
  SIG_JS_DIALOG,
  SIG_JS_DIALOG_CLOSED,
  SIG_CERTIFICATE_ERROR,
  SIG_CERTIFICATE_ERROR_CLOSED,
  SIG_AUTH_REQUIRED,
  SIG_AUTH_CLOSED,
  SIG_CLIENT_CERT_REQUEST,
  SIG_CLIENT_CERT_CLOSED,
  SIG_RENDER_UNRESPONSIVE,
  SIG_RENDER_RESPONSIVE,
  SIG_RENDER_UNRESPONSIVE_CLOSED,
  SIG_TLS_INFO,
  SIG_REQUEST_INTERCEPT,
  SIG_REQUEST_INTERCEPT_CLOSED,
  SIG_CDP_RESULT,
  SIG_CDP_EVENT,
  SIG_FILE_DIALOG,
  SIG_FILE_DIALOG_CLOSED,
  SIG_SCHEME_REQUEST,
  SIG_SCHEME_CLOSED,
  SIG_SCREENSHOT_RESULT,
  SIG_WINDOW_PROPERTIES,
  SIG_LOADING_PROGRESS,
  SIG_FULLSCREEN_CHANGED,
  SIG_AUTO_RESIZE_UPDATED,
  SIG_ZOOM_CHANGED,
  SIG_COOKIES_RESULT,
  SIG_HISTORY_RESULT,
  SIG_PAGE_TEXT_BEGIN,
  SIG_PAGE_TEXT_CHUNK,
  SIG_PAGE_TEXT_END,
  SIG_PDF_BEGIN,
  SIG_PDF_CHUNK,
  SIG_PDF_END,
  SIG_FAVICON_CHANGED,
  SIG_CONSOLE,
  SIG_TOOLTIP_CHANGED,
  SIG_STATUS_MESSAGE,
  SIG_FOCUS_REQUESTED,
  SIG_MEDIA_ACCESS_CHANGED,
  SIG_PRINT_REQUESTED,
  SIG_FRAME_CREATED,
  SIG_FRAME_DESTROYED,
  SIG_FRAME_ATTACHED,
  SIG_FRAME_DETACHED,
  SIG_MAIN_FRAME_CHANGED,
  SIG_COPY_TEXT,
  SIG_OPEN_URL,
  SIG_VIEW_SOURCE,
  SIG_VIEW_SOURCE_ERROR,
  SIG_CLOSED,
  SIG_HELPER_EXITED,
  N_SIGNALS
};

enum {
  PROP_0,
  /* Whether the session opens its own View Source window, or only reports
   * ::view-source and leaves the window to the host. */
  PROP_AUTO_SOURCE_WINDOW,
  /* The persistent profile directory, or NULL for the default ephemeral
   * profile. Only start() reads it. */
  PROP_DATA_DIRECTORY,
  /* Whether the context menu and F12 may open the inspector. Default off. */
  PROP_DEVTOOLS_ENABLED,
  /* Network identity read by the next start(): proxy, bypass list, and
   * Accept-Language. */
  PROP_PROXY,
  PROP_PROXY_BYPASS,
  PROP_ACCEPT_LANGUAGE,
  /* Page-driven size for the browser's own toplevel. */
  PROP_AUTO_RESIZE,
  PROP_AUTO_RESIZE_MIN_WIDTH,
  PROP_AUTO_RESIZE_MIN_HEIGHT,
  PROP_AUTO_RESIZE_MAX_WIDTH,
  PROP_AUTO_RESIZE_MAX_HEIGHT,
  /* The session-wide user agent, applied live over the component's own pipe.
   * Empty (the default) is Chromium's own. */
  PROP_USER_AGENT,
  /* The --autoplay-policy value the next start() hands Chromium, or NULL/""
   * for Chromium's own default. Only start() reads it. */
  PROP_AUTOPLAY_POLICY,
};
static guint signals[N_SIGNALS];

/* The control-channel major this session speaks. The helper reports its own
 * in ::ready; a differing major is a protocol mismatch, and the session tears
 * that helper down rather than misread it. */
#define SESSION_CHANNEL_MAJOR 2

/* One scheme the host registered, until start() hands it to the helper. */
typedef struct {
  char *name;
  guint flags;
} SchemeEntry;

static void scheme_entry_free (SchemeEntry *entry);
void chromium_session_browser_info_free (ChromiumBrowserInfo *info);
void chromium_client_cert_free (ChromiumClientCert *cert);

/* [a-z][a-z0-9+.-]*, never a built-in, never the plugin scheme. */
static gboolean
scheme_name_valid (const char *name)
{
  static const char *const builtins[] = {
    "http", "https", "file", "ftp", "about", "data", "devtools", "chrome",
    "view-source", "blob", "filesystem", "ws", "wss", "cefnest-extension",
  };
  size_t i;

  if (!name || !*name)
    return FALSE;
  if (name[0] < 'a' || name[0] > 'z')
    return FALSE;
  for (i = 1; name[i]; i++)
    {
      char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '.' || c == '-'))
        return FALSE;
    }
  for (i = 0; i < G_N_ELEMENTS (builtins); i++)
    if (g_strcmp0 (name, builtins[i]) == 0)
      return FALSE;
  return TRUE;
}

static void chromium_session_dispose  (GObject *object);
static void chromium_session_get_property (GObject *object, guint property,
                                           GValue *value, GParamSpec *pspec);
static void chromium_session_set_property (GObject *object, guint property,
                                           const GValue *value,
                                           GParamSpec *pspec);
static void source_transfer_reset (ChromiumSession *self);
static void chromium_session_finalize (GObject *object);
static void release_data_directory (ChromiumSession *self);
static void session_apply_auto_resize (ChromiumSession *self, int browser,
                                       int width, int height);
static void session_watch_compositor (ChromiumSession *self);

static void send_json (ChromiumSession *self, JsonNode *root);
static void command_add_string (JsonBuilder *builder, const char *key,
                                const char *value);
static void session_send_a11y (ChromiumSession *self);
static void session_read_a11y (ChromiumSession *self);
static void session_send_devtools_state (ChromiumSession *self);
static void handle_context_menu         (ChromiumSession *self, JsonObject *object);
static void handle_context_menu_dismissed (ChromiumSession *self);
static void handle_permission_request   (ChromiumSession *self, JsonObject *object);
static void handle_permission_dismissed (ChromiumSession *self, JsonObject *object);
static void handle_js_dialog            (ChromiumSession *self, JsonObject *object);
static void handle_js_dialog_closed     (ChromiumSession *self, JsonObject *object);
static void handle_certificate_error    (ChromiumSession *self, JsonObject *object);
static void handle_certificate_closed   (ChromiumSession *self, JsonObject *object);
static void handle_auth_required        (ChromiumSession *self, JsonObject *object);
static void handle_auth_closed          (ChromiumSession *self, JsonObject *object);
static void handle_client_cert_request  (ChromiumSession *self, JsonObject *object);
static void handle_client_cert_closed   (ChromiumSession *self, JsonObject *object);
static void handle_scheme_request       (ChromiumSession *self, JsonObject *object);
static void handle_scheme_closed        (ChromiumSession *self, JsonObject *object);
static void handle_tls_info             (ChromiumSession *self, JsonObject *object);
static void handle_screenshot_result    (ChromiumSession *self, JsonObject *object);
static void handle_browsers_changed     (ChromiumSession *self, JsonObject *object);
static void handle_window_properties    (ChromiumSession *self, JsonObject *object);
static void handle_request_intercept    (ChromiumSession *self, JsonObject *object);
static void handle_request_intercept_closed (ChromiumSession *self, JsonObject *object);
static void handle_cdp_result           (ChromiumSession *self, JsonObject *object);
static void handle_cdp_event            (ChromiumSession *self, JsonObject *object);
static void handle_file_dialog          (ChromiumSession *self, JsonObject *object);
static void handle_file_dialog_closed   (ChromiumSession *self, JsonObject *object);
static void session_plugins_changed     (ChromiumSession *self,
                                         ChromiumPluginManager *manager);
static void session_send_plugins_state  (ChromiumSession *self);
static void session_send_schemes        (ChromiumSession *self);
static void ua_set_send                 (ChromiumSession *self, int browser,
                                         const char *value);
static void scheme_entry_free           (SchemeEntry *entry);
void chromium_session_browser_info_free (ChromiumBrowserInfo *info);
void chromium_client_cert_free (ChromiumClientCert *cert);

struct _ChromiumSession {
  GObject parent_instance;

  char *helper_path;
  char *socket_path;
  ChromiumView *view;

  GSocketListener *listener;
  GSocketConnection *connection;
  GInputStream *input;
  GOutputStream *output;
  GCancellable *cancellable;
  GSubprocess *child;

  /* Bytes arrived and not yet carried by a newline, and the accept retry armed
   * when the listener said no for a reason that saying no again will fix. */
  GString *line_pending;
  guint accept_retry_id;

  char *uri;
  char *title;
  char *startup_url;
  /* The profile directory the host asked for, and the exact string this
   * process has registered for it while a helper is alive. The second is what
   * release_data_directory() removes, so a host that changes the property
   * mid-run does not release the path the old helper is still using. */
  char *data_directory;
  char *claimed_data_directory;
  char *proxy;
  char *proxy_bypass;
  char *accept_language;
  char *autoplay_policy;
  gboolean auto_resize;
  int auto_resize_min_width;
  int auto_resize_min_height;
  int auto_resize_max_width;
  int auto_resize_max_height;
  gboolean loading;
  gboolean can_go_back;
  gboolean can_go_forward;
  gboolean muted;
  double zoom_level;
  gboolean running;
  gboolean stopping;
  gboolean closed_emitted;
  /* The helper said it is going away — by name ({"event":"closed"}) or by
   * hanging up the control channel — while its process may still be running
   * through CefShutdown(). Teardown must not SIGKILL over that announcement:
   * see chromium_session_dispose(). */
  gboolean helper_is_exiting;
  /* The current helper was started without a data directory, so it owns a
   * private tmpfs root the host has to remove when it SIGKILLs the process. */
  gboolean child_ephemeral;

  /* Screen-reader detection: org.a11y.Status.ScreenReaderEnabled on the session
   * bus is the property Orca/gsd maintains. It drives the helper's runtime
   * accessibility state; CHROMIUM_A11Y=1 remains the explicit override. */
  GDBusProxy *a11y_proxy;
  gulong a11y_handler;
  gboolean a11y_enabled;
  gboolean a11y_forced;
  /* A g_dbus_proxy_new_for_bus() is in flight; cleared when its callback runs
   * or when dispose() gives up on it. */
  gboolean a11y_proxy_pending;

  /* The devtools-enabled property, and what the helper has been told. Sent on
   * ::ready and whenever it changes while a helper runs. */
  gboolean devtools_enabled;

  /* The context menu the helper last asked about: pending until the host answers
   * it, and the two widgets that draw it while this side is the one that does. */
  gboolean menu_pending;
  GtkWidget *menu_popover;
  GtkWidget *menu_box;
  GtkWidget *menu_selected;
  guint menu_release_source;
  guint menu_focus_source;
  /* Non-modal menu guards: outside click, Escape, and the window going
   * inactive. They are installed with the popover and removed on release. */
  GtkWindow *menu_window;
  GtkEventController *menu_outside_controller;
  GtkEventController *menu_key_controller;
  gulong menu_active_handler;

  /* Permission requests this side has asked about and not yet answered. Unlike a
     menu, several are open at once, so it is a set of ids rather than a flag —
     and an id leaves the set as soon as the helper dismisses it under the host. */
  GHashTable *permission_held;

  /* Dialog requests the helper is holding for an answer. Both are id sets for
     the same reason as permission_held; a dialog the browser takes back leaves
     the set with its *-closed event, so a host answer for it goes nowhere. */
  GHashTable *js_dialog_held;
  GHashTable *certificate_held;
  GHashTable *auth_held;
  GHashTable *client_cert_held;
  GHashTable *scheme_held;
  GHashTable *intercept_held;
  GHashTable *file_dialog_held;

  /* The helper's browser table, pushed whole on ::browsers-changed: id →
     ChromiumBrowserInfo the session owns. A host reading background-tab state
     reads this cache; a fresh helper empties it with the old ids. */
  GHashTable *browser_table;
  /* The control-channel major the running helper reported in ::ready; 0
     before the first one lands. */
  int helper_version;
  /* The session-wide user agent. Empty (NULL) is Chromium's own; both forms
     ride ua-set commands the moment they change. */
  char *user_agent;
  /* The schemes the host registered, in registration order: start() hands the
     options to the fresh helper through the environment, and every ::ready
     re-attaches the data factories. */
  GPtrArray *schemes;

  /* A view-source transfer in flight. The helper streams one request at a time
     (begin → chunk* → end, or error) and a second begin abandons the first, so
     this is one slot, not a list. */
  struct {
    guint id;
    int browser;
    char *url;
    char *title;
    GString *text;
    gboolean active;
  } source;
  /* The component's own source window, weak: the user closing it only NULLs
     the pointer, and the next request makes a new one. */
  GtkWidget *source_window;
  gboolean auto_source_window;

  /* The plugin manager, created by the first get_plugin_manager() call. A
     session that never serves plugins never builds one, and nothing plugin-
     shaped runs anywhere in that process. */
  ChromiumPluginManager *plugin_manager;
  /* The plugin ids the running helper was last told about, mapped to the root
     each was sent with. The diff against the model's enabled set is what a
     ::changed turns into plugins-load / plugins-unload traffic; ::closed
     empties it, because the helper's tables died with the helper. */
  GHashTable *plugins_sent;
  /* browser id -> the URL that browser is on. The Web frontend reads it:
     runtime.sendMessage goes to a plugin's own extension pages, and which
     pages those are is a URL question. From browser-created and the address
     events; a helper that is gone takes the table with it. */
  GHashTable *browser_urls;
  /* An enabled web plugin's background page, plugin id → browser id. Filled
     from browser-created events that carry the background flag; enable/disable
     walks it against the plugin table. */
  GHashTable *background_browsers;

  /* browser id -> the toplevel its CEF window draws into. The helper creates
   * the shared tab window first and one CefWindow per windowed browser in
   * command order, so the k-th new toplevel belongs to the k-th windowed
   * browser — the pairing test/surface-pair-gate.sh measures that claim before
   * this table is relied on. Values are owned references; ::browser-surface
   * reports a pairing when it is made and NULL when the surface goes away. */
  GHashTable *browser_surfaces;
  GHashTable *surface_browser_ids;
  GHashTable *seen_toplevels;
  GQueue *unpaired_surfaces;
  GQueue *pending_windowed_ids;
  int primary_browser_id;
  gboolean primary_surface_paired;
  /* The helper's notion of "active": every ::browser-selected names it. Events
   * that describe one browser (mute) must not be mistaken for the active
   * view's state when they were about a background one. */
  int active_browser_id;
};

G_DEFINE_TYPE (ChromiumSession, chromium_session, G_TYPE_OBJECT)

static void
session_send_a11y (ChromiumSession *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  if (!self->running)
    return;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "a11y");
  json_builder_set_member_name (builder, "enabled");
  json_builder_add_boolean_value (builder, self->a11y_enabled);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
  g_debug ("session: accessibility %s sent to helper",
           self->a11y_enabled ? "enabled" : "default");
}

static void
session_send_devtools_state (ChromiumSession *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  if (!self->running)
    return;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "devtools");
  json_builder_set_member_name (builder, "enabled");
  json_builder_add_boolean_value (builder, self->devtools_enabled);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
  g_debug ("session: devtools %s sent to helper",
           self->devtools_enabled ? "enabled" : "disabled");
}

static void
session_send_auto_resize (ChromiumSession *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  if (!self->running)
    return;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "auto-resize");
  json_builder_set_member_name (builder, "enabled");
  json_builder_add_boolean_value (builder, self->auto_resize);
  json_builder_set_member_name (builder, "min_width");
  json_builder_add_int_value (builder, self->auto_resize_min_width);
  json_builder_set_member_name (builder, "min_height");
  json_builder_add_int_value (builder, self->auto_resize_min_height);
  json_builder_set_member_name (builder, "max_width");
  json_builder_add_int_value (builder, self->auto_resize_max_width);
  json_builder_set_member_name (builder, "max_height");
  json_builder_add_int_value (builder, self->auto_resize_max_height);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* ------------------------------------------------------------------ *
 * Plugins: the model on this side, the tables on the helper's        *
 *                                                                    *
 * The helper keeps no plugin state that survives a restart, so the   *
 * session's rule is the same one the devtools switch follows: what   *
 * the host has set goes to a running helper at once, and to a new    *
 * helper at ::ready. The diff is driven by the manager's ::changed   *
 * against plugins_sent — what this side last told a helper — so a    *
 * rescan that changes nothing sends nothing.                         *
 * ------------------------------------------------------------------ */

gboolean
_chromium_session_send_plugin_command (ChromiumSession *self,
                                       JsonBuilder *builder)
{
  g_autoptr (JsonNode) root = NULL;

  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  g_return_val_if_fail (builder != NULL, FALSE);

  /* A registry verb that fires while no helper runs — a plugin enabled
     before start(), or one disabling during teardown — has nowhere to send.
     announce() and the ready path put the menu rows back; the script table
     goes with the next plugins-load. */
  if (!self->running)
    return FALSE;

  root = json_builder_get_root (builder);
  send_json (self, root);
  return TRUE;
}

/* One plugin's entry in a plugins-load array. Native plugins carry no
   scripts but do carry the root: the extension scheme serves every loaded
   plugin's pages from it, popup and options included. */
static void
session_append_plugin (JsonBuilder *builder, ChromiumPlugin *plugin)
{
  JsonArray *scripts = _chromium_plugin_get_content_scripts (plugin);

  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, chromium_plugin_get_id (plugin));
  json_builder_set_member_name (builder, "root");
  json_builder_add_string_value (builder, chromium_plugin_get_root (plugin));
  json_builder_set_member_name (builder, "scripts");
  json_builder_begin_array (builder);
  if (scripts)
    for (guint i = 0; i < json_array_get_length (scripts); i++)
      {
        JsonObject *entry = json_array_get_object_element (scripts, i);
        JsonArray *matches = json_object_has_member (entry, "matches") ?
          json_object_get_array_member (entry, "matches") : NULL;
        JsonArray *files = json_object_has_member (entry, "js") ?
          json_object_get_array_member (entry, "js") : NULL;
        const char *run_at = json_object_has_member (entry, "run_at") ?
          json_object_get_string_member (entry, "run_at") : NULL;

        json_builder_begin_object (builder);
        json_builder_set_member_name (builder, "matches");
        json_builder_begin_array (builder);
        if (matches)
          for (guint j = 0; j < json_array_get_length (matches); j++)
            json_builder_add_string_value (
              builder, json_array_get_string_element (matches, j));
        json_builder_end_array (builder);
        json_builder_set_member_name (builder, "js");
        json_builder_begin_array (builder);
        if (files)
          for (guint j = 0; j < json_array_get_length (files); j++)
            json_builder_add_string_value (
              builder, json_array_get_string_element (files, j));
        json_builder_end_array (builder);
        json_builder_set_member_name (builder, "run_at");
        json_builder_add_string_value (builder, run_at ? run_at
                                                       : "document_idle");
        json_builder_end_object (builder);
      }
  json_builder_end_array (builder);
}

/* One plugins-load carrying one plugin. The helper replaces the entry with
   this id — its semantics are "the whole truth about this id", never
   "add to what you have". */
static void
session_send_plugin_load (ChromiumSession *self, ChromiumPlugin *plugin)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *background = _chromium_plugin_get_background_page (plugin);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "plugins-load");
  json_builder_set_member_name (builder, "plugins");
  json_builder_begin_array (builder);
  json_builder_begin_object (builder);
  session_append_plugin (builder, plugin);
  if (background && *background)
    {
      json_builder_set_member_name (builder, "background");
      json_builder_add_string_value (builder, background);
    }
  json_builder_end_object (builder);
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
session_send_plugin_unload (ChromiumSession *self, const char *id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "plugins-unload");
  json_builder_set_member_name (builder, "id");
  json_builder_add_string_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* The background-page half of the plugin diff: an enabled plugin whose
   manifest names a background.service_worker gets one resident hidden
   browser, and one that is disabled or gone loses it. @enabled holds the
   enabled plugins (id → plugin, refs). Called with the plugins-load traffic
   so a helper that has just come up — or a plugin that has just been
   enabled — reaches the same shape either way. */
static void
session_sync_background_browsers (ChromiumSession *self, GHashTable *enabled)
{
  GHashTableIter iter;
  gpointer key, value;

  if (!self->background_browsers)
    self->background_browsers = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free, NULL);

  g_hash_table_iter_init (&iter, enabled);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ChromiumPlugin *plugin = CHROMIUM_PLUGIN (value);
      const char *background = _chromium_plugin_get_background_page (plugin);

      if (background && *background &&
          !g_hash_table_contains (self->background_browsers, key))
        {
          g_autofree char *uri =
            g_strconcat (CHROMIUM_EXTENSION_URI_PREFIX,
                         chromium_plugin_get_id (plugin),
                         "/__cefnest_background__.html", NULL);

          chromium_session_new_background_browser (self, uri);
          /* browser-created fills the table when the helper announces the
             new browser; until then this id stays absent, but the page load
             itself is idempotent — a second ask before the announcement
             would be a second browser, so the announcement is what the
             contains() gate waits for. */
        }
    }

  g_hash_table_iter_init (&iter, self->background_browsers);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!g_hash_table_contains (enabled, key))
      {
        chromium_session_close_browser (self, GPOINTER_TO_INT (value));
        g_hash_table_iter_remove (&iter);
      }
}

/* The diff itself. Only ever called while a helper runs; the ::ready path
   calls it against an empty plugins_sent, which sends every enabled plugin
   — the "start 补发" half of the rule. */
static void
session_send_plugins_state (ChromiumSession *self)
{
  ChromiumPluginManager *manager = self->plugin_manager;
  GListModel *model;
  GHashTable *live, *enabled;
  GHashTableIter iter;
  gpointer key, value;
  guint n, i;

  if (!manager || !self->running)
    return;

  /* ::closed empties the sent table along with the helper's own tables; a
     start() after that rebuilds it here rather than at some earlier moment
     that may never have happened. */
  if (!self->plugins_sent)
    self->plugins_sent = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

  live = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  enabled = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   g_object_unref);
  model = chromium_plugin_manager_get_plugins (manager);
  n = g_list_model_get_n_items (model);
  for (i = 0; i < n; i++)
    {
      ChromiumPlugin *plugin = g_list_model_get_item (model, i);

      if (chromium_plugin_get_state (plugin) ==
            CHROMIUM_PLUGIN_STATE_ENABLED)
        {
          const char *id = chromium_plugin_get_id (plugin);
          const char *root = chromium_plugin_get_root (plugin);
          const char *sent_root = g_hash_table_lookup (self->plugins_sent, id);

          if (g_strcmp0 (sent_root, root) != 0)
            session_send_plugin_load (self, plugin);
          g_hash_table_replace (live, g_strdup (id), g_strdup (root));
          g_hash_table_replace (enabled, g_strdup (id),
                                g_object_ref (plugin));
        }
      g_object_unref (plugin);
    }

  g_hash_table_iter_init (&iter, self->plugins_sent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!g_hash_table_contains (live, key))
      {
        session_send_plugin_unload (self, key);
        g_hash_table_iter_remove (&iter);
      }

  g_hash_table_unref (self->plugins_sent);
  self->plugins_sent = live;

  session_sync_background_browsers (self, enabled);
  g_hash_table_unref (enabled);
}

/* --- the Web frontend's session-side needs ------------------------------ */

void
_chromium_session_run_javascript_in_browser (ChromiumSession *self,
                                             int browser, const char *code)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (code != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "js");
  json_builder_set_member_name (builder, "code");
  json_builder_add_string_value (builder, code);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, 0);
  json_builder_set_member_name (builder, "browser");
  json_builder_add_int_value (builder, browser);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
_chromium_session_load_uri_in_browser (ChromiumSession *self, int browser,
                                       const char *url)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (url != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "load");
  json_builder_set_member_name (builder, "url");
  json_builder_add_string_value (builder, url);
  json_builder_set_member_name (builder, "browser");
  json_builder_add_int_value (builder, browser);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
_chromium_session_deliver_plugin_message (ChromiumSession *self,
                                          const char *plugin_id,
                                          const char *message_json)
{
  g_autofree char *prefix = NULL;
  g_autofree char *code = NULL;
  GHashTableIter iter;
  gpointer key, value;
  g_autofree char *message_quoted = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (plugin_id != NULL);

  if (!self->browser_urls)
    return;

  prefix = g_strconcat (CHROMIUM_EXTENSION_URI_PREFIX, plugin_id, "/", NULL);
  message_quoted = g_strescape (message_json ? message_json : "null", NULL);

  g_hash_table_iter_init (&iter, self->browser_urls);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *url = value;

      if (g_str_has_prefix (url, prefix))
        {
          /* One line of JS per open page: the shim's __cefnestOnMessage fans
             the message out to that page's own listeners. */
          code = g_strdup_printf ("window.__cefnestOnMessage&&"
                                  "__cefnestOnMessage(\"%s\");",
                                  message_quoted);
          _chromium_session_run_javascript_in_browser (self,
                                                       GPOINTER_TO_INT (key),
                                                       code);
          g_clear_pointer (&code, g_free);
        }
    }
}

/* Connected swapped, so the call is (self, manager): the session first — the
   same order the body below wants to read. */
static void
session_plugins_changed (ChromiumSession *self,
                         ChromiumPluginManager *manager)
{
  if (manager != self->plugin_manager)
    return;

  /* The .so half needs no channel: it loads and unloads against the model
     alone, so it also runs before start() — that is what lets a host enable
     everything before the helper exists. */
  chromium_plugin_native_sync (manager, self);
  session_send_plugins_state (self);
}

/* The plugin install root is anchored on the data directory, not on this
 * component's name: <data-dir>/cefnest-plugins, the same tree the plugin
 * storage already keeps (<data-dir>/cefnest-plugins/<id>/storage.json) and
 * the same tree clear_data() removes. A host that asked for a profile gets
 * its plugin tree scanned; a host without a data directory gets no default
 * root at all, which is what keeps "no directory means nothing on disk"
 * true for plugins too. Derived when the manager is created and whenever
 * the property moves; the next scan() reads it. */
static void
session_plugin_install_root (ChromiumSession *self)
{
  g_autofree char *root = NULL;

  if (!self->plugin_manager)
    return;

  /* A relative path is start()'s problem — it fails there loudly — and is
     nothing to anchor until then. */
  if (self->data_directory && g_path_is_absolute (self->data_directory))
    root = g_build_filename (self->data_directory, "cefnest-plugins", NULL);

  _chromium_plugin_manager_set_install_root (self->plugin_manager, root);
}

/* The session owns the manager and creates it here, on first use. */
ChromiumPluginManager *
chromium_session_get_plugin_manager (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);

  if (!self->plugin_manager)
    {
      self->plugin_manager = chromium_plugin_manager_new ();
      self->plugins_sent = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_free);
      self->browser_urls = g_hash_table_new_full (g_direct_hash,
                                                  g_direct_equal,
                                                  NULL, g_free);
      _chromium_plugin_manager_set_session (self->plugin_manager, self);
      session_plugin_install_root (self);
      chromium_plugin_web_attach (self->plugin_manager, self);
      g_signal_connect_object (self->plugin_manager, "changed",
                               G_CALLBACK (session_plugins_changed), self,
                               G_CONNECT_SWAPPED);
    }

  return self->plugin_manager;
}

static void
session_read_a11y (ChromiumSession *self)
{
  g_autoptr (GVariant) value = NULL;
  gboolean enabled = FALSE;

  if (!self->a11y_proxy || self->a11y_forced)
    return;
  value = g_dbus_proxy_get_cached_property (self->a11y_proxy,
                                            "ScreenReaderEnabled");
  if (value && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
    enabled = g_variant_get_boolean (value);

  if (enabled == self->a11y_enabled)
    return;
  self->a11y_enabled = enabled;
  g_debug ("session: ScreenReaderEnabled=%d", enabled);
  session_send_a11y (self);
}

static void
session_a11y_changed (GDBusProxy *proxy, GVariant *changed,
                      const char *const *invalidated, gpointer data)
{
  (void) changed;
  (void) invalidated;
  session_read_a11y (data);
}

static void
chromium_session_a11y_proxy_ready (GObject *source, GAsyncResult *result,
                                   gpointer data)
{
  ChromiumSession *self = data;
  g_autoptr (GDBusProxy) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  if (!self->a11y_proxy_pending)
    {
      /* dispose() gave up on this proxy while the call was in flight. */
      g_object_unref (self);
      return;
    }
  self->a11y_proxy_pending = FALSE;

  if (!proxy)
    g_debug ("session: no org.a11y.Status: %s",
             error ? error->message : "unknown");
  else if (!self->a11y_proxy)
    {
      self->a11y_proxy = g_steal_pointer (&proxy);
      self->a11y_handler =
        g_signal_connect (self->a11y_proxy, "g-properties-changed",
                          G_CALLBACK (session_a11y_changed), self);
    }

  session_read_a11y (self);
  /* Held from the g_dbus_proxy_new_for_bus() call: the session outlives
   * dispose() only until this callback has run. */
  g_object_unref (self);
}

/* No GTK/GIO dependency is added to the helper: the host watches the session
 * bus property and flips CefBrowserHost::SetAccessibilityState through the
 * existing channel. */
static void
chromium_session_watch_a11y (ChromiumSession *self)
{
  if (self->a11y_forced || self->a11y_proxy || self->a11y_proxy_pending)
    return;

  self->a11y_proxy_pending = TRUE;
  /* Async on purpose: a sync proxy creation blocks this thread on a round trip
   * to the session bus, and building a session to ask it what its helper path
   * is — which is what the demo does before deciding whether a browser is even
   * available — must not pay for that. The answer lands on the main loop, by
   * which time the helper is usually connected and the state session_read_a11y
   * works out rides along with ::ready's own a11y command or follows it. */
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                            "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Status",
                            NULL, chromium_session_a11y_proxy_ready,
                            g_object_ref (self));
}

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
   * that was stillborn or is already being disposed.
   *
   * Whoever gets here first sets the reason, and a second arrival is silent.
   * That is not a lost fact: both of the paths that race (the channel hanging
   * up and the child being reaped) mean the same thing to the host — the helper
   * is gone. @why goes out on the signal, so a host can tell its own stop from
   * a death it did not ask for. */
  if (self->closed_emitted || (!self->running && !self->child))
    return;

  self->closed_emitted = TRUE;
  self->running = FALSE;

  /* Before anything emits: the native plugins' handlers are pages of code a
     g_module_close() is about to unmap, and the ::changed below — set_state
     emits it — must not call into one. ::closed at the end of this function
     goes out the same way, with the plugins already gone. */
  chromium_plugin_native_unload_all (self->plugin_manager);
  g_clear_pointer (&self->plugins_sent, g_hash_table_unref);
  g_clear_pointer (&self->browser_urls, g_hash_table_unref);
  g_clear_pointer (&self->background_browsers, g_hash_table_unref);

  g_message ("session: helper stopped (%s)", why);
  chromium_session_set_state (self, NULL, NULL, FALSE, FALSE, FALSE);
  /* A transfer the helper was streaming dies with it; the half-assembled
   * string is not a source anyone can read. */
  source_transfer_reset (self);
  g_signal_emit (self, signals[SIG_CLOSED], 0, why);
}

static const char *
event_string (JsonObject *object, const char *key)
{
  /* The helper is a separate process, and a signal that documents a const char *
   * must not be handed a NULL because one field went missing on the way. */
  /* has_member() is true for an explicit JSON null, and the getter answers
   * NULL for it. A signal documented as const char * must not receive that
   * any more than it may receive a missing member. */
  if (!json_object_has_member (object, key))
    return "";

  {
    const char *value = json_object_get_string_member (object, key);

    return value ? value : "";
  }
}

/* The three below are the same rule as event_string, for the other field types a
 * helper of a different version can leave out: json-glib answers a missing member
 * with a critical and a zero, and a zero is a value — an id of 0, a 0 % progress,
 * a menu at 0,0 — that goes on to be believed. Measured (test/event-shape-gate.sh,
 * 2026-10-22): 28 name-only events through the unguarded shape give 18 criticals
 * and then a SIGSEGV in the host. */
static int
event_int (JsonObject *object, const char *key)
{
  return json_object_has_member (object, key) ?
           (int) json_object_get_int_member (object, key) : 0;
}

static gboolean
event_flag (JsonObject *object, const char *key)
{
  return json_object_has_member (object, key) ?
           json_object_get_boolean_member (object, key) : FALSE;
}

/* ------------------------------------------------------------------ *
 * View Source                                                         *
 *                                                                      *
 * The helper streams one frame's HTML as view-source-begin/chunk/end  *
 * (or -error) events, cut to fit the channel's line limit. This side  *
 * reassembles it, reports ::view-source, and — unless the host turned *
 * auto-source-window off — shows it in the component's own window.    *
 * The text is the DOM as GetSource() serialized it, script changes    *
 * included; it is not the network's original bytes.                   *
 * ------------------------------------------------------------------ */

/* The helper caps a stream at 16 MiB; the same cap here means a broken or
 * hostile helper cannot grow the string without end. */
#define SOURCE_HOST_MAX (16u << 20)

static void
source_transfer_reset (ChromiumSession *self)
{
  self->source.active = FALSE;
  self->source.id = 0;
  self->source.browser = 0;
  g_clear_pointer (&self->source.url, g_free);
  g_clear_pointer (&self->source.title, g_free);
  if (self->source.text)
    {
      g_string_free (self->source.text, TRUE);
      self->source.text = NULL;
    }
}

static gboolean
source_transfer_current (ChromiumSession *self, JsonObject *object)
{
  return self->source.active &&
         self->source.id == (guint) event_int (object, "id");
}

static void
source_transfer_finished (ChromiumSession *self, gboolean truncated)
{
  GtkRoot *root = self->view ? gtk_widget_get_root (GTK_WIDGET (self->view))
                             : NULL;

  g_signal_emit (self, signals[SIG_VIEW_SOURCE], 0,
                 self->source.browser,
                 self->source.url ? self->source.url : "",
                 self->source.text ? self->source.text->str : "");

  if (self->auto_source_window)
    {
      /* Reused when the previous window is still alive: two requests, one
       * window, the second text replaces the first. */
      g_debug ("source window: %s",
               self->source_window ? "reused" : "created");
      if (!self->source_window)
        {
          self->source_window = cefnest_source_window_new ();
          g_object_add_weak_pointer (G_OBJECT (self->source_window),
                                     (gpointer *) &self->source_window);
        }
      cefnest_source_window_show (self->source_window,
                                  GTK_IS_WINDOW (root) ? GTK_WINDOW (root)
                                                       : NULL,
                                  self->source.url, self->source.title,
                                  self->source.text ? self->source.text->str
                                                    : "",
                                  self->source.text
                                    ? (gssize) self->source.text->len : 0,
                                  truncated);
    }

  source_transfer_reset (self);
}

static double
event_number (JsonObject *object, const char *key)
{
  return json_object_has_member (object, key) ?
           json_object_get_double_member (object, key) : 0.0;
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

/* The whole event as one JSON text. The result-shaped signals carry the
 * helper's own payload this way rather than growing a parameter per member:
 * the control channel's shapes are documented once, in docs/control-channel.md,
 * and a host that needs a member parses it from here. */
static char *
event_json_text (JsonObject *object)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) node = json_node_new (JSON_NODE_OBJECT);
  gsize length = 0;

  json_node_take_object (node, json_object_ref (object));
  json_generator_set_root (generator, node);
  return json_generator_to_data (generator, &length);
}

/* ------------------------------------------------------------------ *
 * Browser -> surface pairing                                         *
 *                                                                    *
 * The helper creates one toplevel for the shared tab window and one   *
 * more for every windowed browser, in the order the commands were     *
 * issued. The compositor sees those toplevels in its own list order.  *
 * Measured 2026-10-26 (test/surface-pair-gate.sh, real desktop): the  *
 * two orders agree in all four scenarios the plugin plan named —      *
 * popup+options at once, close one then open another (the address     *
 * reuse case), with the inspector window open, and four plugins'      *
 * popups opened back to back — with every surface sampled and the     *
 * colour matched to its browser id. Either side can still arrive      *
 * first: a browser-created event can precede its toplevel, and a      *
 * toplevel can precede the browser that owns it, so both directions   *
 * are queued and the pairing is made when the second half shows up.   *
 * ------------------------------------------------------------------ */

static void
session_pair_browser_surface (ChromiumSession *self, int id,
                              NestedSurface *surface)
{
  NestedSurface *old;

  if (!self->browser_surfaces)
    return;
  old = g_hash_table_lookup (self->browser_surfaces, GINT_TO_POINTER (id));
  if (old == surface)
    return;
  if (old)
    {
      g_hash_table_remove (self->surface_browser_ids, old);
      g_hash_table_remove (self->browser_surfaces, GINT_TO_POINTER (id));
      g_signal_emit (self, signals[SIG_BROWSER_SURFACE], 0, id, NULL);
    }
  g_hash_table_replace (self->browser_surfaces, GINT_TO_POINTER (id),
                        g_object_ref (surface));
  g_hash_table_replace (self->surface_browser_ids, surface,
                        GINT_TO_POINTER (id));
  g_signal_emit (self, signals[SIG_BROWSER_SURFACE], 0, id, surface);
}

static void
session_unpair_browser_surface (ChromiumSession *self, int id)
{
  NestedSurface *surface;

  if (!self->browser_surfaces)
    return;
  surface = g_hash_table_lookup (self->browser_surfaces, GINT_TO_POINTER (id));
  if (!surface)
    return;
  g_hash_table_remove (self->surface_browser_ids, surface);
  g_hash_table_remove (self->browser_surfaces, GINT_TO_POINTER (id));
  g_signal_emit (self, signals[SIG_BROWSER_SURFACE], 0, id, NULL);
}

static void
session_note_toplevel (ChromiumSession *self, NestedSurface *surface)
{
  gpointer pending;

  if (g_hash_table_contains (self->seen_toplevels, surface))
    return;
  g_hash_table_add (self->seen_toplevels, g_object_ref (surface));

  if (!self->primary_surface_paired && self->primary_browser_id != 0)
    {
      self->primary_surface_paired = TRUE;
      session_pair_browser_surface (self, self->primary_browser_id, surface);
      return;
    }
  pending = self->pending_windowed_ids
              ? g_queue_pop_head (self->pending_windowed_ids) : NULL;
  if (pending)
    {
      session_pair_browser_surface (self, GPOINTER_TO_INT (pending), surface);
      return;
    }
  g_queue_push_tail (self->unpaired_surfaces, g_object_ref (surface));
}

static void
session_note_browser_created (ChromiumSession *self, int id, gboolean windowed,
                              gboolean background)
{
  if (!self->browser_surfaces)
    return;

  /* A background page owns no toplevel — pairing it here would either spend
     a FIFO surface on it or park a windowed id waiting for a surface that
     never comes. */
  if (background)
    return;

  if (!windowed && self->primary_browser_id == 0)
    {
      NestedSurface *surface = self->unpaired_surfaces
                                 ? g_queue_pop_head (self->unpaired_surfaces)
                                 : NULL;

      self->primary_browser_id = id;
      if (surface && !self->primary_surface_paired)
        {
          self->primary_surface_paired = TRUE;
          session_pair_browser_surface (self, id, surface);
        }
      else if (surface)
        g_queue_push_head (self->unpaired_surfaces, surface);
      return;
    }

  if (windowed)
    {
      NestedSurface *surface = self->unpaired_surfaces
                                 ? g_queue_pop_head (self->unpaired_surfaces)
                                 : NULL;

      if (surface)
        session_pair_browser_surface (self, id, surface);
      else
        g_queue_push_tail (self->pending_windowed_ids, GINT_TO_POINTER (id));
    }
}

static void
session_toplevels_changed (NestedCompositor *compositor, gpointer data)
{
  ChromiumSession *self = CHROMIUM_SESSION (data);
  GList *tops, *l, *keys, *k;

  tops = nested_compositor_get_toplevels (compositor);

  /* A surface that is gone takes its pairing with it: the next window can come
   * back with the same address, and a stale key would make that look paired. */
  keys = g_hash_table_get_keys (self->seen_toplevels);
  for (k = keys; k; k = k->next)
    {
      NestedSurface *surface = k->data;

      if (g_list_find (tops, surface))
        continue;
      g_queue_remove (self->unpaired_surfaces, surface);
      if (g_hash_table_contains (self->surface_browser_ids, surface))
        {
          int id = GPOINTER_TO_INT (
            g_hash_table_lookup (self->surface_browser_ids, surface));
          session_unpair_browser_surface (self, id);
        }
      g_hash_table_remove (self->seen_toplevels, surface);
    }
  g_list_free (keys);

  for (l = tops; l; l = l->next)
    session_note_toplevel (self, l->data);

  g_list_free_full (tops, g_object_unref);
}

static void
session_watch_compositor (ChromiumSession *self)
{
  NestedCompositor *compositor;

  if (!self->view)
    return;
  compositor = chromium_view_get_compositor (self->view);
  if (!compositor)
    return;
  g_signal_handlers_disconnect_matched (compositor, G_SIGNAL_MATCH_DATA, 0, 0,
                                        NULL, NULL, self);
  g_signal_connect_object (compositor, "toplevels-changed",
                           G_CALLBACK (session_toplevels_changed), self, 0);
  session_toplevels_changed (compositor, self);
}

static NestedSurface *
session_surface_for_browser (ChromiumSession *self, int id)
{
  if (!self->browser_surfaces)
    return NULL;
  return g_hash_table_lookup (self->browser_surfaces, GINT_TO_POINTER (id));
}

static void
session_apply_auto_resize (ChromiumSession *self, int browser, int width,
                           int height)
{
  NestedCompositor *compositor;
  NestedSurface *surface;

  if (!self->view || width <= 0 || height <= 0)
    return;
  compositor = chromium_view_get_compositor (self->view);
  if (!compositor)
    return;
  surface = session_surface_for_browser (self, browser);
  if (!surface)
    {
      g_debug ("session: auto-resize %dx%d for browser %d has no paired surface",
               width, height, browser);
      return;
    }
  nested_compositor_set_surface_size (compositor, surface, width, height);
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
      int version = (int) event_int (object, "version");

      g_message ("session: helper ready, pid=%d cef=%s version=%d",
                 (int) event_int (object, "pid"),
                 event_string (object, "cef"), version);

      /* The handshake: a helper that speaks a different major moves the whole
         protocol under the host, and nothing either side says after that is
         readable. Tear this run down rather than misread it. */
      if (version > 0 && version / 100 != SESSION_CHANNEL_MAJOR / 100)
        {
          g_critical ("session: helper protocol version %d does not match "
                      "this session's %d — stop(): the helper is leaving",
                      version, SESSION_CHANNEL_MAJOR);
          self->helper_version = version;
          chromium_session_stop (self);
          return;
        }
      self->helper_version = version;

      session_send_a11y (self);
      session_send_devtools_state (self);
      session_send_auto_resize (self);
      session_send_schemes (self);
      if (self->user_agent && *self->user_agent)
        ua_set_send (self, 0, self->user_agent);
      /* The helper's plugin tables start empty on every start(), and a plugin
         disabled while the previous helper ran left its .so unloaded — the
         sync first brings the loaded set back in step with the model, then
         the announce puts the menu rows back, then the diff sends the script
         table against the empty plugins_sent, which is every enabled
         plugin. */
      if (self->plugin_manager)
        {
          chromium_plugin_native_sync (self->plugin_manager, self);
          chromium_plugin_native_announce (self->plugin_manager);
          session_send_plugins_state (self);
        }

      /* After the plugin table, not before: the helper builds every new
       * renderer's plugin table from what it knows at spawn time, so a first
       * navigation that overtakes plugins-load launches a renderer with an
       * empty table and the page's document_start scripts never run. Measured
       * 2026-10-26: positional-URL start showed 0/6 until a reload. The first
       * navigation still goes through the channel, not the command line:
       * Chromium rewrites its own argv during start-up, so a URL handed over
       * as an argument may not survive until the browser is created. */
      if (self->startup_url && *self->startup_url)
        {
          chromium_session_load_uri (self, 0, self->startup_url);
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
                                  event_string (object, "url"),
                                  NULL, self->loading, self->can_go_back,
                                  self->can_go_forward);
      if (self->browser_urls)
        g_hash_table_replace (self->browser_urls,
                              GINT_TO_POINTER (event_int (object, "browser")),
                              g_strdup (event_string (object, "url")));
      return;
    }

  if (!strcmp (event, "title"))
    {
      chromium_session_set_state (self, NULL,
                                  event_string (object, "title"),
                                  self->loading, self->can_go_back,
                                  self->can_go_forward);
      return;
    }

  if (!strcmp (event, "loading"))
    {
      gboolean loading = event_flag (object, "loading");
      gboolean can_go_back = event_flag (object, "can_go_back");
      gboolean can_go_forward = event_flag (object, "can_go_forward");

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

  if (!strcmp (event, "browser-created"))
    {
      int id = (int) event_int (object, "browser");
      gboolean windowed = event_flag (object, "windowed");
      gboolean background = event_flag (object, "background");

      if (self->browser_urls)
        g_hash_table_replace (self->browser_urls, GINT_TO_POINTER (id),
                              g_strdup (event_string (object, "url")));
      if (background && self->background_browsers)
        {
          /* The background page's URL carries the plugin id it belongs to:
           * cefnest-extension://<id>/__cefnest_background__.html. The id is
           * how enable/disable finds the browser to tear down later. */
          const char *url = event_string (object, "url");
          const char *prefix = CHROMIUM_EXTENSION_URI_PREFIX;
          if (g_str_has_prefix (url, prefix))
            {
              const char *rest = url + strlen (prefix);
              const char *slash = strchr (rest, '/');
              if (slash && slash > rest)
                g_hash_table_replace (self->background_browsers,
                                      g_strndup (rest, slash - rest),
                                      GINT_TO_POINTER (id));
            }
        }
      session_note_browser_created (self, id, windowed, background);
      g_signal_emit (self, signals[SIG_BROWSER_CREATED], 0, id,
                     event_string (object, "url"),
                     windowed,
                     event_flag (object, "devtools"),
                     background);
      return;
    }

  if (!strcmp (event, "browser-selected"))
    {
      int id = (int) event_int (object, "browser");
      self->active_browser_id = id;
      g_signal_emit (self, signals[SIG_BROWSER_SELECTED], 0, id);
      return;
    }

  if (!strcmp (event, "browser-closed"))
    {
      int id = (int) event_int (object, "browser");

      if (self->browser_urls)
        g_hash_table_remove (self->browser_urls, GINT_TO_POINTER (id));
      if (self->background_browsers)
        {
          /* A background page that went away on its own un-registers here:
             the next enable, or the next plugins-state diff, can then create
             a fresh one instead of trusting a dead id. */
          GHashTableIter bg_iter;
          gpointer bg_key, bg_value;

          g_hash_table_iter_init (&bg_iter, self->background_browsers);
          while (g_hash_table_iter_next (&bg_iter, &bg_key, &bg_value))
            if (GPOINTER_TO_INT (bg_value) == id)
              g_hash_table_iter_remove (&bg_iter);
        }
      session_unpair_browser_surface (self, id);
      if (self->primary_browser_id == id)
        {
          self->primary_browser_id = 0;
          self->primary_surface_paired = FALSE;
        }
      g_signal_emit (self, signals[SIG_BROWSER_CLOSED], 0, id);
      return;
    }

  if (!strcmp (event, "take-focus"))
    {
      /* The page's traversal ran off the edge of the document: the walk goes
       * back to GTK, from the view's own place in the chain. The browser the
       * event carries is informational — only the page that holds the
       * keyboard can ever get here, and that is the page the view shows. */
      if (self->view)
        chromium_view_release_chain_focus (self->view,
                                           event_flag (object, "next"));
      return;
    }

  if (!strcmp (event, "view-source-begin"))
    {
      /* A second begin abandons the first transfer: the helper only streams
       * one request at a time, so this is a restart, not an overlap. */
      source_transfer_reset (self);
      self->source.active = TRUE;
      self->source.id = (guint) event_int (object, "id");
      self->source.browser = (int) event_int (object, "browser");
      self->source.url = g_strdup (event_string (object, "url"));
      self->source.title = g_strdup (event_string (object, "title"));
      self->source.text = g_string_new (NULL);
      return;
    }

  if (!strcmp (event, "view-source-chunk"))
    {
      if (!source_transfer_current (self, object))
        return;
      /* Chunks are ordered, and each is whole UTF-8; appending is all the
       * reassembly there is. The cap is the silent stop for a stream that
       * lied about its size. */
      if (self->source.text && self->source.text->len < SOURCE_HOST_MAX)
        g_string_append (self->source.text, event_string (object, "data"));
      return;
    }

  if (!strcmp (event, "view-source-end"))
    {
      if (!source_transfer_current (self, object))
        return;
      source_transfer_finished (self, event_flag (object, "truncated"));
      return;
    }

  if (!strcmp (event, "view-source-error"))
    {
      /* An error can follow a begin (mid-stream failure) or arrive alone:
       * the frame can be gone before anything was streamed, which is what
       * the helper's EmitFrameSource() reports with no begin in front. */
      if (source_transfer_current (self, object))
        {
          int browser = self->source.browser;

          source_transfer_reset (self);
          g_signal_emit (self, signals[SIG_VIEW_SOURCE_ERROR], 0, browser,
                         event_string (object, "message"));
        }
      else if (!self->source.active)
        g_signal_emit (self, signals[SIG_VIEW_SOURCE_ERROR], 0,
                       (int) event_int (object, "browser"),
                       event_string (object, "message"));
      return;
    }

  if (!strcmp (event, "cookies-result"))
    {
      g_autofree char *payload = event_json_text (object);

      g_signal_emit (self, signals[SIG_COOKIES_RESULT], 0,
                     (int) event_int (object, "id"),
                     event_flag (object, "ok"), payload);
      return;
    }

  if (!strcmp (event, "history-result"))
    {
      g_autofree char *payload = event_json_text (object);

      g_signal_emit (self, signals[SIG_HISTORY_RESULT], 0,
                     (int) event_int (object, "id"), payload);
      return;
    }

  if (!strcmp (event, "page-text-begin"))
    {
      g_signal_emit (self, signals[SIG_PAGE_TEXT_BEGIN], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "browser"),
                     (int) event_int (object, "bytes"),
                     event_flag (object, "truncated"));
      return;
    }

  if (!strcmp (event, "page-text-chunk"))
    {
      g_signal_emit (self, signals[SIG_PAGE_TEXT_CHUNK], 0,
                     (int) event_int (object, "id"),
                     event_string (object, "data"));
      return;
    }

  if (!strcmp (event, "page-text-end"))
    {
      g_signal_emit (self, signals[SIG_PAGE_TEXT_END], 0,
                     (int) event_int (object, "id"),
                     event_flag (object, "truncated"));
      return;
    }

  if (!strcmp (event, "pdf-begin"))
    {
      g_signal_emit (self, signals[SIG_PDF_BEGIN], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "bytes"),
                     event_flag (object, "truncated"));
      return;
    }

  if (!strcmp (event, "pdf-chunk"))
    {
      gsize length = 0;
      g_autofree guchar *decoded =
        g_base64_decode (event_string (object, "data"), &length);
      GBytes *bytes = g_bytes_new_take (g_steal_pointer (&decoded), length);

      g_signal_emit (self, signals[SIG_PDF_CHUNK], 0,
                     (int) event_int (object, "id"), bytes);
      g_bytes_unref (bytes);
      return;
    }

  if (!strcmp (event, "pdf-end"))
    {
      g_signal_emit (self, signals[SIG_PDF_END], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "bytes"),
                     event_flag (object, "truncated"),
                     event_string (object, "error"));
      return;
    }

  if (!strcmp (event, "favicon"))
    {
      JsonArray *urls = json_object_has_member (object, "urls")
                          ? json_object_get_array_member (object, "urls")
                          : NULL;
      guint n = urls ? json_array_get_length (urls) : 0;
      g_autofree const char **list = g_new0 (const char *, n + 1);

      /* The strings belong to the parsed event, which lives exactly as long as
       * the emission; the strv itself may be kept. */
      for (guint i = 0; i < n; i++)
        list[i] = json_array_get_string_element (urls, i);
      g_signal_emit (self, signals[SIG_FAVICON_CHANGED], 0,
                     (int) event_int (object, "browser"), list);
      return;
    }

  if (!strcmp (event, "load-started"))
    {
      g_signal_emit (self, signals[SIG_LOAD_STARTED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "url"),
                     (int) event_int (object, "transition"),
                     event_flag (object, "is_main_frame"));
      return;
    }

  if (!strcmp (event, "load-end"))
    {
      g_signal_emit (self, signals[SIG_LOAD_END], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "url"),
                     (int) event_int (object, "http_code"));
      return;
    }

  if (!strcmp (event, "load-error"))
    {
      g_warning ("session: load error %d for %s: %s",
                 (int) event_int (object, "error_code"),
                 event_string (object, "url"),
                 event_string (object, "message"));

      /* Exactly one error per failed navigation — the helper already drops
         the ERR_ABORTED echo Chromium sends behind the real one — and the
         ::load-end with HTTP code 0 that closes the same navigation still
         follows, so a host that only listens there cannot tell "loaded" from
         "did not": this signal is the only place the reason is. */
      g_signal_emit (self, signals[SIG_LOAD_ERROR], 0,
                     (int) event_int (object, "browser"),
                     (int) event_int (object, "error_code"),
                     event_string (object, "url"),
                     event_string (object, "message"));
      return;
    }

  if (!strcmp (event, "renderer-exited"))
    {
      /* Nothing else reports this. The surface keeps showing the last frame and
       * every command after it goes unanswered, so a host that does not listen
       * here sees a page that stopped moving rather than a dead process. */
      g_signal_emit (self, signals[SIG_RENDERER_EXITED], 0,
                     (int) event_int (object, "status"),
                     (int) event_int (object, "error_code"),
                     event_string (object, "message"));
      return;
    }

  if (!strcmp (event, "page-message"))
    {
      /* The page pushed a message through cefnestPostMessage: the payload is
       * one opaque JSON string — the host owns whatever schema lives inside
       * it. Oversized payloads were already dropped renderer-side (the same
       * 64 KiB cap the js result relay uses). */
      g_signal_emit (self, signals[SIG_PAGE_MESSAGE], 0,
                     event_string (object, "json"));
      return;
    }

  if (!strcmp (event, "plugin-menu-command"))
    {
      /* A plugin's context-menu row was picked. The routing goes through the
       * manager's loaded natives, not a signal: the row belongs to whichever
       * .so registered that slot, and its callback is the only receiver. A
       * slot nothing loaded claims is dropped — the helper can report a row
       * this side disabled a moment ago. */
      JsonObject *params = NULL;

      if (json_object_has_member (object, "params"))
        {
          JsonNode *node = json_object_get_member (object, "params");

          if (JSON_NODE_HOLDS_OBJECT (node))
            params = json_node_get_object (node);
        }
      /* The Web frontend's slots are the ones it allocated itself; anything
         it does not claim belongs to a native plugin's registry. A session
         that never built a manager drops the row — nothing routes it. */
      if (self->plugin_manager &&
          !chromium_plugin_web_deliver_menu (self->plugin_manager,
                                             event_int (object, "browser"),
                                             event_int (object, "slot"),
                                             params))
        chromium_plugin_native_dispatch_menu (self->plugin_manager,
                                              event_int (object, "slot"),
                                              params);
      return;
    }

  if (!strcmp (event, "plugin-message"))
    {
      if (self->plugin_manager)
        chromium_plugin_web_handle_message (self->plugin_manager,
                                            event_int (object, "browser"),
                                            event_int (object, "reply_id"),
                                            event_string (object, "id"),
                                            event_string (object, "api"),
                                            event_string (object, "args"));
      return;
    }

  if (!strcmp (event, "browsers"))
    {
      if (self->plugin_manager && json_object_has_member (object, "browsers"))
        {
          JsonNode *node = json_object_get_member (object, "browsers");

          if (JSON_NODE_HOLDS_ARRAY (node))
            chromium_plugin_web_handle_browsers (self->plugin_manager,
                                                 event_int (object, "id"),
                                                 json_node_get_array (node));
        }
      return;
    }

  if (!strcmp (event, "plugin-error"))
    {
      /* Helper-side failures: a script file that could not be read. The
         manager's own signal is where every other plugin problem already
         goes, so this one takes the same door. */
      if (self->plugin_manager)
        g_signal_emit_by_name (self->plugin_manager, "plugin-error",
                               event_string (object, "id"),
                               event_string (object, "message"));
      return;
    }

  if (!strcmp (event, "primary-selection"))
    {
      /* CEF on Wayland reads primary selection but does not create a source
       * for a page selection. The injected renderer listener pushes the text,
       * and the host owns the Gtk primary selection instead; the compositor's
       * primary bridge then offers it to other clients. */
      const char *text = event_string (object, "text");
      if (self->view)
        {
          GdkClipboard *primary =
            gtk_widget_get_primary_clipboard (GTK_WIDGET (self->view));
          if (primary)
            gdk_clipboard_set_text (primary, text ? text : "");
        }
      g_signal_emit (self, signals[SIG_PRIMARY_SELECTION], 0,
                     text ? text : "");
      return;
    }

  if (!strcmp (event, "popup-requested"))
    {
      /* The helper navigates the asking browser in place for every foreground
       * request; background tabs are reported and left to the host. The
       * disposition, gesture and asking browser are what a host's tab UI
       * reads to tell a middle-click background tab from a target=_blank
       * foreground one. */
      g_signal_emit (self, signals[SIG_POPUP_REQUESTED], 0,
                     event_string (object, "url"),
                     (int) event_int (object, "disposition"),
                     event_flag (object, "gesture"),
                     (int) event_int (object, "browser"));
      return;
    }

  if (!strcmp (event, "copy-text"))
    {
      g_signal_emit (self, signals[SIG_COPY_TEXT], 0,
                     event_string (object, "text"));
      return;
    }

  if (!strcmp (event, "open-url"))
    {
      g_signal_emit (self, signals[SIG_OPEN_URL], 0,
                     event_string (object, "url"));
      return;
    }

  if (!strcmp (event, "zoom"))
    {
      /* The session's cached level is the active browser's: a background
         tab's zoom lands in the browsers table, not here. */
      int browser = (int) event_int (object, "browser");
      if (browser != 0 && self->active_browser_id != 0 &&
          browser != self->active_browser_id)
        return;
      self->zoom_level = event_number (object, "level");
      g_signal_emit (self, signals[SIG_ZOOM_CHANGED], 0, self->zoom_level);
      return;
    }

  if (!strcmp (event, "muted"))
    {
      /* The helper, not the caller, is the one that read CefBrowserHost back.
       * Nothing about the page changes when this happens, so ::changed is the
       * only place a host can see it land. The event names the browser it
       * muted: a background tab going quiet says nothing about the view the
       * host is showing. */
      int browser = (int) event_int (object, "browser");
      gboolean muted = event_flag (object, "muted");

      if (browser != 0 && self->active_browser_id != 0 &&
          browser != self->active_browser_id)
        return;
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
                     (int) event_int (object, "id"),
                     event_flag (object, "ok"),
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
                     (int) event_int (object, "id"),
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
                     (int) event_int (object, "id"),
                     event_bytes (object, "received_bytes"),
                     event_bytes (object, "total_bytes"),
                     (int) event_int (object, "percent"),
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
                     (int) event_int (object, "id"),
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
                     (int) event_int (object, "identifier"),
                     (int) event_int (object, "count"),
                     (int) event_int (object, "active_ordinal"),
                     event_flag (object, "final"));
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

  if (!strcmp (event, "js-dialog"))
    {
      /* The page's script is stopped until this is answered. The host answers
       * if it takes the signal; otherwise the Chrome default (ok for alert and
       * beforeunload, cancel for confirm and prompt) is sent here. */
      handle_js_dialog (self, object);
      return;
    }

  if (!strcmp (event, "certificate-error"))
    {
      handle_certificate_error (self, object);
      return;
    }

  if (!strcmp (event, "certificate-error-closed"))
    {
      handle_certificate_closed (self, object);
      return;
    }

  if (!strcmp (event, "auth-required"))
    {
      handle_auth_required (self, object);
      return;
    }

  if (!strcmp (event, "auth-closed"))
    {
      handle_auth_closed (self, object);
      return;
    }

  if (!strcmp (event, "client-cert-request"))
    {
      handle_client_cert_request (self, object);
      return;
    }

  if (!strcmp (event, "client-cert-closed"))
    {
      handle_client_cert_closed (self, object);
      return;
    }

  if (!strcmp (event, "tls-info"))
    {
      handle_tls_info (self, object);
      return;
    }

  if (!strcmp (event, "scheme-request"))
    {
      handle_scheme_request (self, object);
      return;
    }

  if (!strcmp (event, "scheme-closed"))
    {
      handle_scheme_closed (self, object);
      return;
    }

  if (!strcmp (event, "screenshot-result"))
    {
      handle_screenshot_result (self, object);
      return;
    }

  if (!strcmp (event, "window-properties"))
    {
      handle_window_properties (self, object);
      return;
    }

  if (!strcmp (event, "browsers-changed"))
    {
      handle_browsers_changed (self, object);
      return;
    }

  if (!strcmp (event, "request-intercept"))
    {
      handle_request_intercept (self, object);
      return;
    }

  if (!strcmp (event, "request-intercept-closed"))
    {
      handle_request_intercept_closed (self, object);
      return;
    }

  if (!strcmp (event, "cdp-result"))
    {
      handle_cdp_result (self, object);
      return;
    }

  if (!strcmp (event, "cdp-event"))
    {
      handle_cdp_event (self, object);
      return;
    }

  if (!strcmp (event, "js-dialog-closed"))
    {
      /* Navigation or the browser itself took the dialog away. The host's own
       * panel has nothing left to answer. */
      handle_js_dialog_closed (self, object);
      return;
    }

  if (!strcmp (event, "file-dialog"))
    {
      /* A chooser is open on the page's behalf; only the host can draw one. */
      handle_file_dialog (self, object);
      return;
    }

  if (!strcmp (event, "file-dialog-closed"))
    {
      handle_file_dialog_closed (self, object);
      return;
    }

  if (!strcmp (event, "loading-progress"))
    {
      g_signal_emit (self, signals[SIG_LOADING_PROGRESS], 0,
                     event_number (object, "progress"));
      return;
    }

  if (!strcmp (event, "console"))
    {
      g_signal_emit (self, signals[SIG_CONSOLE], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "level"),
                     event_string (object, "message"),
                     event_string (object, "source"),
                     (int) event_int (object, "line"));
      return;
    }

  if (!strcmp (event, "tooltip-changed"))
    {
      g_signal_emit (self, signals[SIG_TOOLTIP_CHANGED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "text"));
      return;
    }

  if (!strcmp (event, "status-message"))
    {
      g_signal_emit (self, signals[SIG_STATUS_MESSAGE], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "text"));
      return;
    }

  if (!strcmp (event, "focus-requested"))
    {
      g_signal_emit (self, signals[SIG_FOCUS_REQUESTED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "source"));
      return;
    }

  if (!strcmp (event, "media-access-changed"))
    {
      g_signal_emit (self, signals[SIG_MEDIA_ACCESS_CHANGED], 0,
                     (int) event_int (object, "browser"),
                     event_flag (object, "video"),
                     event_flag (object, "audio"));
      return;
    }

  if (!strcmp (event, "render-unresponsive"))
    {
      g_signal_emit (self, signals[SIG_RENDER_UNRESPONSIVE], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "browser"));
      return;
    }

  if (!strcmp (event, "render-responsive"))
    {
      g_signal_emit (self, signals[SIG_RENDER_RESPONSIVE], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "browser"));
      return;
    }

  if (!strcmp (event, "render-unresponsive-closed"))
    {
      g_signal_emit (self, signals[SIG_RENDER_UNRESPONSIVE_CLOSED], 0,
                     (int) event_int (object, "id"),
                     (int) event_int (object, "browser"));
      return;
    }

  if (!strcmp (event, "frame-created"))
    {
      g_signal_emit (self, signals[SIG_FRAME_CREATED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "frame"),
                     event_flag (object, "is_main"),
                     event_string (object, "url"));
      return;
    }

  if (!strcmp (event, "frame-destroyed"))
    {
      g_signal_emit (self, signals[SIG_FRAME_DESTROYED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "frame"));
      return;
    }

  if (!strcmp (event, "frame-attached"))
    {
      g_signal_emit (self, signals[SIG_FRAME_ATTACHED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "frame"),
                     event_flag (object, "reattached"));
      return;
    }

  if (!strcmp (event, "frame-detached"))
    {
      g_signal_emit (self, signals[SIG_FRAME_DETACHED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "frame"));
      return;
    }

  if (!strcmp (event, "main-frame-changed"))
    {
      g_signal_emit (self, signals[SIG_MAIN_FRAME_CHANGED], 0,
                     (int) event_int (object, "browser"),
                     event_string (object, "old_frame"),
                     event_string (object, "new_frame"));
      return;
    }

  if (!strcmp (event, "print-requested"))
    {
      g_signal_emit (self, signals[SIG_PRINT_REQUESTED], 0,
                     (int) event_int (object, "browser"));
      return;
    }

  if (!strcmp (event, "fullscreen"))
    {
      g_signal_emit (self, signals[SIG_FULLSCREEN_CHANGED], 0,
                     event_flag (object, "fullscreen"));
      return;
    }

  if (!strcmp (event, "auto-resize"))
    {
      const int browser = (int) event_int (object, "browser");
      const int width = (int) event_int (object, "width");
      const int height = (int) event_int (object, "height");

      session_apply_auto_resize (self, browser, width, height);
      g_signal_emit (self, signals[SIG_AUTO_RESIZE_UPDATED], 0,
                     browser, width, height);
      return;
    }

  if (!strcmp (event, "status"))
    {
      g_message ("session: helper status: %s",
                 event_string (object, "message"));
      return;
    }

  if (!strcmp (event, "closed"))
    {
      /* The helper's own words that it is on its way out; the process is still
       * running for a moment after this line. */
      self->helper_is_exiting = TRUE;
      mark_closed (self, "helper closed the browser");
      return;
    }

  /* The raw line rather than just the name: an event this version does not
   * know is the normal state of a helper newer than its host, and the only way
   * to find out what arrived is to see it. */
  g_warning ("session: unknown event: %s", line);
}

static void drop_connection (ChromiumSession *self);

static void
line_arrived (ChromiumSession *self, const char *line, gsize length)
{
  /* A NUL in the middle of a line is not a line with a NUL in it: JSON has no
   * way to carry one, so whatever wrote this is not speaking this protocol.
   * Handing it to a string-based parser would answer with the bytes before the
   * NUL and call that an event, which is the one outcome worse than refusing —
   * it is wrong and it looks right. The boundary is known, so this line goes and
   * the stream after it stays. */
  if (length != strlen (line))
    {
      g_warning ("session: control line holds a NUL at %zu of %zu, which JSON "
                 "cannot carry: the line is dropped, not truncated",
                 (size_t) strnlen (line, length), length);
      return;
    }

  handle_event (self, line);
}

/* Append what just arrived and take every whole line out of it. Returns FALSE
 * only if the partial line passed the ceiling, which means the far side is not
 * framing anything and reading more of it is not worth the memory. */
static gboolean
feed_control_bytes (ChromiumSession *self, const guint8 *data, gsize length)
{
  GString *pending = self->line_pending;
  /* Leftover bytes hold no newline — every complete line was taken before the
   * last return — so only what this chunk added needs scanning, and the
   * consumed lines are cut out by index with the tail moved once per chunk.
   * Erasing per line made a long js-result arriving in 4 KiB reads pay a full
   * memmove of everything after it at every line-sized step. */
  gsize scan = pending->len;
  gsize head = 0;

  g_string_append_len (pending, (const char *) data, length);

  for (;;)
    {
      const char *nl = memchr (pending->str + scan, '\n', pending->len - scan);
      g_autofree char *line = NULL;
      gsize end;

      if (!nl)
        break;

      end = (gsize) (nl - pending->str);
      line = g_strndup (pending->str + head, end - head);
      line_arrived (self, line, end - head);

      /* handle_event() can close the channel from under this — a "shutdown"
       * event, or anything it sends that fails — so the buffer it owns cannot be
       * touched again without asking whether it is still there. It cannot be
       * *replaced* here: attach_connection() runs only from the accept callback,
       * which refuses a second connection while this one is being read. */
      if (!self->line_pending)
        return TRUE;

      head = end + 1;
      scan = head;
    }

  if (head > 0)
    g_string_erase (pending, 0, head);

  if (pending->len > CONTROL_LINE_MAX)
    {
      g_warning ("session: %zu bytes with no newline, over the %u byte ceiling: "
                 "the helper is not framing control events",
                 pending->len, CONTROL_LINE_MAX);
      return FALSE;
    }

  return TRUE;
}

static void read_chunk (ChromiumSession *self);

static void
read_chunk_done (GObject *source, GAsyncResult *result, gpointer data)
{
  ChromiumSession *self = data;
  gsize length = 0;
  GError *error = NULL;
  GBytes *bytes;
  gboolean keep_reading = TRUE;

  bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !self->stopping)
        g_warning ("session: control read failed: %s", error->message);
      g_clear_error (&error);

      /* A failed read is the end of this channel. Re-arming the read on a
       * stream that is already dead would return EOF immediately and arm again,
       * forever: the channel has to be dropped here, not left for the child to
       * be reaped. */
      keep_reading = FALSE;
      drop_connection (self);
      mark_closed (self, "control read failed");
    }
  else if (bytes)
    {
      const guint8 *data = g_bytes_get_data (bytes, &length);

      if (length == 0)
        {
          keep_reading = FALSE;
          /* EOF from the helper's end of the socket: the process is closing up
           * shop, not dead yet. */
          self->helper_is_exiting = TRUE;
          drop_connection (self);
          mark_closed (self, "control channel closed");
        }
      else
        {
          keep_reading = feed_control_bytes (self, data, length);
          if (!keep_reading)
            {
              drop_connection (self);
              mark_closed (self, "control line too long");
            }
        }

      g_bytes_unref (bytes);
    }
  else
    {
      keep_reading = FALSE;
      drop_connection (self);
      mark_closed (self, "control channel closed");
    }

  if (keep_reading && self->input)
    read_chunk (self);

  g_object_unref (self);
}

static void
read_chunk (ChromiumSession *self)
{
  g_input_stream_read_bytes_async (self->input, CONTROL_CHUNK, G_PRIORITY_DEFAULT,
                                   self->cancellable, read_chunk_done,
                                   g_object_ref (self));
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

  /* Anything still unframed belongs to the channel that just went away: a
   * helper that replaces this one starts its own lines from the beginning. */
  if (self->line_pending)
    g_string_free (self->line_pending, TRUE);
  self->line_pending = NULL;

  g_clear_object (&self->connection);
}

static void
attach_connection (ChromiumSession *self, GSocketConnection *connection)
{
  self->connection = g_object_ref (connection);
  self->output = g_object_ref (g_io_stream_get_output_stream (G_IO_STREAM (connection)));
  self->input = g_object_ref (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
  self->line_pending = g_string_new (NULL);

  /* The helper numbers its permission requests and dialogs from one again, so an
   * id left over from a previous helper would answer a different request than it
   * belongs to. */
  g_hash_table_remove_all (self->permission_held);
  g_hash_table_remove_all (self->js_dialog_held);
  g_hash_table_remove_all (self->file_dialog_held);
  g_hash_table_remove_all (self->certificate_held);
  g_hash_table_remove_all (self->auth_held);
  g_hash_table_remove_all (self->client_cert_held);
  g_hash_table_remove_all (self->scheme_held);
  g_hash_table_remove_all (self->intercept_held);
  /* A fresh helper means fresh browser ids: the pushed table has nothing
     left to say about the old ones. The version left the channel with it. */
  if (self->browser_table)
    g_hash_table_remove_all (self->browser_table);
  self->helper_version = 0;

  read_chunk (self);
}

static void accept_one (ChromiumSession *self);

/* EMFILE is the failure this is for: the listener is still listening, the socket
 * is still there, and the next accept will say no again for the same reason until
 * a descriptor comes free. Armed straight away, that is a loop that never sleeps,
 * and it is the one main loop everything in this process shares — so the visible
 * symptom of running out of descriptors is a page that stops drawing. */
#define ACCEPT_RETRY_DELAY 200u

static void
accept_retry_fired (gpointer data)
{
  ChromiumSession *self = data;

  self->accept_retry_id = 0;
  accept_one (self);
}

static void
accepted (GObject *source, GAsyncResult *result, gpointer data)
{
  ChromiumSession *self = data;
  GSocketConnection *connection;
  GError *error = NULL;

  /* A listener closed by stop() or by a restart completes its accept after the
   * field has moved on. That result belongs to the old run; finishing it here
   * is what releases the async operation, but it must not become this
   * session's connection or close a listener started since. */
  if (!self->listener || source != G_OBJECT (self->listener))
    {
      connection = g_socket_listener_accept_finish (G_SOCKET_LISTENER (source),
                                                    result, NULL, &error);
      if (connection)
        g_object_unref (connection);
      if (error)
        g_clear_error (&error);
      g_object_unref (self);
      return;
    }

  connection = g_socket_listener_accept_finish (G_SOCKET_LISTENER (source),
                                                result, NULL, &error);
  if (!connection)
    {
      if (!self->stopping &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("session: accept failed: %s", error->message);

          /* EMFILE/ENFILE is the one class worth another try: descriptors come
           * back, and the listener is otherwise healthy. A permanent failure
           * (the runtime directory removed, permissions changed) would just
           * repeat every 200 ms and flood the log at 5 Hz, so it ends the
           * session instead. */
          if (g_error_matches (error, G_IO_ERROR,
                               G_IO_ERROR_TOO_MANY_OPEN_FILES))
            {
              if (self->accept_retry_id == 0)
                self->accept_retry_id =
                  g_timeout_add_once (ACCEPT_RETRY_DELAY, accept_retry_fired,
                                      self);
            }
          else
            {
              g_clear_handle_id (&self->accept_retry_id, g_source_remove);
              mark_closed (self, "control listener failed");
            }
        }
      g_clear_error (&error);
    }
  else if (self->connection)
    {
      /* One page per widget: a second helper would fight the first over the
       * compositor's single toplevel slot. */
      g_warning ("session: ignoring a second control connection");
      g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
      g_object_unref (connection);
      accept_one (self);
    }
  else
    {
      attach_connection (self, connection);
      accept_one (self);
    }

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
  g_autofree char *json = NULL;
  GError *error = NULL;
  gsize json_len = 0;
  size_t done = 0;
  size_t total;
  gssize written;
  int seg;

  if (!self->output)
    {
      g_warning ("session: no control channel to send on");
      return;
    }

  generator = json_generator_new ();
  json_generator_set_root (generator, root);

  /* NDJSON: the helper's reader only dispatches on a newline. The newline goes
   * out as its own one-byte segment below — g_strconcat copying the whole line
   * and strlen rescanning it existed only to append that byte. */
  json = json_generator_to_data (generator, &json_len);

  /* Two segments: the body, then the newline. One call is not one line. A
   * stream is allowed to take part of a write, and the remainder of a
   * half-written command would be read as the beginning of the next one — the
   * framing does not come back on its own, so everything that is left goes
   * round again until the segment is out. */
  for (seg = 0; seg < 2; seg++)
    {
      const char *data = seg == 0 ? json : "\n";

      total = seg == 0 ? json_len : 1;
      done = 0;
      while (done < total)
        {
          written = g_output_stream_write (self->output, data + done,
                                           total - done, NULL, &error);
          if (written < 0)
            {
              g_warning ("session: control write failed: %s", error->message);
              g_clear_error (&error);
              /* Half a line is on the wire. The peer can no longer tell where the
               * next command starts, so this channel is done: drop it and report
               * the helper gone, exactly as a read-side framing failure does. */
              drop_connection (self);
              mark_closed (self, "control write failed");
              return;
            }
          if (written == 0)
            {
              g_warning ("session: control write stopped after %zu of %zu bytes",
                         done, total);
              drop_connection (self);
              mark_closed (self, "control write stalled");
              return;
            }
          done += (size_t) written;
        }
    }

  if (!g_output_stream_flush (self->output, NULL, &error))
    {
      g_warning ("session: control flush failed: %s", error->message);
      g_clear_error (&error);
      drop_connection (self);
      mark_closed (self, "control flush failed");
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
          /* The "&(B)" spelling. */
          g_string_append_printf (out, "(_%c)", p[2]);
          p += 3;
        }
      else if (*p == '&' && g_ascii_isalnum (p[1]) && p[2] == ')')
        {
          /* CEF's actual spelling is "(&B)": the ampersand is inside the
           * parentheses, so turn it into GTK's "(_B)" mnemonic form. */
          g_string_append_c (out, '_');
          g_string_append_c (out, p[1]);
          g_string_append_c (out, ')');
          p += 2;
        }
      else
        g_string_append_c (out, *p);
    }

  return g_string_free (out, FALSE);
}

static void menu_release      (ChromiumSession *self);
static void menu_release_soon (ChromiumSession *self);
static void menu_popover_closed (GtkPopover *popover, gpointer data);

/* A popover takes keyboard focus while it is mapped. If its focus is still
 * inside the menu when we tear it down, put it back on the view: that is what
 * makes the nested seat keep sending keys to the page. If the user clicked the
 * address bar or another host widget, leave that focus alone. */
static void
menu_restore_view_focus (ChromiumSession *self, GtkWidget *popover)
{
  GtkRoot *root;
  GtkWidget *focus;

  if (!self->view)
    return;

  root = gtk_widget_get_root (GTK_WIDGET (self->view));
  if (!root)
    return;

  focus = gtk_root_get_focus (root);
  if (focus == NULL ||
      (popover && (focus == popover ||
                   gtk_widget_is_ancestor (focus, popover))))
    gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

/* The menu is deliberately non-modal: GtkPopover's autohide is what grabs the
 * keyboard and makes the page lose focus. A context menu is a pointer gesture,
 * so the page keeps focus and these three guards close it instead. */
static gboolean
menu_outside_event (GtkEventControllerLegacy *controller, GdkEvent *event,
                    gpointer data)
{
  ChromiumSession *self = data;
  GdkSurface *popover_surface;

  if (!self->menu_popover)
    return FALSE;
  if (gdk_event_get_event_type (event) != GDK_BUTTON_PRESS)
    return FALSE;

  /* A click on the popover's own surface belongs to its buttons. */
  popover_surface = gtk_native_get_surface (GTK_NATIVE (self->menu_popover));
  if (popover_surface && gdk_event_get_surface (event) == popover_surface)
    return FALSE;

  chromium_session_context_menu_dismiss (self);
  /* Do not also click the page/link/toolbar under the dismissing click. */
  return TRUE;
}

static GPtrArray *
menu_button_list (ChromiumSession *self)
{
  GPtrArray *buttons = g_ptr_array_new ();
  GtkWidget *child;

  if (!self->menu_box)
    return buttons;
  for (child = gtk_widget_get_first_child (self->menu_box); child;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_BUTTON (child) && gtk_widget_get_sensitive (child))
      g_ptr_array_add (buttons, child);
  return buttons;
}

static void
menu_set_selected (ChromiumSession *self, GtkWidget *button)
{
  if (self->menu_selected == button)
    return;
  if (self->menu_selected)
    gtk_widget_remove_css_class (self->menu_selected, "cefnest-menu-selected");
  self->menu_selected = button;
  if (button)
    gtk_widget_add_css_class (button, "cefnest-menu-selected");
}

static void
menu_move_selection (ChromiumSession *self, gboolean forward)
{
  g_autoptr (GPtrArray) buttons = menu_button_list (self);
  guint i, next = 0;

  if (buttons->len == 0)
    return;

  if (!self->menu_selected)
    next = forward ? 0 : buttons->len - 1;
  else
    {
      for (i = 0; i < buttons->len; i++)
        if (g_ptr_array_index (buttons, i) == self->menu_selected)
          break;
      if (i == buttons->len)
        next = forward ? 0 : buttons->len - 1;
      else if (forward)
        next = (i + 1) % buttons->len;
      else
        next = (i + buttons->len - 1) % buttons->len;
    }

  menu_set_selected (self, g_ptr_array_index (buttons, next));
}

static void
menu_select_edge (ChromiumSession *self, gboolean last)
{
  g_autoptr (GPtrArray) buttons = menu_button_list (self);

  if (buttons->len == 0)
    return;
  menu_set_selected (self, g_ptr_array_index (buttons, last ? buttons->len - 1 : 0));
}

static void
menu_activate_selected (ChromiumSession *self)
{
  if (!self->menu_selected)
    menu_move_selection (self, TRUE);
  if (self->menu_selected && gtk_widget_get_sensitive (self->menu_selected))
    g_signal_emit_by_name (self->menu_selected, "clicked");
}

static gboolean
menu_key_pressed (GtkEventControllerKey *controller, guint keyval,
                  guint keycode, GdkModifierType state, gpointer data)
{
  ChromiumSession *self = data;

  if (!self->menu_popover)
    return FALSE;

  if (keyval == GDK_KEY_Escape)
    {
      chromium_session_context_menu_dismiss (self);
      return TRUE;
    }

  /* Leave page shortcuts alone; only bare navigation keys reach the menu. */
  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
    return FALSE;

  switch (keyval)
    {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      menu_move_selection (self, FALSE);
      return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      menu_move_selection (self, TRUE);
      return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      menu_select_edge (self, FALSE);
      return TRUE;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      menu_select_edge (self, TRUE);
      return TRUE;
    case GDK_KEY_Tab:
      menu_move_selection (self, !(state & GDK_SHIFT_MASK));
      return TRUE;
    case GDK_KEY_ISO_Left_Tab:
      menu_move_selection (self, FALSE);
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
    case GDK_KEY_KP_Space:
      menu_activate_selected (self);
      return TRUE;
    default:
      return FALSE;
    }
}

static void
menu_window_active_changed (GtkWindow *window, GParamSpec *pspec, gpointer data)
{
  ChromiumSession *self = data;

  if (self->menu_popover && !gtk_window_is_active (window))
    chromium_session_context_menu_dismiss (self);
}

static void
menu_install_guards (ChromiumSession *self)
{
  GtkRoot *root;
  GtkEventController *controller;

  if (!self->view)
    return;
  root = gtk_widget_get_root (GTK_WIDGET (self->view));
  if (!GTK_IS_WINDOW (root))
    return;

  self->menu_window = g_object_ref (GTK_WINDOW (root));

  controller = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "event", G_CALLBACK (menu_outside_event), self);
  gtk_widget_add_controller (GTK_WIDGET (root), controller);
  self->menu_outside_controller = g_object_ref (controller);

  controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "key-pressed",
                    G_CALLBACK (menu_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (root), controller);
  self->menu_key_controller = g_object_ref (controller);

  self->menu_active_handler =
    g_signal_connect (root, "notify::active",
                      G_CALLBACK (menu_window_active_changed), self);
}

static void
menu_remove_guards (ChromiumSession *self)
{
  if (self->menu_window)
    {
      if (self->menu_active_handler != 0)
        g_signal_handler_disconnect (self->menu_window, self->menu_active_handler);
      self->menu_active_handler = 0;

      if (self->menu_outside_controller)
        {
          gtk_widget_remove_controller (GTK_WIDGET (self->menu_window),
                                        self->menu_outside_controller);
          g_clear_object (&self->menu_outside_controller);
        }
      if (self->menu_key_controller)
        {
          gtk_widget_remove_controller (GTK_WIDGET (self->menu_window),
                                        self->menu_key_controller);
          g_clear_object (&self->menu_key_controller);
        }
      g_clear_object (&self->menu_window);
    }
}

/* Put the menu's widgets back. The popover is reached both from the answer path
 * and from its own ::closed, so this has to survive being called twice. */
/* grab_focus() from inside the popover teardown can be refused while the
 * popover still owns the keyboard; wait one idle, after the surface is gone,
 * and only then hand it back if nothing else took it. */
static void
menu_focus_restore_idle (gpointer data)
{
  ChromiumSession *self = data;

  self->menu_focus_source = 0;
  menu_restore_view_focus (self, NULL);
  g_object_unref (self);
}

static void
menu_restore_focus_soon (ChromiumSession *self)
{
  if (self->menu_focus_source > 0)
    return;

  self->menu_focus_source =
    g_timeout_add_once (50, menu_focus_restore_idle, g_object_ref (self));
}

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

  menu_remove_guards (self);

  if (popover)
    {
      /* This side is the one putting the menu down, so the answer to the helper is
       * already decided and ::closed must not also send one. */
      g_signal_handlers_disconnect_by_func (popover,
                                            G_CALLBACK (menu_popover_closed), self);
      if (GTK_IS_WIDGET (gtk_widget_get_parent (popover)))
        gtk_widget_unparent (popover);
      g_object_unref (popover);
      self->menu_box = NULL;
      self->menu_selected = NULL;

      /* Hand the keyboard back after the popover (and its focus widget) is
       * gone; otherwise the page keeps the last frame but stops taking keys. */
      menu_restore_focus_soon (self);
    }

}

/* Not from inside the popover's own ::closed, and not from inside the activate of
 * an action this menu owns: both would free the widget GTK is still emitting on,
 * so the teardown waits for one idle. */
static void
menu_release_later (gpointer data)
{
  ChromiumSession *self = data;

  self->menu_release_source = 0;

  /* The popover's ::closed can arrive before the row's action activate (the
   * menu pops down and GTK delivers the activation afterwards). Waiting one
   * idle lets the activation answer while the request is still pending; a
   * closed menu with no activation is then the dismissal CEF expects. */
  if (self->menu_pending)
    {
      self->menu_pending = FALSE;
      send_command (self, "context-menu-dismiss");
    }

  menu_release (self);
  g_object_unref (self);
}

static void
menu_release_soon (ChromiumSession *self)
{
  if (self->menu_release_source > 0 || !self->menu_popover)
    return;

  self->menu_release_source =
    g_timeout_add_once (0, menu_release_later, g_object_ref (self));
}

static void
menu_popover_closed (GtkPopover *popover, gpointer data)
{
  ChromiumSession *self = data;

  /* Do not answer here: an action activated by this same click may still be on
   * its way. menu_release_later() decides once the current event has drained. */
  menu_release_soon (self);
}

/* The theme puts 8px of padding on a popover's contents; for a one-row copy
 * menu that reads as a blank strip under the only row. The content class and a
 * selector at the same specificity as the theme's override it locally. */
static GtkCssProvider *menu_css;

static void
menu_ensure_css (GtkWidget *view)
{
  GdkDisplay *display;

  if (menu_css)
    return;

  menu_css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (
    menu_css,
    "popover.cefnest-menu > contents.cefnest-menu-content { padding: 0; }"
    ".cefnest-menu-box { padding: 4px; }"
    ".cefnest-menu-box button { padding: 4px 8px; }"
    ".cefnest-menu-box button.cefnest-menu-selected {"
    "  background-color: alpha(currentColor, 0.12); }"
    ".cefnest-menu-box separator { margin: 3px 0; }");
  display = gtk_widget_get_display (view);
  if (display)
    gtk_style_context_add_provider_for_display (
      display, GTK_STYLE_PROVIDER (menu_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
menu_button_clicked (GtkButton *button, gpointer data)
{
  ChromiumSession *self = data;
  int command = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
                                                     "cefnest-command"));

  chromium_session_context_menu_select (self, command);
}

/* The popover mapped with a real size — the fact test/menu-gate.sh asserts.
 * An idle scheduled from map: map comes before the popover's allocation
 * (measured 0x0 at map), and the idle reads it once the popup has finished
 * presenting. One log line, no timer left behind. */
static void
menu_report_mapped_size (gpointer data)
{
  GtkWidget *popover = data;

  if (GTK_IS_WIDGET (popover))
    g_debug ("session: context menu mapped=%d size=%dx%d",
             gtk_widget_get_mapped (popover),
             gtk_widget_get_width (popover), gtk_widget_get_height (popover));

  g_object_unref (popover);
}

static void
menu_popover_mapped (GtkWidget *popover, gpointer data)
{
  g_idle_add_once (menu_report_mapped_size, g_object_ref (popover));
}

static void
session_menu_show (ChromiumSession *self, int x, int y, GList *items)
{
  GList *l;
  GtkWidget *popover;
  GtkWidget *box;
  GdkRectangle point;
  guint index = 0;
  guint rows = 0;
  gboolean pending_separator = FALSE;

  /* A second context-menu event can arrive while the previous popover is still
   * up: the popdown only schedules its release for an idle, and a navigation
   * can dismiss and re-open in the same dispatch. Put the old one down before
   * replacing the field; otherwise the old idle frees the new popover and the
   * old widget stays parented with no way to take it down. */
  if (self->menu_popover)
    menu_release (self);

  if (!self->view)
    {
      /* Nothing to draw over. Saying so with a dismissal is what keeps the
       * helper's one open request from eating the next right click too. */
      g_debug ("session: no view to draw a context menu over");
      chromium_session_context_menu_dismiss (self);
      menu_release (self);
      return;
    }

  /* A plain GtkPopover with a box, not GtkPopoverMenu: the latter wraps its
   * content in a GtkScrolledWindow and reserves section padding, which leaves
   * an empty strip under a one-row copy menu. A box gives the menu exactly the
   * height of its rows and the same input surface as the rows themselves. */
  popover = gtk_popover_new ();
  gtk_widget_add_css_class (popover, "cefnest-menu");
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "cefnest-menu-box");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_popover_set_child (GTK_POPOVER (popover), box);
  {
    GtkWidget *content = gtk_widget_get_first_child (popover);
    if (content)
      gtk_widget_add_css_class (content, "cefnest-menu-content");
  }

  for (l = items; l; l = l->next, index++)
    {
      ChromiumMenuItem *item = l->data;

      if (!strcmp (item->type, "separator"))
        {
          if (rows > 0)
            pending_separator = TRUE;
          continue;
        }

      /* A submenu row cannot open a submenu in this flat popover. The fake
       * "mixed" model ends with one and an empty label; drawing it as a button
       * leaves exactly the empty strip this menu is supposed to avoid. */
      if (!strcmp (item->type, "submenu") || !item->label || !*item->label)
        continue;

      {
        g_autofree char *label = menu_label (item->label);
        GtkWidget *button;

        if (pending_separator)
          {
            gtk_box_append (GTK_BOX (box),
                            gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
            pending_separator = FALSE;
          }

        button = gtk_button_new_with_mnemonic (label);

        gtk_widget_add_css_class (button, "flat");
        gtk_button_set_use_underline (GTK_BUTTON (button), TRUE);
        gtk_widget_set_halign (button, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand (button, TRUE);
        gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
        gtk_widget_set_sensitive (button, item->enabled);
        g_object_set_data (G_OBJECT (button), "cefnest-command",
                           GINT_TO_POINTER (item->id));
        g_signal_connect (button, "clicked",
                          G_CALLBACK (menu_button_clicked), self);
        gtk_box_append (GTK_BOX (box), button);
        rows++;
      }
    }

  if (rows == 0)
    {
      /* An empty menu is not a menu; dismissing is what frees the request.
       *
       * This popover was built with gtk_popover_new() and has no parent — the
       * set_parent that would sink its floating reference is below, on the path
       * that has rows to show. Measured on GTK 4.18: gtk_widget_unparent() on a
       * parentless widget says nothing at all and leaves the reference exactly
       * where it was, so every empty menu costs a popover, its box and the rows
       * that were built and then skipped. Sink it and put it down. */
      g_object_ref_sink (popover);
      g_object_unref (popover);
      chromium_session_context_menu_dismiss (self);
      menu_release (self);
      return;
    }

  menu_ensure_css (GTK_WIDGET (self->view));

  /* The view owns it as a child; this side keeps a reference of its own because
   * it has to be able to put it down from three different places. */
  gtk_widget_set_parent (popover, GTK_WIDGET (self->view));
  self->menu_popover = g_object_ref (popover);
  self->menu_box = box;
  self->menu_selected = NULL;

  point.x = x;
  point.y = y;
  point.width = 1;
  point.height = 1;
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &point);
  gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_BOTTOM);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  /* Keep the keyboard on the page while the menu is up. GtkPopover's default
   * autohide is also its modal grab; outside clicks/Escape are handled below. */
  gtk_popover_set_autohide (GTK_POPOVER (popover), FALSE);

  /* GTK_POS_BOTTOM anchors the surface according to the popover's halign:
   * CENTER (the default) straddles the pointer, START puts the menu's left edge
   * on the pointer and lets it grow right. That is the context-menu geometry. */
  gtk_widget_set_halign (popover, GTK_ALIGN_START);

  gtk_popover_present (GTK_POPOVER (popover));

  g_signal_connect (popover, "closed", G_CALLBACK (menu_popover_closed), self);
  g_signal_connect (popover, "map", G_CALLBACK (menu_popover_mapped), NULL);

  menu_install_guards (self);
  gtk_popover_popup (GTK_POPOVER (popover));
  g_debug ("session: context menu with %u items drawn at %d,%d", index, x, y);
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

  x = (int) event_int (object, "x");
  y = (int) event_int (object, "y");

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
          item->id = (int) event_int (row, "id");
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
 * standing on the answer. Measured against real Chromium:
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

  id = (int) event_int (object, "id");
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
  int id = (int) event_int (object, "id");
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

/* ------------------------------------------------------------------------ *
 * Dialogs: the page's script is stopped until these are answered            *
 *                                                                          *
 * Both are parked on the helper, so every request that reaches this side has *
 * an id that can still be answered. A host that takes the signal owns the    *
 * answer; one that does not gets Chrome's default, because the alternative   *
 * — leaving the page frozen — is not a state any of these can end on its own. *
 * ------------------------------------------------------------------------ */

static void
js_dialog_respond_send (ChromiumSession *self, int id, gboolean ok,
                        const char *value)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "js-dialog-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_set_member_name (builder, "ok");
  json_builder_add_boolean_value (builder, ok);
  command_add_string (builder, "value", value ? value : "");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_js_dialog (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");
  const char *kind = event_string (object, "kind");
  const char *origin = event_string (object, "origin");
  const char *message = event_string (object, "message");
  const char *default_value = event_string (object, "default_value");

  /* Held before the emission for the same reason as a permission: a host that
   * answers from inside the signal must find the id still there. */
  g_hash_table_add (self->js_dialog_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_JS_DIALOG], 0,
                 id, browser, kind, origin, message, default_value, &handled);
  g_debug ("session: js-dialog %d (%s) from %s, host took it: %d",
           id, kind, origin, handled);

  if (handled)
    return;

  /* A handler that answered synchronously and still returned FALSE has already
   * taken the id out; sending the default would be a second answer. */
  if (!g_hash_table_remove (self->js_dialog_held, GINT_TO_POINTER (id)))
    return;

  /* Chrome's answers for a host that draws nothing: alert and beforeunload let
   * the page continue, confirm and prompt answer "no". The prompt value is empty
   * for the same reason — a cancelled prompt has no input to report. */
  gboolean ok = !strcmp (kind, "alert") || !strcmp (kind, "beforeunload");
  g_debug ("session: js-dialog %d (%s) answered %s by default",
           id, kind, ok ? "ok" : "cancel");
  js_dialog_respond_send (self, id, ok, "");
}

static void
handle_js_dialog_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  /* An id this side already answered (or never held) belongs to a dialog the
   * host has already dealt with; there is no panel left to take down. */
  if (!g_hash_table_remove (self->js_dialog_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: js-dialog %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_JS_DIALOG_CLOSED], 0, id, browser);
}

void
chromium_session_js_dialog_respond (ChromiumSession *self, int id, gboolean ok,
                                    const char *value)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->js_dialog_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no js dialog %d to answer", id);
      return;
    }

  js_dialog_respond_send (self, id, ok, value);
}

static void
certificate_respond_send (ChromiumSession *self, int id, gboolean allow)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "certificate-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_set_member_name (builder, "allow");
  json_builder_add_boolean_value (builder, allow);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* The certificate question has no safe answer the session could give: the
 * Chrome default is deny and the navigation dies with it, so a host that
 * draws nothing gets exactly that — the question is dropped and the page
 * shows its error. The signal exists for the host that wants to ask the
 * person, or that keeps its own fingerprint policy. */
static void
handle_certificate_error (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  g_hash_table_add (self->certificate_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_CERTIFICATE_ERROR], 0,
                 id, browser,
                 event_string (object, "error"),
                 event_string (object, "url"),
                 event_string (object, "subject"),
                 event_string (object, "issuer"),
                 event_string (object, "fingerprint"),
                 &handled);
  g_debug ("session: certificate-error %d from %s, host took it: %d",
           id, event_string (object, "url"), handled);

  if (handled)
    return;
  if (!g_hash_table_remove (self->certificate_held, GINT_TO_POINTER (id)))
    return;

  /* The default is "no" (Chrome's), and it is sent so the helper's parked
     entry goes away with the question. */
  g_debug ("session: certificate-error %d denied by default", id);
  certificate_respond_send (self, id, FALSE);
}

static void
handle_certificate_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->certificate_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: certificate-error %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_CERTIFICATE_ERROR_CLOSED], 0, id, browser);
}

void
chromium_session_certificate_respond (ChromiumSession *self, int id,
                                      gboolean allow)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->certificate_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no certificate error %d to answer", id);
      return;
    }

  certificate_respond_send (self, id, allow);
}

/* The policy half: authorize one fingerprint for this helper's lifetime.
 * Nothing here touches the held table — authorization applies to errors the
 * helper has not asked about yet. */
void
chromium_session_certificate_authorize (ChromiumSession *self,
                                        const char *fingerprint)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (fingerprint != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "certificate-authorize");
  command_add_string (builder, "fingerprint", fingerprint);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_certificate_clear_authorized (ChromiumSession *self)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "certificate-clear-authorized");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* ------------------------------------------------------------------ *
 * HTTP authentication (must-be-answered, default cancel)             *
 * ------------------------------------------------------------------ */

static void
auth_answer_send (ChromiumSession *self, int id, const char *user,
                  const char *password)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "auth-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "user", user ? user : "");
  command_add_string (builder, "password", password ? password : "");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
auth_cancel_send (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "auth-cancel");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* The Chrome default is also the safe one: no host, no credentials — the
 * request fails with its 401 and the page shows that. */
static void
handle_auth_required (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  g_hash_table_add (self->auth_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_AUTH_REQUIRED], 0,
                 id, browser,
                 event_flag (object, "is_proxy"),
                 event_string (object, "scheme"),
                 event_string (object, "realm"),
                 event_string (object, "origin"),
                 &handled);
  g_debug ("session: auth-required %d (%s), host took it: %d",
           id, event_string (object, "origin"), handled);

  if (handled)
    return;
  if (!g_hash_table_remove (self->auth_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: auth-required %d cancelled by default", id);
  auth_cancel_send (self, id);
}

static void
handle_auth_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->auth_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: auth %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_AUTH_CLOSED], 0, id, browser);
}

void
chromium_session_auth_respond (ChromiumSession *self, int id,
                               const char *user, const char *password)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->auth_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no auth request %d to answer", id);
      return;
    }

  auth_answer_send (self, id, user, password);
}

void
chromium_session_auth_cancel (ChromiumSession *self, int id)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->auth_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no auth request %d to answer", id);
      return;
    }

  auth_cancel_send (self, id);
}

/* ------------------------------------------------------------------ *
 * Client certificate selection (must-be-answered, default none)      *
 * ------------------------------------------------------------------ */

void
chromium_session_client_cert_respond (ChromiumSession *self, int id, int index)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->client_cert_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no client-cert request %d to answer", id);
      return;
    }

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "client-cert-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_set_member_name (builder, "index");
  json_builder_add_int_value (builder, index);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_render_unresponsive_respond (ChromiumSession *self,
                                              int id,
                                              ChromiumHangAction action)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "render-unresponsive-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "action",
                      action == CHROMIUM_HANG_TERMINATE ? "terminate"
                                                        : "wait");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_client_cert_request (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  GList *certs = NULL;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");
  JsonArray *array = json_object_has_member (object, "certs")
                       ? json_object_get_array_member (object, "certs") : NULL;
  gsize i;

  if (array)
    {
      for (i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *node = json_array_get_element (array, i);
          ChromiumClientCert *cert;

          if (!JSON_NODE_HOLDS_OBJECT (node))
            continue;
          cert = g_new0 (ChromiumClientCert, 1);
          {
            JsonObject *entry = json_node_get_object (node);
            cert->subject = g_strdup (json_object_has_member (entry, "subject")
                                        ? json_object_get_string_member
                                            (entry, "subject") : "");
            cert->issuer = g_strdup (json_object_has_member (entry, "issuer")
                                       ? json_object_get_string_member
                                           (entry, "issuer") : "");
            cert->fingerprint =
              g_strdup (json_object_has_member (entry, "fingerprint")
                          ? json_object_get_string_member
                              (entry, "fingerprint") : "");
          }
          certs = g_list_append (certs, cert);
        }
    }

  g_hash_table_add (self->client_cert_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_CLIENT_CERT_REQUEST], 0,
                 id, browser,
                 event_string (object, "origin"),
                 certs,
                 &handled);
  g_debug ("session: client-cert-request %d from %s, host took it: %d",
           id, event_string (object, "origin"), handled);

  g_list_free_full (certs, (GDestroyNotify) chromium_client_cert_free);

  if (handled)
    return;
  if (!g_hash_table_remove (self->client_cert_held,
                            GINT_TO_POINTER (id)))
    return;

  /* "None" is the answer that cannot leak a key: no chooser, no choice. */
  g_debug ("session: client-cert-request %d answered none by default", id);
  chromium_session_client_cert_respond (self, id, -1);
}

static void
handle_client_cert_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->client_cert_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: client-cert %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_CLIENT_CERT_CLOSED], 0, id, browser);
}

/* ------------------------------------------------------------------ *
 * TLS info about a loaded page (one read, component's own pipe)      *
 * ------------------------------------------------------------------ */

void
chromium_session_get_tls_info (ChromiumSession *self, int id, int browser)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "tls-info");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_tls_info (ChromiumSession *self, JsonObject *object)
{
  g_signal_emit (self, signals[SIG_TLS_INFO], 0,
                 (int) event_int (object, "id"),
                 (int) event_int (object, "browser"),
                 event_flag (object, "ok"),
                 event_string (object, "security_state"),
                 event_string (object, "subject"),
                 event_string (object, "issuer"),
                 event_string (object, "fingerprint"),
                 (int) event_int (object, "valid_from"),
                 (int) event_int (object, "valid_to"));
}

/* ------------------------------------------------------------------ *
 * Host-served schemes                                                *
 * ------------------------------------------------------------------ */

static void
scheme_entry_free (SchemeEntry *entry)
{
  g_free (entry->name);
  g_free (entry);
}

/* The option flags ride start(); the factory half is live the moment a
   helper exists to hear it, and again on every ::ready. */
static void
scheme_register_send (ChromiumSession *self, const char *name, guint flags)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  struct { const char *name; guint bit; } bits[] = {
    { "local", CHROMIUM_SCHEME_LOCAL },
    { "display_isolated", CHROMIUM_SCHEME_DISPLAY_ISOLATED },
    { "secure", CHROMIUM_SCHEME_SECURE },
    { "cors", CHROMIUM_SCHEME_CORS },
    { "fetch", CHROMIUM_SCHEME_FETCH },
  };

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "scheme-register");
  command_add_string (builder, "name", name);
  if (flags)
    {
      GString *joined = g_string_new ("");
      gboolean first = TRUE;

      for (gsize i = 0; i < G_N_ELEMENTS (bits); i++)
        {
          if (!(flags & bits[i].bit))
            continue;
          if (!first)
            g_string_append_c (joined, ',');
          first = FALSE;
          g_string_append (joined, bits[i].name);
        }
      command_add_string (builder, "flags", joined->str);
      g_string_free (joined, TRUE);
    }
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
scheme_unregister_send (ChromiumSession *self, const char *name)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "scheme-unregister");
  command_add_string (builder, "name", name);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* CHROMIUM_HELPER_SCHEMES: "name=flag,flag;name2". The helper parses this in
   OnRegisterCustomSchemes, in every process — the one point CEF gives scheme
   options a voice. */
static char *
schemes_env_value (ChromiumSession *self)
{
  GString *value = g_string_new ("");

  for (guint i = 0; self->schemes && i < self->schemes->len; i++)
    {
      SchemeEntry *entry = g_ptr_array_index (self->schemes, i);
      struct { const char *name; guint bit; } bits[] = {
        { "local", CHROMIUM_SCHEME_LOCAL },
        { "display_isolated", CHROMIUM_SCHEME_DISPLAY_ISOLATED },
        { "secure", CHROMIUM_SCHEME_SECURE },
        { "cors", CHROMIUM_SCHEME_CORS },
        { "fetch", CHROMIUM_SCHEME_FETCH },
      };
      gboolean first = TRUE;

      if (i > 0)
        g_string_append_c (value, ';');
      g_string_append (value, entry->name);
      for (gsize b = 0; b < G_N_ELEMENTS (bits); b++)
        {
          if (!(entry->flags & bits[b].bit))
            continue;
          g_string_append_c (value, first ? '=' : ',');
          first = FALSE;
          g_string_append (value, bits[b].name);
        }
    }
  return g_string_free (value, FALSE);
}

/* Every scheme in the table, said again to a helper that just told us it is
   ready: the factories are the live half. */
static void
session_send_schemes (ChromiumSession *self)
{
  for (guint i = 0; self->schemes && i < self->schemes->len; i++)
    {
      SchemeEntry *entry = g_ptr_array_index (self->schemes, i);
      scheme_register_send (self, entry->name, entry->flags);
    }
}

static const char *const kSchemeNameBad =
    "a scheme name is [a-z][a-z0-9+.-]* and may not be built-in";

void
chromium_session_scheme_register (ChromiumSession *self, const char *name,
                                  guint flags)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (name != NULL);

  if (!scheme_name_valid (name))
    {
      g_warning ("session: %s: %s", name, kSchemeNameBad);
      return;
    }

  /* One entry per name, in first-registration order: a re-register is an
     update. */
  gboolean found = FALSE;
  for (guint i = 0; self->schemes && i < self->schemes->len; i++)
    {
      SchemeEntry *entry = g_ptr_array_index (self->schemes, i);
      if (g_strcmp0 (entry->name, name) == 0)
        {
          entry->flags = flags;
          found = TRUE;
          break;
        }
    }
  if (!found)
    {
      SchemeEntry *entry = g_new0 (SchemeEntry, 1);
      entry->name = g_strdup (name);
      entry->flags = flags;
      g_ptr_array_add (self->schemes, entry);
    }

  scheme_register_send (self, name, flags);
}

void
chromium_session_scheme_unregister (ChromiumSession *self, const char *name)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (name != NULL);

  for (guint i = 0; self->schemes && i < self->schemes->len; i++)
    {
      SchemeEntry *entry = g_ptr_array_index (self->schemes, i);
      if (g_strcmp0 (entry->name, name) == 0)
        {
          g_ptr_array_remove_index (self->schemes, i);
          break;
        }
    }

  scheme_unregister_send (self, name);
}

static void
scheme_command_send (ChromiumSession *self, const char *cmd, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", cmd);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_scheme_response (ChromiumSession *self, int id, int status,
                                  const char *reason,
                                  const char *content_type,
                                  const char *headers_json)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "scheme-response");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_set_member_name (builder, "status");
  json_builder_add_int_value (builder, status);
  command_add_string (builder, "reason",
                      reason && *reason ? reason : "OK");
  command_add_string (builder, "content_type",
                      content_type && *content_type ? content_type
                                                    : "text/html");
  if (headers_json && *headers_json)
    command_add_string (builder, "headers", headers_json);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_scheme_write (ChromiumSession *self, int id, GBytes *data)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  gconstpointer body;
  gsize length = 0;
  char *encoded;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (data != NULL);

  body = g_bytes_get_data (data, &length);
  /* 48 KiB raw → 64 KiB base64, the PDF stream's shape: one piece stays
     comfortably inside one channel line. A host with a bigger buffer still
     works — this only caps what one command carries. */
  g_return_if_fail (length <= 48 * 1024);

  encoded = g_base64_encode (body, length);
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "scheme-chunk");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "data", encoded);
  json_builder_end_object (builder);
  g_free (encoded);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_scheme_end (ChromiumSession *self, int id)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  scheme_command_send (self, "scheme-end", id);
}

void
chromium_session_scheme_abort (ChromiumSession *self, int id)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->scheme_held, GINT_TO_POINTER (id)))
    return;
  scheme_command_send (self, "scheme-abort", id);
}

/* A parked scheme request with no host answer is a page hung on its own
   resource: the session aborts it, the one default that does not invent
   data. */
static void
handle_scheme_request (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");
  JsonNode *headers = json_object_has_member (object, "headers")
                        ? json_object_get_member (object, "headers") : NULL;
  g_autofree char *headers_json =
      headers ? json_to_string (headers, FALSE) : g_strdup ("{}");

  g_hash_table_add (self->scheme_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_SCHEME_REQUEST], 0,
                 id, browser,
                 event_string (object, "url"),
                 event_string (object, "method"),
                 headers_json,
                 &handled);
  g_debug ("session: scheme-request %d (%s), host took it: %d",
           id, event_string (object, "url"), handled);

  if (handled)
    return;
  if (!g_hash_table_remove (self->scheme_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: scheme-request %d aborted by default", id);
  scheme_command_send (self, "scheme-abort", id);
}

static void
handle_scheme_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->scheme_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: scheme-request %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_SCHEME_CLOSED], 0, id, browser);
}

/* ------------------------------------------------------------------ *
 * Host scripts                                                       *
 * ------------------------------------------------------------------ */

void
chromium_session_script_add (ChromiumSession *self, guint id, int browser,
                             const char *const *patterns, const char *code,
                             const char *run_at, gboolean all_frames)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *const *pattern;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (patterns != NULL && *patterns != NULL);
  g_return_if_fail (code != NULL && *code != '\0');

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "script-add");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, (gint64) id);
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  json_builder_set_member_name (builder, "patterns");
  json_builder_begin_array (builder);
  for (pattern = patterns; *pattern; pattern++)
    json_builder_add_string_value (builder, *pattern);
  json_builder_end_array (builder);
  command_add_string (builder, "code", code);
  command_add_string (builder, "run_at",
                      run_at && *run_at ? run_at : "document_idle");
  json_builder_set_member_name (builder, "all_frames");
  json_builder_add_boolean_value (builder, all_frames != FALSE);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_script_remove (ChromiumSession *self, guint id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "script-remove");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, (gint64) id);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* ------------------------------------------------------------------ *
 * User agent, screenshots, typed data clearing                       *
 * ------------------------------------------------------------------ */

static void
ua_set_send (ChromiumSession *self, int browser, const char *value)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "ua-set");
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  command_add_string (builder, "value", value ? value : "");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_set_user_agent (ChromiumSession *self, const char *user_agent)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  g_free (self->user_agent);
  self->user_agent = g_strdup (user_agent ? user_agent : "");
  ua_set_send (self, 0, self->user_agent);
}

const char *
chromium_session_get_user_agent (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->user_agent ? self->user_agent : "";
}

void
chromium_session_browser_set_user_agent (ChromiumSession *self, int browser,
                                         const char *user_agent)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  ua_set_send (self, browser, user_agent ? user_agent : "");
}

void
chromium_session_capture_screenshot (ChromiumSession *self, int id,
                                     int browser)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "capture-screenshot");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_screenshot_result (ChromiumSession *self, JsonObject *object)
{
  gboolean ok = event_flag (object, "ok");
  GBytes *png = NULL;

  if (ok)
    {
      gsize length = 0;
      g_autofree guchar *decoded =
        g_base64_decode (event_string (object, "data"), &length);
      png = g_bytes_new_take (g_steal_pointer (&decoded), length);
    }

  g_signal_emit (self, signals[SIG_SCREENSHOT_RESULT], 0,
                 (int) event_int (object, "id"),
                 (int) event_int (object, "browser"),
                 ok, png,
                 event_string (object, "message"));
  if (png)
    g_bytes_unref (png);
}

void
chromium_session_clear_data_types (ChromiumSession *self, guint types,
                                   const char *const *origins)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *const *origin;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (types != 0);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "clear-data");
  json_builder_set_member_name (builder, "types");
  json_builder_add_int_value (builder, (gint64) types);
  if (types & CHROMIUM_CLEAR_ORIGIN_STORAGE)
    {
      json_builder_set_member_name (builder, "origins");
      json_builder_begin_array (builder);
      for (origin = origins; origin && *origin; origin++)
        json_builder_add_string_value (builder, *origin);
      json_builder_end_array (builder);
    }
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* ------------------------------------------------------------------ *
 * Version handshake                                                  *
 * ------------------------------------------------------------------ */

int
chromium_session_get_helper_version (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), 0);
  return self->helper_version;
}

/* ------------------------------------------------------------------ *
 * The browser table (the ::browsers-changed cache)                   *
 * ------------------------------------------------------------------ */

void
chromium_session_browser_info_free (ChromiumBrowserInfo *info)
{
  if (!info)
    return;
  g_free (info->url);
  g_free (info->title);
  g_free (info);
}

void
chromium_client_cert_free (ChromiumClientCert *cert)
{
  if (!cert)
    return;
  g_free (cert->subject);
  g_free (cert->issuer);
  g_free (cert->fingerprint);
  g_free (cert);
}

void
chromium_client_cert_list_free (GList *certs)
{
  g_list_free_full (certs, (GDestroyNotify) chromium_client_cert_free);
}

ChromiumBrowserInfo *
chromium_session_browsers_list (ChromiumSession *self, gsize *length)
{
  GHashTableIter iter;
  gpointer key, value;
  ChromiumBrowserInfo *out;
  gsize n = 0;
  guint count;

  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  if (length)
    *length = 0;
  if (!self->browser_table)
    return NULL;

  count = g_hash_table_size (self->browser_table);
  if (count == 0)
    return NULL;
  out = g_new0 (ChromiumBrowserInfo, count);

  g_hash_table_iter_init (&iter, self->browser_table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ChromiumBrowserInfo *one = value;
      out[n].id = one->id;
      out[n].url = g_strdup (one->url);
      out[n].title = g_strdup (one->title);
      out[n].active = one->active;
      out[n].windowed = one->windowed;
      out[n].muted = one->muted;
      out[n].zoom = one->zoom;
      n++;
    }
  if (length)
    *length = n;
  return out;
}

static void
handle_browsers_changed (ChromiumSession *self, JsonObject *object)
{
  JsonArray *array = json_object_has_member (object, "browsers")
                       ? json_object_get_array_member (object, "browsers")
                       : NULL;
  gsize i;

  g_hash_table_remove_all (self->browser_table);
  if (array)
    {
      for (i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *node = json_array_get_element (array, i);
          ChromiumBrowserInfo *info;

          if (!JSON_NODE_HOLDS_OBJECT (node))
            continue;
          {
            JsonObject *entry = json_node_get_object (node);
            info = g_new0 (ChromiumBrowserInfo, 1);
            info->id = (int) (json_object_has_member (entry, "id")
                                ? json_object_get_int_member (entry, "id") : 0);
            info->url = g_strdup
              (json_object_has_member (entry, "url")
                 ? json_object_get_string_member (entry, "url") : "");
            info->title = g_strdup
              (json_object_has_member (entry, "title")
                 ? json_object_get_string_member (entry, "title") : "");
            info->active =
              json_object_has_member (entry, "active") &&
              json_object_get_boolean_member (entry, "active");
            info->windowed =
              json_object_has_member (entry, "windowed") &&
              json_object_get_boolean_member (entry, "windowed");
            info->muted =
              json_object_has_member (entry, "muted") &&
              json_object_get_boolean_member (entry, "muted");
            info->zoom =
              json_object_has_member (entry, "zoom")
                ? json_object_get_double_member (entry, "zoom") : 0.0;
          }
          g_hash_table_replace (self->browser_table,
                                GINT_TO_POINTER (info->id), info);
        }
    }
  g_signal_emit (self, signals[SIG_BROWSERS_CHANGED], 0);
}

static void
handle_window_properties (ChromiumSession *self, JsonObject *object)
{
  g_signal_emit (self, signals[SIG_WINDOW_PROPERTIES], 0,
                 (int) event_int (object, "browser"),
                 (int) event_int (object, "x"),
                 (int) event_int (object, "y"),
                 (int) event_int (object, "w"),
                 (int) event_int (object, "h"),
                 event_flag (object, "toolbar"),
                 event_flag (object, "menubar"),
                 event_flag (object, "status"),
                 event_flag (object, "resizable"),
                 event_flag (object, "fullscreen"));
}

void
chromium_session_new_browser_with_geometry (ChromiumSession *self,
                                            const char *url, gboolean windowed,
                                            int x, int y, int width, int height)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "new-browser");
  command_add_string (builder, "url", url ? url : "");
  command_add_string (builder, "mode", windowed ? "window" : "tab");
  json_builder_set_member_name (builder, "geometry");
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "x");
  json_builder_add_int_value (builder, x);
  json_builder_set_member_name (builder, "y");
  json_builder_add_int_value (builder, y);
  json_builder_set_member_name (builder, "width");
  json_builder_add_int_value (builder, width);
  json_builder_set_member_name (builder, "height");
  json_builder_add_int_value (builder, height);
  json_builder_end_object (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* The scoped history jump: the component's own pipe end to end. */
void
chromium_session_history_go (ChromiumSession *self, int browser, int index)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "history-go");
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  json_builder_set_member_name (builder, "index");
  json_builder_add_int_value (builder, index);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
request_respond_send (ChromiumSession *self, int request_id,
                      const char *action, const char *url)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "request-respond");
  json_builder_set_member_name (builder, "request_id");
  json_builder_add_int_value (builder, request_id);
  command_add_string (builder, "action", action);
  if (url && *url)
    command_add_string (builder, "url", url);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* No default answer here, on purpose: a request is a hole in the page until
 * it resolves, and inventing one would be the component making a network
 * policy call. The question stays held until the host answers it or the
 * browser goes away. */
static void
handle_request_intercept (ChromiumSession *self, JsonObject *object)
{
  int request_id = (int) event_int (object, "request_id");
  int browser = (int) event_int (object, "browser");
  JsonNode *headers = json_object_has_member (object, "headers")
                        ? json_object_get_member (object, "headers") : NULL;
  g_autofree char *headers_json =
      headers ? json_to_string (headers, FALSE) : g_strdup ("{}");

  g_hash_table_add (self->intercept_held, GINT_TO_POINTER (request_id));

  g_signal_emit (self, signals[SIG_REQUEST_INTERCEPT], 0,
                 request_id, browser,
                 event_string (object, "url"),
                 event_string (object, "method"),
                 event_string (object, "resource_type"),
                 event_flag (object, "is_main_frame"),
                 headers_json);
  g_debug ("session: request-intercept %d (%s), held for the host",
           request_id, event_string (object, "url"));
}

static void
handle_request_intercept_closed (ChromiumSession *self, JsonObject *object)
{
  int request_id = (int) event_int (object, "request_id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->intercept_held,
                            GINT_TO_POINTER (request_id)))
    return;

  g_debug ("session: request-intercept %d closed by the browser", request_id);
  g_signal_emit (self, signals[SIG_REQUEST_INTERCEPT_CLOSED], 0,
                 request_id, browser);
}

void
chromium_session_request_respond (ChromiumSession *self, int request_id,
                                  ChromiumRequestAction action,
                                  const char *url)
{
  const char *names[] = { "allow", "cancel", "redirect" };

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (action == CHROMIUM_REQUEST_REDIRECT ||
                    url == NULL || !*url ||
                    action != CHROMIUM_REQUEST_REDIRECT);

  if (!g_hash_table_remove (self->intercept_held,
                            GINT_TO_POINTER (request_id)))
    {
      g_debug ("session: no request %d to answer", request_id);
      return;
    }

  request_respond_send (self, request_id, names[action], url);
}

static void
intercept_rules_send_ids (ChromiumSession *self, const char *cmd, guint id,
                          const char *const *patterns,
                          const char *const *resource_types)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *const *entry;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", cmd);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, (gint64) id);
  if (patterns)
    {
      json_builder_set_member_name (builder, "patterns");
      json_builder_begin_array (builder);
      for (entry = patterns; *entry; entry++)
        json_builder_add_string_value (builder, *entry);
      json_builder_end_array (builder);
    }
  if (resource_types)
    {
      json_builder_set_member_name (builder, "resource_types");
      json_builder_begin_array (builder);
      for (entry = resource_types; *entry; entry++)
        json_builder_add_string_value (builder, *entry);
      json_builder_end_array (builder);
    }
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_intercept_add (ChromiumSession *self, guint id,
                                const char *const *patterns,
                                const char *const *resource_types)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (patterns != NULL && *patterns != NULL);

  intercept_rules_send_ids (self, "intercept-add", id, patterns,
                            resource_types);
}

void
chromium_session_intercept_remove (ChromiumSession *self, guint id)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  intercept_rules_send_ids (self, "intercept-remove", id, NULL, NULL);
}

void
chromium_session_intercept_set_passive (ChromiumSession *self,
                                        gboolean passive)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "intercept-mode");
  json_builder_set_member_name (builder, "passive");
  json_builder_add_boolean_value (builder, passive);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/* The DevTools protocol pipe. Fire-and-forget in both directions: the host's
   own id comes back inside the result, events arrive as they happen. Nothing
   here runs unless the host turned the observer on — cdp_enable is the one
   switch, and without it the helper refuses every call by name. */
static void
handle_cdp_result (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");

  g_signal_emit (self, signals[SIG_CDP_RESULT], 0,
                 id,
                 event_flag (object, "ok"),
                 event_string (object, "json"),
                 event_string (object, "message"));
}

static void
handle_cdp_event (ChromiumSession *self, JsonObject *object)
{
  int browser = (int) event_int (object, "browser");

  g_signal_emit (self, signals[SIG_CDP_EVENT], 0,
                 browser,
                 event_string (object, "method"),
                 event_string (object, "params"),
                 event_string (object, "message"));
}

static void
cdp_target_send (ChromiumSession *self, const char *cmd, int browser)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", cmd);
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_cdp_enable (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  cdp_target_send (self, "cdp-enable", browser);
}

void
chromium_session_cdp_disable (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  cdp_target_send (self, "cdp-disable", browser);
}

void
chromium_session_cdp_execute (ChromiumSession *self, int id, int browser,
                              const char *method, const char *params_json)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (method != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "cdp-execute");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
  command_add_string (builder, "method", method);
  json_builder_set_member_name (builder, "params");
  json_builder_add_string_value (builder,
                                 params_json ? params_json : "");
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
file_dialog_respond_send (ChromiumSession *self, int id,
                          const char *const *paths)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  const char *const *path;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "file-dialog-respond");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_set_member_name (builder, "paths");
  json_builder_begin_array (builder);
  for (path = paths; path && *path; path++)
    json_builder_add_string_value (builder, *path);
  json_builder_end_array (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

static void
handle_file_dialog (ChromiumSession *self, JsonObject *object)
{
  gboolean handled = FALSE;
  GList *accept = NULL;
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");
  const char *mode = event_string (object, "mode");
  const char *title = event_string (object, "title");
  const char *default_path = event_string (object, "default_path");
  gsize i;

  if (json_object_has_member (object, "accept") &&
      json_node_get_node_type (json_object_get_member (object, "accept")) == JSON_NODE_ARRAY)
    {
      JsonArray *array = json_object_get_array_member (object, "accept");

      for (i = 0; i < json_array_get_length (array); i++)
        accept = g_list_append (accept,
                               g_strdup (json_array_get_string_element (array, i)));
    }

  g_hash_table_add (self->file_dialog_held, GINT_TO_POINTER (id));

  g_signal_emit (self, signals[SIG_FILE_DIALOG], 0,
                 id, browser, mode, title, default_path, accept, &handled);
  g_debug ("session: file-dialog %d (%s) %s, host took it: %d",
           id, mode, title, handled);

  g_list_free_full (accept, g_free);

  if (handled)
    return;
  if (!g_hash_table_remove (self->file_dialog_held, GINT_TO_POINTER (id)))
    return;

  /* No chooser, no choice: cancel is the only answer that does not invent a
   * path the page never asked for. */
  g_debug ("session: file-dialog %d (%s) cancelled by default", id, mode);
  file_dialog_respond_send (self, id, NULL);
}

static void
handle_file_dialog_closed (ChromiumSession *self, JsonObject *object)
{
  int id = (int) event_int (object, "id");
  int browser = (int) event_int (object, "browser");

  if (!g_hash_table_remove (self->file_dialog_held, GINT_TO_POINTER (id)))
    return;

  g_debug ("session: file-dialog %d closed by the browser", id);
  g_signal_emit (self, signals[SIG_FILE_DIALOG_CLOSED], 0, id, browser);
}

void
chromium_session_file_dialog_respond (ChromiumSession *self, int id,
                                      const char *const *paths)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (!g_hash_table_remove (self->file_dialog_held, GINT_TO_POINTER (id)))
    {
      g_debug ("session: no file dialog %d to answer", id);
      return;
    }

  file_dialog_respond_send (self, id, paths);
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

  /* A restart can replace self->child before the old process is reaped. Only
   * the wait for the current child may close this session's channel; an old
   * wait that did so would tear down the connection the new helper is using. */
  if (self->child && source == G_OBJECT (self->child))
    {
      mark_closed (self, "process exited");

      /* A ::closed handler may start a fresh helper from inside that emission
       * (examples/useragent.c does). This wait belongs to the old child; had it
       * gone on, it would clear the new one's child and close the channel the
       * new helper is about to use. */
      if (self->child && source == G_OBJECT (self->child))
        {
          release_data_directory (self);
          g_clear_object (&self->child);
          self->child_ephemeral = FALSE;
          drop_connection (self);

          /* ::closed said the page is gone; this says the process that held it
           * is gone too. The distinction is what lets a host keep its display
           * alive through the helper's CefShutdown() — see the signal's
           * documentation in chromium-session.h. Emitted after the child is
           * cleared so a handler that stops the session cannot address a
           * process already reaped. */
          g_signal_emit (self, signals[SIG_HELPER_EXITED], 0);
        }
    }
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
      /* bind() has already created the inode; removing the socket object does
       * not remove it from the runtime directory. */
      g_unlink (self->socket_path);
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
      g_unlink (self->socket_path);
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

/* Data directories claimed by live sessions in this process. The helper's own
 * flock is the authority across processes; this table is what lets start()
 * refuse the same path twice before a second helper exists to kill. Created on
 * first use. */
static GHashTable *data_directories;

static void
release_data_directory (ChromiumSession *self)
{
  if (!self->claimed_data_directory)
    return;

  if (data_directories)
    g_hash_table_remove (data_directories, self->claimed_data_directory);
  g_clear_pointer (&self->claimed_data_directory, g_free);
}

static void
claim_data_directory (ChromiumSession *self)
{
  if (!data_directories)
    data_directories = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);

  release_data_directory (self);
  self->claimed_data_directory = g_strdup (self->data_directory);
  g_hash_table_add (data_directories, g_strdup (self->data_directory));
}

/* TRUE when @dir is held by a helper, or when the lock cannot be examined at
 * all: both mean "do not start". The helper's flock is the authority; this
 * precheck only turns the common collision into an error from start(). */
static gboolean
data_directory_locked (const char *dir, GError **error)
{
  g_autofree char *lock_path = g_build_filename (dir, ".cefnest.lock", NULL);
  int fd = open (lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot open data directory lock %s: %s",
                   lock_path, g_strerror (errno));
      return TRUE;
    }

  if (flock (fd, LOCK_EX | LOCK_NB) == -1)
    {
      int saved = errno;

      close (fd);
      if (saved == EWOULDBLOCK)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     "data directory is in use by another session: %s", dir);
      else
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                     "cannot lock data directory %s: %s", dir,
                     g_strerror (saved));
      return TRUE;
    }

  /* Released right away: the helper must be the one holding it, and a second
   * descriptor in this process would be the thing in its way. */
  close (fd);
  return FALSE;
}

static gboolean
prepare_data_directory (ChromiumSession *self, GError **error)
{
  const char *dir = self->data_directory;
  gboolean ours;

  if (!dir || !*dir)
    return TRUE;

  if (!g_path_is_absolute (dir))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "data directory must be an absolute path: %s", dir);
      return FALSE;
    }

  ours = g_strcmp0 (self->claimed_data_directory, dir) == 0;
  if (!ours && data_directories &&
      g_hash_table_contains (data_directories, dir))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "data directory already in use by another session: %s", dir);
      return FALSE;
    }

  /* A leaf that is a file: mkdir would fail with EEXIST, which reads like a
   * concurrency problem rather than the configuration mistake it is. */
  if (g_file_test (dir, G_FILE_TEST_EXISTS) &&
      !g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                   "data directory is not a directory: %s", dir);
      return FALSE;
    }

  if (g_mkdir_with_parents (dir, 0700) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "cannot create data directory %s: %s", dir,
                   g_strerror (errno));
      return FALSE;
    }

  /* A restart keeps its own directory: the helper being torn down still holds
   * the lock, and waiting for that kill to land here would only block the
   * thread on a lock this session is about to take back. */
  if (!ours && data_directory_locked (dir, error))
    return FALSE;

  return TRUE;
}

/* GLib has no recursive delete, and every host that owns a profile would
 * otherwise write this. Children go before the directory that holds them. */
static gboolean
remove_tree (const char *path, GError **error)
{
  g_autoptr (GFile) dir = g_file_new_for_path (path);
  g_autoptr (GFileEnumerator) entries = NULL;
  GFileInfo *info;

  entries = g_file_enumerate_children (dir,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                       G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       NULL, error);
  if (!entries)
    return FALSE;

  while ((info = g_file_enumerator_next_file (entries, NULL, error)) != NULL)
    {
      g_autoptr (GFile) child =
        g_file_get_child (dir, g_file_info_get_name (info));
      gboolean is_dir =
        g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

      g_object_unref (info);
      if (is_dir ? !remove_tree (g_file_peek_path (child), error)
                 : !g_file_delete (child, NULL, error))
        return FALSE;
    }

  if (error && *error)
    return FALSE;

  return g_file_delete (dir, NULL, error);
}

/* SIGKILL is delivered asynchronously, and until it lands Chromium still owns
 * the directory: files deleted in that window come back. A zombie has stopped
 * executing and can only be reaped, so wait for that state before touching the
 * tree. The child's own wait callback runs on the main context this function is
 * blocking, so the pid is normally still there; a pid that is already gone was
 * reaped and is equally safe. This is Linux-only, like the helper. */
static void
wait_for_child_stop (const char *identifier)
{
  g_autofree char *stat_path = g_strdup_printf ("/proc/%s/stat", identifier);
  gint64 deadline = g_get_monotonic_time () + G_TIME_SPAN_SECOND;

  for (;;)
    {
      g_autofree char *stat = NULL;
      const char *close, *state = NULL;

      if (!g_file_get_contents (stat_path, &stat, NULL, NULL))
        return;

      /* "pid (comm) state ...": comm may contain spaces and parentheses, so
       * the state is the field after the last ')'. */
      close = strrchr (stat, ')');
      if (close && close[1] == ' ')
        state = close + 2;
      if (state && *state == 'Z')
        return;

      if (g_get_monotonic_time () >= deadline)
        return;
      g_usleep (1000);
    }
}

/* The helper removes its own scratch root on every exit it can reach. A host
 * stop is a SIGKILL, which is not one of them: the helper never runs its tail
 * and the root would sit in tmpfs until the next helper's sweep. The host made
 * that exit unreachable, so it removes the root instead. The child is a zombie
 * until reaped and cannot have its pid reused, so a name match can only be this
 * helper's root; anything else in the runtime directory is left alone.
 *
 * Deletion is retried until the directory is gone: SIGKILL stops the browser
 * process, but Chromium's own children can still touch its cache for a moment,
 * which makes a single pass fail with ENOENT or ENOTEMPTY. A root that refuses
 * to go inside the deadline is left to the next helper's sweep. */
static void
remove_child_scratch (GSubprocess *child)
{
  const char *identifier = g_subprocess_get_identifier (child);
  const char *runtime;
  g_autofree char *prefix = NULL;
  g_autoptr (GFile) base = NULL;
  gint64 deadline;

  if (!identifier)
    return;

  wait_for_child_stop (identifier);

  /* The helper reads XDG_RUNTIME_DIR exactly, with /dev/shm as its fallback;
   * g_get_user_runtime_dir() would add a fallback of its own. */
  runtime = g_getenv ("XDG_RUNTIME_DIR");
  if (!runtime || !*runtime)
    runtime = "/dev/shm";

  prefix = g_strdup_printf ("cefnest-helper-%s-", identifier);
  base = g_file_new_for_path (runtime);
  deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  for (;;)
    {
      g_autoptr (GFileEnumerator) entries = NULL;
      GError *error = NULL;
      GFileInfo *info;
      gboolean found = FALSE;

      entries = g_file_enumerate_children (base,
                                           G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                           G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           NULL, &error);
      if (!entries)
        {
          g_debug ("session: cannot scan %s for %s: %s", runtime, prefix,
                   error->message);
          g_error_free (error);
          return;
        }

      while ((info = g_file_enumerator_next_file (entries, NULL, &error)) != NULL)
        {
          const char *name = g_file_info_get_name (info);
          gboolean is_dir =
            g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

          if (is_dir && g_str_has_prefix (name, prefix))
            {
              g_autoptr (GFile) candidate = g_file_get_child (base, name);

              found = TRUE;
              remove_tree (g_file_peek_path (candidate), NULL);
            }
          g_object_unref (info);
        }

      if (error)
        {
          g_debug ("session: scanning %s for %s failed: %s", runtime, prefix,
                   error->message);
          g_error_free (error);
        }

      if (!found)
        return;
      if (g_get_monotonic_time () >= deadline)
        {
          g_debug ("session: could not finish removing %s; the next helper "
                   "will sweep it", prefix);
          return;
        }
      g_usleep (20 * 1000);
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

  /* Before the teardown below: a bad path must not cost the host the session
   * it already has. */
  if (!prepare_data_directory (self, error))
    return FALSE;

  if (self->child)
    {
      if (self->running)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                       "the helper is already running");
          return FALSE;
        }

      /* ::closed has already told the host this run is over, but the process
       * has not been reaped yet. Let it go and take its channel down so a
       * start from the ::closed handler is not answered with BUSY forever;
       * child_waited() compares against self->child, so the old wait callback
       * will not touch the new session. */
      if (self->cancellable)
        g_cancellable_cancel (self->cancellable);
      g_clear_handle_id (&self->accept_retry_id, g_source_remove);
      g_subprocess_force_exit (self->child);
      if (self->child_ephemeral)
        remove_child_scratch (self->child);
      else
        {
          /* A persistent helper holds the directory's flock until the process
           * is actually dead; the next helper would lose that race. */
          const char *identifier = g_subprocess_get_identifier (self->child);

          if (identifier)
            wait_for_child_stop (identifier);
        }
      g_clear_object (&self->child);
      self->child_ephemeral = FALSE;
      close_listener (self);
      drop_connection (self);
      /* The next helper claims whatever path this start() was configured
       * with; the one the old process held is free once the kill lands. */
      release_data_directory (self);
    }
  if (!self->view)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "no view set: the helper would have no compositor to draw on");
      return FALSE;
    }

  /* Armed here, not in init, so a process that never starts a session never
   * touches the session bus. The proxy arrives asynchronously and is idempotent,
   * so a stop and a second start keep the one proxy. */
  chromium_session_watch_a11y (self);

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
  /* The helper's profile is this session's decision, never the caller's
   * environment: an inherited CHROMIUM_HELPER_DATA_DIR would make a host that
   * asked for incognito disk-backed, and the other way round. Unsetting it
   * explicitly is the half that matters — CEF writes to ~/.config/cef_user_data
   * when nothing points it elsewhere. */
  if (self->data_directory && *self->data_directory)
    g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_DATA_DIR",
                                  self->data_directory, TRUE);
  else
    g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_DATA_DIR");
  /* Same rule for the network identity: the session's values are the only
   * source, and an inherited variable from the host's environment must not
   * turn a session that asked for no proxy into a proxied one. */
  if (self->proxy && *self->proxy)
    g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_PROXY",
                                  self->proxy, TRUE);
  else
    g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_PROXY");
  if (self->proxy_bypass && *self->proxy_bypass)
    g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_PROXY_BYPASS",
                                  self->proxy_bypass, TRUE);
  else
    g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_PROXY_BYPASS");
  if (self->accept_language && *self->accept_language)
    g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_ACCEPT_LANG",
                                  self->accept_language, TRUE);
  else
    g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_ACCEPT_LANG");
  /* Autoplay rides the same spawn-time rule: one global value the helper
   * turns into --autoplay-policy, and an inherited variable must not give a
   * session that asked for Chromium's default a strict policy instead. */
  if (self->autoplay_policy && *self->autoplay_policy)
    g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_AUTOPLAY_POLICY",
                                  self->autoplay_policy, TRUE);
  else
    g_subprocess_launcher_unsetenv (launcher,
                                    "CHROMIUM_HELPER_AUTOPLAY_POLICY");
  /* The host's schemes: the option flags are only listenable at process
     start, so they ride the environment exactly like the proxy does. The
     data factories are live traffic and go over the channel on ::ready. */
  {
    g_autofree char *schemes = schemes_env_value (self);

    if (*schemes)
      g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_SCHEMES",
                                    schemes, TRUE);
    else
      g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_SCHEMES");
  }
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
  self->child_ephemeral = !(self->data_directory && *self->data_directory);

  if (self->data_directory && *self->data_directory)
    claim_data_directory (self);
  else
    release_data_directory (self);

  self->running = TRUE;
  self->stopping = FALSE;
  self->closed_emitted = FALSE;
  self->helper_is_exiting = FALSE;
  /* A fresh helper is a fresh browser: it starts unmuted and at 100 % zoom
   * whatever the previous one was told. Chromium's own per-origin zoom is
   * applied inside the new helper when that origin loads. */
  self->muted = FALSE;
  self->zoom_level = 0.0;
  /* A new helper means new browser ids and new toplevels: nothing the previous
   * run paired can still be true. The URL table starts empty too — the first
   * helper run may never have created it (no plugin manager yet), and ::closed
   * tears whatever it held down with the old browser ids. Recreated here, it
   * keeps runtime-message delivery alive across a restart. */
  g_hash_table_remove_all (self->browser_surfaces);
  g_hash_table_remove_all (self->surface_browser_ids);
  g_hash_table_remove_all (self->seen_toplevels);
  g_queue_clear_full (self->unpaired_surfaces, g_object_unref);
  g_queue_clear (self->pending_windowed_ids);
  if (!self->browser_urls)
    self->browser_urls = g_hash_table_new_full (g_direct_hash,
                                                g_direct_equal,
                                                NULL, g_free);
  else
    g_hash_table_remove_all (self->browser_urls);
  if (!self->background_browsers)
    self->background_browsers = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free, NULL);
  else
    g_hash_table_remove_all (self->background_browsers);
  self->primary_browser_id = 0;
  self->primary_surface_paired = FALSE;
  self->active_browser_id = 0;
  session_watch_compositor (self);
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

  if (self->child && !self->helper_is_exiting)
    {
      gboolean ephemeral = self->child_ephemeral;

      g_subprocess_force_exit (self->child);

      /* SIGKILL is the one exit the helper cannot clean up behind. Its root is
       * named after this pid, which the zombie cannot lose until it is reaped,
       * so this cannot hit another helper's directory. */
      if (ephemeral)
        remove_child_scratch (self->child);
    }

  close_listener (self);
  drop_connection (self);
  mark_closed (self, "stopped by the host");
  self->stopping = FALSE;
}

void
chromium_session_set_data_directory (ChromiumSession *self, const char *path)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (g_strcmp0 (self->data_directory, path && *path ? path : NULL) == 0)
    return;

  g_free (self->data_directory);
  self->data_directory = path && *path ? g_strdup (path) : NULL;
  /* A manager created before this call derived its install root from the
     value that was set then; move it with the property. The model refreshes
     on the host's next scan(), the same way the helper reads the property
     only at start(). */
  session_plugin_install_root (self);
  g_object_notify (G_OBJECT (self), "data-directory");
}

const char *
chromium_session_get_data_directory (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->data_directory;
}

static void
session_set_string (ChromiumSession *self, char **field, const char *value,
                    const char *property)
{
  char *replacement = value && *value ? g_strdup (value) : NULL;

  if (g_strcmp0 (*field, replacement) == 0)
    {
      g_free (replacement);
      return;
    }
  g_free (*field);
  *field = replacement;
  g_object_notify (G_OBJECT (self), property);
}

void
chromium_session_set_proxy (ChromiumSession *self, const char *proxy)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  session_set_string (self, &self->proxy, proxy, "proxy");
}

const char *
chromium_session_get_proxy (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->proxy;
}

void
chromium_session_set_proxy_bypass (ChromiumSession *self, const char *bypass)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  session_set_string (self, &self->proxy_bypass, bypass, "proxy-bypass");
}

const char *
chromium_session_get_proxy_bypass (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->proxy_bypass;
}

void
chromium_session_set_accept_language (ChromiumSession *self,
                                      const char *language)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  session_set_string (self, &self->accept_language, language,
                      "accept-language");
}

const char *
chromium_session_get_accept_language (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->accept_language;
}

void
chromium_session_set_autoplay_policy (ChromiumSession *self,
                                      const char *policy)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  session_set_string (self, &self->autoplay_policy, policy,
                      "autoplay-policy");
}

const char *
chromium_session_get_autoplay_policy (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return self->autoplay_policy;
}

void
chromium_session_set_auto_resize (ChromiumSession *self, gboolean enabled)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  if (self->auto_resize == enabled)
    return;
  self->auto_resize = enabled;
  session_send_auto_resize (self);
  g_object_notify (G_OBJECT (self), "auto-resize");
}

gboolean
chromium_session_get_auto_resize (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  return self->auto_resize;
}

void
chromium_session_set_auto_resize_bounds (ChromiumSession *self, int min_width,
                                         int min_height, int max_width,
                                         int max_height)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (min_width >= 0 && min_height >= 0);
  g_return_if_fail (max_width >= 0 && max_height >= 0);

  self->auto_resize_min_width = min_width;
  self->auto_resize_min_height = min_height;
  self->auto_resize_max_width = max_width;
  self->auto_resize_max_height = max_height;
  session_send_auto_resize (self);
  g_object_notify (G_OBJECT (self), "auto-resize-min-width");
  g_object_notify (G_OBJECT (self), "auto-resize-min-height");
  g_object_notify (G_OBJECT (self), "auto-resize-max-width");
  g_object_notify (G_OBJECT (self), "auto-resize-max-height");
}

NestedSurface *
chromium_session_get_browser_surface (ChromiumSession *self, int id)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), NULL);
  return session_surface_for_browser (self, id);
}

gboolean
chromium_session_clear_data (ChromiumSession *self, GError **error)
{
  const char *dir;
  g_autofree char *lock_path = NULL;
  gboolean removed;
  int fd = -1;

  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->running)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "cannot clear the data directory while the session runs");
      return FALSE;
    }

  dir = self->data_directory;
  if (!dir || !*dir)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "no data directory is set");
      return FALSE;
    }

  /* Nothing there is a success: the caller asked for "gone", and it is. */
  if (!g_file_test (dir, G_FILE_TEST_EXISTS))
    return TRUE;

  /* Only a helper creates this file, and a helper holds its lock for as long
   * as it lives — so a missing file means nobody to ask, and a lock that
   * cannot be taken means somebody still has the directory. The descriptor
   * stays open across the removal so no helper can take the lock in between. */
  lock_path = g_build_filename (dir, ".cefnest.lock", NULL);
  fd = open (lock_path, O_RDWR | O_CLOEXEC);
  if (fd >= 0 && flock (fd, LOCK_EX | LOCK_NB) == -1)
    {
      int saved = errno;

      close (fd);
      if (saved == EWOULDBLOCK)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     "data directory is in use by another session: %s", dir);
      else
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                     "cannot lock data directory %s: %s", dir,
                     g_strerror (saved));
      return FALSE;
    }

  removed = remove_tree (dir, error);
  if (fd >= 0)
    close (fd);
  return removed;
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
  session_watch_compositor (self);
}

void
chromium_session_new_browser (ChromiumSession *self, const char *url,
                              gboolean windowed)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "new-browser");
  command_add_string (builder, "url", url ? url : "");
  command_add_string (builder, "mode", windowed ? "window" : "tab");
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_new_background_browser (ChromiumSession *self, const char *url)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "new-browser");
  command_add_string (builder, "url", url ? url : "");
  command_add_string (builder, "mode", "background");
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_select_browser (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "select-browser");
  json_builder_set_member_name (builder, "browser");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_close_browser (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "close-browser");
  json_builder_set_member_name (builder, "browser");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

uint64_t
chromium_session_get_frame_count (ChromiumSession *self)
{
  NestedCompositor *compositor;

  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), 0);
  if (!self->view)
    return 0;
  compositor = chromium_view_get_compositor (self->view);
  return compositor ? nested_compositor_get_frame_count (compositor) : 0;
}

void
chromium_session_get_commit_stats (ChromiumSession *self,
                                   guint64 *shm_commits,
                                   guint64 *dmabuf_commits,
                                   guint64 *shm_bytes_copied,
                                   guint64 *shm_bytes_wrapped)
{
  NestedCompositor *compositor;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  if (!self->view)
    return;
  compositor = chromium_view_get_compositor (self->view);
  if (compositor)
    nested_compositor_get_commit_stats (compositor, shm_commits, dmabuf_commits,
                                        shm_bytes_copied, shm_bytes_wrapped);
}

void
chromium_session_set_accessibility (ChromiumSession *self, gboolean enabled)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  self->a11y_enabled = enabled;
  session_send_a11y (self);
}

void
chromium_session_set_devtools_enabled (ChromiumSession *self, gboolean enabled)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  /* Through the property so a host watching it gets the change too; a running
   * helper hears about it from set_property. */
  g_object_set (self, "devtools-enabled", enabled, NULL);
}

gboolean
chromium_session_get_devtools_enabled (ChromiumSession *self)
{
  g_return_val_if_fail (CHROMIUM_IS_SESSION (self), FALSE);

  return self->devtools_enabled;
}

/* A named browser on any command that takes one: 0 keeps the helper's own
   "active browser" default, anything else is that browser's id. */
static void
command_add_browser (JsonBuilder *builder, int browser)
{
  if (browser > 0)
    {
      json_builder_set_member_name (builder, "browser");
      json_builder_add_int_value (builder, browser);
    }
}

static void
send_command_browser (ChromiumSession *self, const char *cmd, int browser)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", cmd);
  command_add_browser (builder, browser);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_load_uri (ChromiumSession *self, int browser,
                           const char *uri)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (uri != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "load");
  command_add_browser (builder, browser);
  command_add_string (builder, "url", uri);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_go_back (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command_browser (self, "back", browser);
}

void
chromium_session_go_forward (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command_browser (self, "forward", browser);
}

void
chromium_session_reload (ChromiumSession *self, int browser, gboolean hard)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "reload");
  command_add_browser (builder, browser);
  command_add_boolean (builder, "hard", hard);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_stop_loading (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  send_command_browser (self, "stop", browser);
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

/* One command per step: the helper applies the CEF zoom level and answers with
 * a zoom event, so the host never has to guess whether the change landed. */
void
chromium_session_zoom (ChromiumSession *self, int browser, const char *action)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (action != NULL);

  if (!strcmp (action, "in"))
    send_command_browser (self, "zoom-in", browser);
  else if (!strcmp (action, "out"))
    send_command_browser (self, "zoom-out", browser);
  else if (!strcmp (action, "reset"))
    send_command_browser (self, "zoom-reset", browser);
  else
    g_return_if_reached ();
}

void
chromium_session_set_zoom_level (ChromiumSession *self, int browser,
                                 double level)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "zoom-set");
  command_add_browser (builder, browser);
  json_builder_set_member_name (builder, "level");
  json_builder_add_double_value (builder, level);
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
chromium_session_run_javascript_expression (ChromiumSession *self,
                                            const char *code, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (code != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "js-expr");
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
chromium_session_pause_download (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "download-pause");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_resume_download (ChromiumSession *self, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "download-resume");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
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
chromium_session_find (ChromiumSession *self, int browser,
                       const char *pattern,
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
  command_add_browser (builder, browser);
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
chromium_session_stop_finding (ChromiumSession *self, int browser,
                               gboolean clear_selection)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "find-stop");
  command_add_browser (builder, browser);
  json_builder_set_member_name (builder, "clear");
  json_builder_add_boolean_value (builder, clear_selection != FALSE);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  send_json (self, root);
}

/**
 * chromium_session_view_source:
 * @self: the session
 *
 * Asks for the active browser's main frame source — the same request the
 * context menu's "查看网页源代码" makes. The answer arrives as
 * #ChromiumSession::view-source (and, with #ChromiumSession:auto-source-window
 * on, the component's own source window); there is no receipt for the
 * command itself. What Chromium hands back is the serialized DOM, script
 * changes included, not the network's original bytes.
 *
 * Since: 156.0.14
 */
void
chromium_session_view_source (ChromiumSession *self, int browser)
{
  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  send_command_browser (self, "view-source", browser);
}

/* The three cookies commands read and write the running helper's own request
 * context. The answer is one ::cookies-result keyed by @id, whichever command
 * asked; nothing here waits for it. */
void
chromium_session_cookies_get (ChromiumSession *self, int id, const char *url)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (url != NULL);

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "cookies-get");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  command_add_string (builder, "url", url);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_cookies_set (ChromiumSession *self, int id, const char *url,
                              const char *cookies_json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  JsonNode *cookies;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail (cookies_json != NULL);

  /* An array of cookie objects, the same shape cookies-get answers with. Refuse
   * to put a malformed literal on the channel: the helper would have to drop
   * the line, and the host would never learn which command it was. */
  if (!json_parser_load_from_data (parser, cookies_json, -1, &error) ||
      (cookies = json_parser_get_root (parser)) == NULL ||
      !JSON_NODE_HOLDS_ARRAY (cookies))
    {
      g_warning ("session: cookies-set needs a JSON array of cookie objects: %s",
                 error ? error->message : "the root is not an array");
      return;
    }

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "cookies-set");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (url && *url)
    command_add_string (builder, "url", url);
  json_builder_set_member_name (builder, "cookies");
  json_builder_add_value (builder, json_node_copy (cookies));
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_cookies_delete (ChromiumSession *self, int id, const char *url,
                                 const char *name)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));
  g_return_if_fail ((url && *url) || (name && *name));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "cookies-delete");
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (url && *url)
    command_add_string (builder, "url", url);
  if (name && *name)
    command_add_string (builder, "name", name);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_page_text (ChromiumSession *self, int browser, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "page-text");
  command_add_browser (builder, browser);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_history (ChromiumSession *self, int browser, int id)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "history");
  command_add_browser (builder, browser);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  send_json (self, root);
}

void
chromium_session_print_pdf (ChromiumSession *self, int browser, int id)
{
  chromium_session_print_pdf_with_settings (self, browser, id, NULL);
}

void
chromium_session_print_pdf_with_settings (ChromiumSession *self, int browser,
                                          int id,
                                          const ChromiumPdfSettings *settings)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;

  g_return_if_fail (CHROMIUM_IS_SESSION (self));

  json_builder_begin_object (builder);
  command_add_string (builder, "cmd", "print-pdf");
  command_add_browser (builder, browser);
  json_builder_set_member_name (builder, "id");
  json_builder_add_int_value (builder, id);
  if (settings)
    {
      json_builder_set_member_name (builder, "landscape");
      json_builder_add_boolean_value (builder, settings->landscape != FALSE);
      json_builder_set_member_name (builder, "print_background");
      json_builder_add_boolean_value (builder,
                                      settings->print_background != FALSE);
      if (settings->scale > 0)
        {
          json_builder_set_member_name (builder, "scale");
          json_builder_add_double_value (builder, settings->scale);
        }
      if (settings->page_ranges && *settings->page_ranges)
        command_add_string (builder, "page_ranges", settings->page_ranges);
      if (settings->max_bytes > 0)
        {
          json_builder_set_member_name (builder, "max_bytes");
          json_builder_add_int_value (builder, settings->max_bytes);
        }
    }
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
  object_class->get_property = chromium_session_get_property;
  object_class->set_property = chromium_session_set_property;

  /**
   * ChromiumSession:auto-source-window:
   *
   * Whether the session opens its own View Source window when page source
   * arrives (the default), or only emits ::view-source and leaves the
   * window to the host — which is what a host with windows of its own
   * wants, since the session can only guess the parent from its one view.
   *
   * Since: 156.0.14
   */
  g_object_class_install_property
    (object_class, PROP_AUTO_SOURCE_WINDOW,
     g_param_spec_boolean ("auto-source-window", "Auto source window",
                           "Open the component's own View Source window",
                           TRUE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * ChromiumSession:data-directory:
   *
   * The persistent profile directory — cookies, localStorage, IndexedDB and
   * the HTTP cache all live under it — or %NULL (the default) for an
   * in-memory, incognito profile whose helper-side scratch files sit on tmpfs
   * and are removed when the helper exits. See
   * chromium_session_set_data_directory() for the path rules; the property is
   * read once per start(), by the helper that start() spawns.
   */
  g_object_class_install_property
    (object_class, PROP_DATA_DIRECTORY,
     g_param_spec_string ("data-directory", "Data directory",
                          "Persistent profile directory, or NULL for none",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY));

  /**
   * ChromiumSession:devtools-enabled:
   *
   * Whether the context menu offers 检查 and F12 / Ctrl+Shift+I open the
   * inspector, in a window of its own. The default is %FALSE, and that is the
   * whole off switch: no menu row, no shortcut, and the helper never calls
   * ShowDevTools. See chromium_session_set_devtools_enabled() for what turning
   * it on gives the person at the keyboard.
   */
  g_object_class_install_property
    (object_class, PROP_DEVTOOLS_ENABLED,
     g_param_spec_boolean ("devtools-enabled", "DevTools enabled",
                           "Offer the inspector in the context menu and on F12",
                           FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * ChromiumSession:proxy:
   *
   * The proxy the next start() hands Chromium (`scheme://host:port`), or %NULL
   * for a direct connection. See chromium_session_set_proxy().
   */
  g_object_class_install_property
    (object_class, PROP_PROXY,
     g_param_spec_string ("proxy", "Proxy",
                          "Proxy for the next helper start, or NULL",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY));

  /**
   * ChromiumSession:proxy-bypass:
   *
   * Chromium's proxy bypass list for the next start(). See
   * chromium_session_set_proxy_bypass().
   */
  g_object_class_install_property
    (object_class, PROP_PROXY_BYPASS,
     g_param_spec_string ("proxy-bypass", "Proxy bypass list",
                          "Chromium proxy bypass list for the next start",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY));

  /**
   * ChromiumSession:accept-language:
   *
   * The Accept-Language list for the next start(), e.g. "zh-CN,zh". See
   * chromium_session_set_accept_language().
   */
  g_object_class_install_property
    (object_class, PROP_ACCEPT_LANGUAGE,
     g_param_spec_string ("accept-language", "Accept-Language",
                          "Accept-Language list for the next start",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY));

  /**
   * ChromiumSession:auto-resize:
   *
   * Whether page content drives the size of the browser's toplevel. See
   * chromium_session_set_auto_resize().
   */
  g_object_class_install_property
    (object_class, PROP_AUTO_RESIZE,
     g_param_spec_boolean ("auto-resize", "Auto resize",
                           "Let page content drive the toplevel size",
                           FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property
    (object_class, PROP_AUTO_RESIZE_MIN_WIDTH,
     g_param_spec_int ("auto-resize-min-width", "Auto resize minimum width",
                       "Smallest width auto-resize may configure", 0, G_MAXINT,
                       0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property
    (object_class, PROP_AUTO_RESIZE_MIN_HEIGHT,
     g_param_spec_int ("auto-resize-min-height", "Auto resize minimum height",
                       "Smallest height auto-resize may configure", 0, G_MAXINT,
                       0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property
    (object_class, PROP_AUTO_RESIZE_MAX_WIDTH,
     g_param_spec_int ("auto-resize-max-width", "Auto resize maximum width",
                       "Largest width auto-resize may configure, 0 = no limit",
                       0, G_MAXINT, 0,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property
    (object_class, PROP_AUTO_RESIZE_MAX_HEIGHT,
     g_param_spec_int ("auto-resize-max-height", "Auto resize maximum height",
                       "Largest height auto-resize may configure, 0 = no limit",
                       0, G_MAXINT, 0,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * ChromiumSession:user-agent:
   *
   * The session-wide user agent, applied live over the component's own pipe —
   * every browser of this session, current and future, until a per-browser
   * override (chromium_session_browser_set_user_agent()) says otherwise for
   * one of them. NULL or "" is Chromium's own. A UA switcher no longer needs
   * the restart an environment variable required.
   */
  g_object_class_install_property
    (object_class, PROP_USER_AGENT,
     g_param_spec_string ("user-agent", "User agent",
                          "Session-wide user agent override (\"\" = off)",
                          "",
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * ChromiumSession:autoplay-policy:
   *
   * The `--autoplay-policy` value for the next start(), e.g.
   * "user-gesture-required". %NULL or "" (the default) leaves Chromium's own
   * policy in place. Chromium has no runtime and no per-site face for it, so
   * like #ChromiumSession:proxy this is one global value read once per
   * start(); setting it after start() takes effect on the next start.
   */
  g_object_class_install_property
    (object_class, PROP_AUTOPLAY_POLICY,
     g_param_spec_string ("autoplay-policy", "Autoplay policy",
                          "--autoplay-policy for the next start, NULL for "
                          "Chromium's default",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY));

  signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_READY] =
    g_signal_new ("ready", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /**
   * ChromiumSession::load-started:
   * @browser: the browser whose navigation committed
   * @url: the URL committed to
   * @transition: Chromium's transition-type bitmask (link click, typed
   *   address, …)
   * @is_main_frame: whether the committing frame is the main frame
   *
   * One per committed navigation, the earliest fact a navigation offers: a
   * navigation that fails before commit never reaches here, so a host's
   * history service pairs this with ::load-end / ::load-error and never sees
   * a start without a finish.
   *
   * Since: 156.0.14
   */
  signals[SIG_LOAD_STARTED] =
    g_signal_new ("load-started", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);
  signals[SIG_LOAD_END] =
    g_signal_new ("load-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT);
  signals[SIG_LOAD_ERROR] =
    g_signal_new ("load-error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
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

  /**
   * ChromiumSession::js-dialog:
   * @self: the session
   * @id: the request id to answer with chromium_session_js_dialog_respond()
   * @browser: the browser whose page is waiting
   * @kind: "alert", "confirm", "prompt" or "beforeunload"
   * @origin: the asking page's origin, or "" for a beforeunload dialog
   * @message: the text the page passed to the dialog
   * @default_value: the prompt's initial text, or "" for the other kinds
   *
   * The page's script is stopped until this dialog is answered. A handler that
   * draws its own dialog returns %TRUE and owes exactly one answer; returning
   * %FALSE — or connecting nothing — makes the session answer with Chrome's
   * default: ok for alert and beforeunload, cancel (with an empty value) for
   * confirm and prompt.
   *
   * Since: 156.0.14
   */
  signals[SIG_JS_DIALOG] =
    g_signal_new ("js-dialog", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 6,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::js-dialog-closed:
   * @self: the session
   * @id: the request id
   * @browser: the browser that owned it
   *
   * A dialog this side still held was taken away by the browser — navigation,
   * a reload, or the browser closing. Nothing can be answered any more, so a
   * host panel has to come down.
   *
   * Since: 156.0.14
   */
  signals[SIG_JS_DIALOG_CLOSED] =
    g_signal_new ("js-dialog-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::certificate-error:
   * @self: the session
   * @id: the request id to answer with chromium_session_certificate_respond()
   * @browser: the browser that asked
   * @error: Chromium's net-error name ("ERR_CERT_AUTHORITY_INVALID", …)
   * @url: the request the certificate did not satisfy
   * @subject: the leaf certificate's subject display name, or ""
   * @issuer: its issuer's display name, or ""
   * @fingerprint: the SHA-256 of the leaf's DER, hex, or "" when the
   *   certificate could not be read
   *
   * The navigation is stopped until this is answered. A handler that wants to
   * decide returns %TRUE and owes exactly one
   * chromium_session_certificate_respond(); returning %FALSE — or connecting
   * nothing — denies, which is Chrome's own default and the safe one. The
   * decision repeats per error: a host that wants "remember this
   * certificate" calls chromium_session_certificate_authorize() with the
   * fingerprint, which authorizes it for this helper's lifetime; persistence
   * beyond that is the host's own policy store.
   *
   * Since: 156.0.14
   */
  signals[SIG_CERTIFICATE_ERROR] =
    g_signal_new ("certificate-error", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 7,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::certificate-error-closed:
   * @self: the session
   * @id: the request that went away
   * @browser: the browser that owned it
   *
   * The error was answered elsewhere, or its browser navigated or closed
   * before an answer came. A panel waiting on this @id should go away; there
   * is nothing left to answer.
   *
   * Since: 156.0.14
   */
  signals[SIG_CERTIFICATE_ERROR_CLOSED] =
    g_signal_new ("certificate-error-closed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::request-intercept:
   * @self: the session
   * @request_id: the id to answer with chromium_session_request_respond()
   * @browser: the browser loading the resource
   * @url: the request URL
   * @method: "GET", "POST", …
   * @resource_type: "main_frame", "image", "xhr", …
   * @is_main_frame: whether this request is the page itself
   * @headers: the request headers as one JSON object text
   *
   * The request is stopped until this is answered — there is no default, no
   * timeout, and no answer the session could invent without making a network
   * policy call. A host that registers rules owes fast answers; closing the
   * browser cancels what it left behind and reports it as
   * ::request-intercept-closed.
   *
   * With no rule registered nothing here runs at all: the feature is off
   * until the host says otherwise, and passive mode reports without
   * stopping.
   *
   * Since: 156.0.14
   */
  signals[SIG_REQUEST_INTERCEPT] =
    g_signal_new ("request-intercept", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 7,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);

  /**
   * ChromiumSession::request-intercept-closed:
   * @self: the session
   * @request_id: the request that went away
   * @browser: the browser that owned it
   *
   * The browser closed while the question was parked; the request was
   * cancelled with it. Nothing is left to answer.
   *
   * Since: 156.0.14
   */
  signals[SIG_REQUEST_INTERCEPT_CLOSED] =
    g_signal_new ("request-intercept-closed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::cdp-result:
   * @self: the session
   * @id: the id the matching chromium_session_cdp_execute() carried
   * @ok: whether the method succeeded
   * @json: the "result" (or the error dictionary) as one JSON text
   * @message: "cdp-too-large" when the answer exceeded the channel's line
   *   limit, "" otherwise
   *
   * Since: 156.0.14
   */
  signals[SIG_CDP_RESULT] =
    g_signal_new ("cdp-result", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::cdp-event:
   * @self: the session
   * @browser: the browser the event came from
   * @method: the protocol event name ("Page.frameStartedLoading", …)
   * @params: the event parameters as one JSON text
   * @message: "cdp-too-large" when the parameters exceeded the channel's
   *   line limit, "" otherwise
   *
   * Everything the browser's DevTools backend emits while the observer is
   * on — navigation, lifecycle, network, console. That is the feature: the
   * observer is only registered after chromium_session_cdp_enable().
   *
   * Since: 156.0.14
   */
  signals[SIG_CDP_EVENT] =
    g_signal_new ("cdp-event", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::file-dialog:
   * @self: the session
   * @id: the request id to answer with chromium_session_file_dialog_respond()
   * @browser: the browser that asked
   * @mode: "open", "open-multiple", "open-folder" or "save"
   * @title: the title the page asked for, or ""
   * @default_path: the initially selected path, or ""
   * @accept: a GList of owned-by-the-session filter strings ("image/…", ".txt")
   *
   * A page wants a file chooser. A handler that draws one returns %TRUE and
   * answers with the chosen paths — an empty list means cancelled; returning
   * %FALSE — or connecting nothing — cancels the chooser.
   *
   * Since: 156.0.14
   */
  signals[SIG_FILE_DIALOG] =
    g_signal_new ("file-dialog", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 6,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_POINTER);

  /**
   * ChromiumSession::file-dialog-closed:
   * @self: the session
   * @id: the request id
   * @browser: the browser that owned it
   *
   * The chooser was taken away with its browser. Nothing to answer.
   *
   * Since: 156.0.14
   */
  signals[SIG_FILE_DIALOG_CLOSED] =
    g_signal_new ("file-dialog-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::loading-progress:
   * @self: the session
   * @progress: the load's progress, 0.0 to 1.0
   *
   * The page's load progress, for a host that draws a progress bar. Advisory
   * and fire-and-forget; the next value supersedes this one.
   *
   * Since: 156.0.14
   */
  signals[SIG_LOADING_PROGRESS] =
    g_signal_new ("loading-progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_DOUBLE);

  /**
   * ChromiumSession::fullscreen-changed:
   * @self: the session
   * @fullscreen: whether the page is in fullscreen
   *
   * The page entered or left fullscreen. The helper moves its own CEF window
   * to match, and a host that owns the toplevel can follow here.
   *
   * Since: 156.0.14
   */
  signals[SIG_FULLSCREEN_CHANGED] =
    g_signal_new ("fullscreen-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::auto-resize-updated:
   * @self: the session
   * @browser: the browser whose page changed its preferred size
   * @width: the new width in logical pixels, already clamped
   * @height: the new height in logical pixels, already clamped
   *
   * The session has configured the toplevel paired with @browser to this size.
   * A host that draws the window itself (a popover, an embedded panel) follows
   * here; a host that leaves the window to the compositor has nothing to do.
   *
   * Since: 156.0.14
   */
  signals[SIG_AUTO_RESIZE_UPDATED] =
    g_signal_new ("auto-resize-updated", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::cookies-result:
   * @self: the session
   * @id: the id the host passed to the cookies call
   * @ok: whether the request succeeded
   * @json: the result object as JSON text — `cookies` (array of cookie
   *   objects) and `count` for a get, `count` and `failed` for a set,
   *   `deleted` for a delete, `error` when @ok is %FALSE
   *
   * The answer to chromium_session_cookies_get(), _set() or _delete().
   *
   * Since: 156.0.14
   */
  signals[SIG_COOKIES_RESULT] =
    g_signal_new ("cookies-result", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING);

  /**
   * ChromiumSession::history-result:
   * @self: the session
   * @id: the id the host passed to chromium_session_history()
   * @json: the result object as JSON text: `current_index` and `entries`,
   *   each entry an object of `index`, `url`, `title`, `current`
   *
   * The browser's session-history entries, oldest first.
   *
   * Since: 156.0.14
   */
  signals[SIG_HISTORY_RESULT] =
    g_signal_new ("history-result", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::page-text-begin:
   * @self: the session
   * @id: the id the host passed to chromium_session_page_text()
   * @browser: the browser whose frame was read
   * @bytes: the number of bytes that will follow, after truncation
   * @truncated: whether the text was cut at the channel's 4 MiB cap
   *
   * The frame's text arrives as one begin, ordered page-text-chunk events and
   * one page-text-end. Chunks are whole UTF-8 and only valid during their
   * emission.
   *
   * Since: 156.0.14
   */
  signals[SIG_PAGE_TEXT_BEGIN] =
    g_signal_new ("page-text-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::page-text-chunk:
   * @self: the session
   * @id: the request id
   * @data: a piece of the frame's text
   *
   * Since: 156.0.14
   */
  signals[SIG_PAGE_TEXT_CHUNK] =
    g_signal_new ("page-text-chunk", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::page-text-end:
   * @self: the session
   * @id: the request id
   * @truncated: whether the text was cut at the 4 MiB cap
   *
   * Since: 156.0.14
   */
  signals[SIG_PAGE_TEXT_END] =
    g_signal_new ("page-text-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::pdf-begin:
   * @self: the session
   * @id: the id the host passed to chromium_session_print_pdf()
   * @bytes: the number of PDF bytes that will follow, after truncation
   * @truncated: whether the PDF was cut at the byte cap
   *
   * Since: 156.0.14
   */
  signals[SIG_PDF_BEGIN] =
    g_signal_new ("pdf-begin", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::pdf-chunk:
   * @self: the session
   * @id: the request id
   * @data: the decoded PDF bytes
   *
   * Raw bytes, already base64-decoded; append them in order.
   *
   * Since: 156.0.14
   */
  signals[SIG_PDF_CHUNK] =
    g_signal_new ("pdf-chunk", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_BYTES);

  /**
   * ChromiumSession::pdf-end:
   * @self: the session
   * @id: the request id
   * @bytes: total bytes streamed
   * @truncated: whether the bytes were cut at the cap
   * @error: "" on success, or the helper's reason
   *
   * Since: 156.0.14
   */
  signals[SIG_PDF_END] =
    g_signal_new ("pdf-end", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING);

  /**
   * ChromiumSession::favicon-changed:
   * @self: the session
   * @browser: the browser whose page declared icons
   * @urls: a %NULL-terminated array of icon URLs, owned by the session for
   *   the duration of the emission
   *
   * The page's icon URLs, in the order it declared them. Which to fetch and
   * where to show it is the host's call; the helper downloads nothing.
   *
   * Since: 156.0.14
   */
  signals[SIG_FAVICON_CHANGED] =
    g_signal_new ("favicon-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRV);

  /**
   * ChromiumSession::console:
   * @self: the session
   * @browser: the browser whose page wrote the line
   * @level: "debug", "info", "warning" or "error"
   * @message: the text after console's own formatting
   * @source: the script URL it came from ("" when Chromium has none)
   * @line: the line in that script, 0 when unknown
   *
   * One per page console.* call. Without this relay the lines only exist
   * inside a DevTools agent; here they feed a host's error collection or
   * console panel. Fire-and-forget.
   *
   * Since: 156.0.14
   */
  signals[SIG_CONSOLE] =
    g_signal_new ("console", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_INT);

  /**
   * ChromiumSession::tooltip-changed:
   * @self: the session
   * @browser: the browser under the pointer
   * @text: the hover text, or "" when the tooltip goes away
   *
   * The page's hover text (a link's `title`, an element's tooltip). The
   * component suppresses CEF's own tooltip popup — drawing the hint is the
   * host's call, and the empty text is the hide half.
   *
   * Since: 156.0.14
   */
  signals[SIG_TOOLTIP_CHANGED] =
    g_signal_new ("tooltip-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::status-message:
   * @self: the session
   * @browser: the browser under the pointer
   * @text: the status text, typically a hovered link's URL
   *
   * Since: 156.0.14
   */
  signals[SIG_STATUS_MESSAGE] =
    g_signal_new ("status-message", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::focus-requested:
   * @self: the session
   * @browser: the browser that asked
   * @source: "navigation" (an API load) or "system" (a system-generated
   *   focus event, a popup's first show among them)
   *
   * The page asked for focus. The ask is relayed and granted; whether the
   * window follows is the host's activation policy.
   *
   * Since: 156.0.14
   */
  signals[SIG_FOCUS_REQUESTED] =
    g_signal_new ("focus-requested", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::media-access-changed:
   * @self: the session
   * @browser: the browser holding (or releasing) the devices
   * @video: whether a camera is in use
   * @audio: whether a microphone is in use
   *
   * A capture started or stopped. Background pages are relayed too: the mic
   * light burns while a hidden page holds the device, and a host indicator
   * that stayed dark would be lying about the hardware.
   *
   * Since: 156.0.14
   */
  signals[SIG_MEDIA_ACCESS_CHANGED] =
    g_signal_new ("media-access-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::print-requested:
   * @self: the session
   * @browser: the browser whose page asked to print
   *
   * The page called window.print(). The component runs no printer dialog —
   * printing is the host's GTK transaction — so this only reports the ask;
   * a host that wants to print answers with chromium_session_print_pdf() and
   * feeds the bytes to its own dialog. (Measured: the helper relays from the
   * start of the print transaction, which happens whether or not the
   * environment has a printer for CEF's own pipeline to consult.)
   *
   * Since: 156.0.14
   */
  signals[SIG_PRINT_REQUESTED] =
    g_signal_new ("print-requested", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  G_TYPE_INT);

  /**
   * ChromiumSession::frame-created:
   * @self: the session
   * @browser: the browser that owns the frame
   * @frame: the frame's identifier, as CEF spells it (a string; "-4"
   *   placeholders never reach here)
   * @is_main: whether this is the browser's main frame
   * @url: the frame's URL, possibly still about:blank for a fresh frame
   *
   * A frame came into existence — the main frame of a navigation or any
   * iframe the page opened. Fire-and-forget; the pair for it is
   * ::frame-destroyed.
   *
   * Since: 156.0.14
   */
  signals[SIG_FRAME_CREATED] =
    g_signal_new ("frame-created", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);

  /**
   * ChromiumSession::frame-destroyed:
   * @self: the session
   * @browser: the browser that owned the frame
   * @frame: the frame's identifier
   *
   * Since: 156.0.14
   */
  signals[SIG_FRAME_DESTROYED] =
    g_signal_new ("frame-destroyed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::frame-attached:
   * @self: the session
   * @browser: the browser that owns the frame
   * @frame: the frame's identifier
   * @reattached: whether this is a return from the back-forward cache or a
   *   recovered connection, not a first attach
   *
   * The frame can route commands to its renderer again — after a creation,
   * a cross-process swap, or a return from the back-forward cache.
   *
   * Since: 156.0.14
   */
  signals[SIG_FRAME_ATTACHED] =
    g_signal_new ("frame-attached", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN);

  /**
   * ChromiumSession::frame-detached:
   * @self: the session
   * @browser: the browser that owns the frame
   * @frame: the frame's identifier
   *
   * The frame lost its renderer — destroyed, cached, or a rare connection
   * error; ::frame-destroyed follows when it is destruction.
   *
   * Since: 156.0.14
   */
  signals[SIG_FRAME_DETACHED] =
    g_signal_new ("frame-detached", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  /**
   * ChromiumSession::main-frame-changed:
   * @self: the session
   * @browser: the browser whose main frame changed
   * @old_frame: the previous main frame's identifier, or "" on the first
   *   assignment
   * @new_frame: the new main frame's identifier, or "" on the last removal
   *
   * Cross-origin main-frame navigations swap the frame object (and usually
   * the renderer process) under the browser; this is the fact a host's
   * process-attribution needs.
   *
   * Since: 156.0.14
   */
  signals[SIG_MAIN_FRAME_CHANGED] =
    g_signal_new ("main-frame-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIG_CLOSED] =
    g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_HELPER_EXITED] =
    g_signal_new ("helper-exited", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_ZOOM_CHANGED] =
    g_signal_new ("zoom-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[SIG_COPY_TEXT] =
    g_signal_new ("copy-text", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_OPEN_URL] =
    g_signal_new ("open-url", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * ChromiumSession::view-source:
   * @self: the session
   * @browser: the browser the source came from
   * @url: the frame's URL
   * @source: the frame's HTML as Chromium serialized the DOM — script
   *   changes included, not the network's original bytes. Only valid
   *   during the emission; keep a copy to use it later.
   *
   * A View Source request completed. Emitted whether or not the session
   * opens its own window (see #ChromiumSession:auto-source-window).
   *
   * Since: 156.0.14
   */
  signals[SIG_VIEW_SOURCE] =
    g_signal_new ("view-source", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::view-source-error:
   * @self: the session
   * @browser: the browser the request was for
   * @message: why the source never arrived
   *
   * Since: 156.0.14
   */
  signals[SIG_VIEW_SOURCE_ERROR] =
    g_signal_new ("view-source-error", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_STRING);

  signals[SIG_PAGE_MESSAGE] =
    g_signal_new ("page-message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_PRIMARY_SELECTION] =
    g_signal_new ("primary-selection", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_BROWSER_CREATED] =
    g_signal_new ("browser-created", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                  G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                  G_TYPE_BOOLEAN);
  signals[SIG_BROWSER_SELECTED] =
    g_signal_new ("browser-selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_BROWSER_CLOSED] =
    g_signal_new ("browser-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  /**
   * ChromiumSession::browser-surface:
   * @self: the session
   * @browser: the browser id
   * @surface: (transfer none) (nullable): the toplevel the browser's CEF window
   *   draws into, or %NULL when that surface went away
   *
   * The browser-to-surface pairing, reported when it is made and again with
   * %NULL when the surface disappears. It can arrive before or after
   * ::browser-created; a host binding a view should handle both orders and
   * also ask chromium_session_get_browser_surface() when it needs the answer
   * outside an emission. Ownership stays with the session.
   *
   * Since: 156.0.14
   */
  signals[SIG_BROWSER_SURFACE] =
    g_signal_new ("browser-surface", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, NESTED_TYPE_SURFACE);
  signals[SIG_POPUP_REQUESTED] =
    g_signal_new ("popup-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 4,
                  G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_INT);

  /**
   * ChromiumSession::browsers-changed:
   * @self: the session
   *
   * The helper's browser table moved — a browser was created, closed or
   * selected, a title or address changed, a mute or zoom landed — and
   * chromium_session_browsers_list () now returns the new picture. There are
   * no arguments: the list is the payload.
   *
   * Since: 156.0.14
   */
  signals[SIG_BROWSERS_CHANGED] =
    g_signal_new ("browsers-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ChromiumSession::auth-required:
   * @self: the session
   * @id: the request id to answer with chromium_session_auth_respond() or
   *   chromium_session_auth_cancel()
   * @browser: the browser whose request stopped
   * @is_proxy: whether the challenge came from a proxy
   * @scheme: "basic", "digest", …
   * @realm: the challenge's realm, possibly ""
   * @origin: the URL that asked
   *
   * The request is stopped until this is answered. A handler that draws its
   * own prompt returns %TRUE and owes exactly one answer; returning %FALSE —
   * or connecting nothing — cancels, which is Chrome's own default and the
   * safe one: the request fails with its 401 and the page shows that.
   *
   * Since: 156.0.14
   */
  signals[SIG_AUTH_REQUIRED] =
    g_signal_new ("auth-required", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 6,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumSession::auth-closed:
   * @self: the session
   * @id: the request that went away
   * @browser: the browser that owned it
   *
   * The browser navigated, closed, or answered the challenge itself; a panel
   * waiting on this @id comes down, and answering it afterwards does nothing.
   *
   * Since: 156.0.14
   */
  signals[SIG_AUTH_CLOSED] =
    g_signal_new ("auth-closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::client-cert-request:
   * @self: the session
   * @id: the request id to answer with chromium_session_client_cert_respond()
   * @browser: the browser whose handshake stopped
   * @origin: "host:port" of the server (or proxy) asking
   * @certs: a GList of ChromiumClientCert* owned by the session for the
   *   duration of the emission, already pruned by Chromium to the issuers the
   *   server trusts
   *
   * A handler that offers its own picker returns %TRUE and owes exactly one
   * chromium_session_client_cert_respond(); returning %FALSE — or connecting
   * nothing — answers "none", which continues the handshake without a
   * certificate and is the only answer that cannot leak a key.
   *
   * Since: 156.0.14
   */
  signals[SIG_CLIENT_CERT_REQUEST] =
    g_signal_new ("client-cert-request", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 4,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_POINTER);

  /**
   * ChromiumSession::client-cert-closed:
   * @self: the session
   * @id: the request that went away
   * @browser: the browser that owned it
   *
   * Since: 156.0.14
   */
  signals[SIG_CLIENT_CERT_CLOSED] =
    g_signal_new ("client-cert-closed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::render-unresponsive:
   * @self: the session
   * @id: the question's id
   * @browser: the browser whose renderer stopped answering
   *
   * The page's script is stuck and its surface shows the last frame it
   * painted. Answer with chromium_session_render_unresponsive_respond();
   * not answering keeps CEF's own keep-waiting.
   *
   * Since: 156.0.14
   */
  signals[SIG_RENDER_UNRESPONSIVE] =
    g_signal_new ("render-unresponsive", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::render-responsive:
   * @self: the session
   * @id: the question that resolved itself
   * @browser: the browser that came back
   *
   * The page recovered after a ::render-unresponsive; a host panel for it
   * comes down, and answering that @id afterwards does nothing.
   *
   * Since: 156.0.14
   */
  signals[SIG_RENDER_RESPONSIVE] =
    g_signal_new ("render-responsive", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::render-unresponsive-closed:
   * @self: the session
   * @id: the question that went away
   * @browser: the browser that owned it
   *
   * Since: 156.0.14
   */
  signals[SIG_RENDER_UNRESPONSIVE_CLOSED] =
    g_signal_new ("render-unresponsive-closed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::tls-info:
   * @self: the session
   * @id: the id the host passed to chromium_session_get_tls_info()
   * @browser: the browser that was asked about
   * @ok: whether the read landed
   * @security_state: Chromium's own word — "secure", "insecure", …
   * @subject: the leaf certificate's subject display name, or ""
   * @issuer: its issuer's display name, or ""
   * @fingerprint: the SHA-256 of the leaf's DER, hex, or ""
   * @valid_from: the leaf's not-before as Unix seconds, 0 when absent
   * @valid_to: its not-after as Unix seconds, 0 when absent
   *
   * The certificate facts about a loaded page — the address bar's "查看证书"
   * reads this. The pipe is the component's own; the host never enables cdp
   * for it.
   *
   * Since: 156.0.14
   */
  signals[SIG_TLS_INFO] =
    g_signal_new ("tls-info", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 9,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                  G_TYPE_INT);

  /**
   * ChromiumSession::scheme-request:
   * @self: the session
   * @id: the request id to answer with chromium_session_scheme_response() /
   *   scheme_write() / scheme_end() / scheme_abort()
   * @browser: the browser that asked (0 for a request outside any browser)
   * @url: the full URL asked for
   * @method: "GET", "POST", …
   * @headers: the request headers as one flat JSON object text
   *
   * A request to a scheme the host registered stops here. A handler that
   * serves it returns %TRUE and owes a scheme-response then any number of
   * writes and one scheme-end; returning %FALSE — or connecting nothing —
   * aborts, because the component does not invent page data.
   *
   * Since: 156.0.14
   */
  signals[SIG_SCHEME_REQUEST] =
    g_signal_new ("scheme-request", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 5,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                  G_TYPE_STRING);

  /**
   * ChromiumSession::scheme-closed:
   * @self: the session
   * @id: the request that went away
   * @browser: the browser that owned it
   *
   * The browser went away mid-request; anything still owed for this @id is
   * moot, and answering it afterwards does nothing.
   *
   * Since: 156.0.14
   */
  signals[SIG_SCHEME_CLOSED] =
    g_signal_new ("scheme-closed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_INT, G_TYPE_INT);

  /**
   * ChromiumSession::screenshot-result:
   * @self: the session
   * @id: the id the host passed to chromium_session_capture_screenshot()
   * @browser: the browser that was captured
   * @ok: whether the capture landed
   * @png: the PNG bytes on success, NULL otherwise
   * @message: the reason on failure ("screenshot-too-large" among them), ""
   *   on success
   *
   * Since: 156.0.14
   */
  signals[SIG_SCREENSHOT_RESULT] =
    g_signal_new ("screenshot-result", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 5,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_BYTES,
                  G_TYPE_STRING);

  /**
   * ChromiumSession::window-properties:
   * @self: the session
   * @browser: the windowed browser whose window this describes
   * @x: window x, logical pixels
   * @y: window y
   * @w: window width
   * @h: window height
   * @toolbar: whether a toolbar row is present (this helper: always FALSE)
   * @menubar: whether a menu bar is present (always FALSE)
   * @status: whether a status bar is present (always FALSE)
   * @resizable: whether the window can be resized
   * @fullscreen: whether the window is fullscreen right now
   *
   * Fires when the window is created and whenever its bounds or fullscreen
   * state change: the host's picture of a window.open, reported.
   *
   * Since: 156.0.14
   */
  signals[SIG_WINDOW_PROPERTIES] =
    g_signal_new ("window-properties", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 10,
                  G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
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
  self->js_dialog_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->file_dialog_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->certificate_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->auth_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->client_cert_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->scheme_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->intercept_held = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->browser_table = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL,
                                               (GDestroyNotify)
                                               chromium_session_browser_info_free);
  self->schemes = g_ptr_array_new_with_free_func
    ((GDestroyNotify) scheme_entry_free);
  self->browser_surfaces = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, g_object_unref);
  self->surface_browser_ids = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->seen_toplevels = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                NULL, g_object_unref);
  self->unpaired_surfaces = g_queue_new ();
  self->pending_windowed_ids = g_queue_new ();
  /* The property spec's default does not reach the field on its own: without
   * this the pspec says TRUE and the session still ships windows closed. */
  self->auto_source_window = TRUE;
  /* Nothing has started, so there is nothing to report as closed yet. */
  self->closed_emitted = TRUE;
  self->a11y_forced = g_strcmp0 (g_getenv ("CHROMIUM_A11Y"), "1") == 0;
}

static void
chromium_session_get_property (GObject *object, guint property,
                               GValue *value, GParamSpec *pspec)
{
  ChromiumSession *self = CHROMIUM_SESSION (object);

  switch (property)
    {
    case PROP_AUTO_SOURCE_WINDOW:
      g_value_set_boolean (value, self->auto_source_window);
      break;
    case PROP_DATA_DIRECTORY:
      g_value_set_string (value, self->data_directory);
      break;
    case PROP_DEVTOOLS_ENABLED:
      g_value_set_boolean (value, self->devtools_enabled);
      break;
    case PROP_PROXY:
      g_value_set_string (value, self->proxy);
      break;
    case PROP_PROXY_BYPASS:
      g_value_set_string (value, self->proxy_bypass);
      break;
    case PROP_ACCEPT_LANGUAGE:
      g_value_set_string (value, self->accept_language);
      break;
    case PROP_AUTO_RESIZE:
      g_value_set_boolean (value, self->auto_resize);
      break;
    case PROP_AUTO_RESIZE_MIN_WIDTH:
      g_value_set_int (value, self->auto_resize_min_width);
      break;
    case PROP_AUTO_RESIZE_MIN_HEIGHT:
      g_value_set_int (value, self->auto_resize_min_height);
      break;
    case PROP_AUTO_RESIZE_MAX_WIDTH:
      g_value_set_int (value, self->auto_resize_max_width);
      break;
    case PROP_AUTO_RESIZE_MAX_HEIGHT:
      g_value_set_int (value, self->auto_resize_max_height);
      break;
    case PROP_USER_AGENT:
      g_value_set_string (value, self->user_agent);
      break;
    case PROP_AUTOPLAY_POLICY:
      g_value_set_string (value, self->autoplay_policy);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property, pspec);
    }
}

static void
chromium_session_set_property (GObject *object, guint property,
                               const GValue *value, GParamSpec *pspec)
{
  ChromiumSession *self = CHROMIUM_SESSION (object);

  switch (property)
    {
    case PROP_AUTO_SOURCE_WINDOW:
      self->auto_source_window = g_value_get_boolean (value);
      break;
    case PROP_DATA_DIRECTORY:
      chromium_session_set_data_directory (self, g_value_get_string (value));
      break;
    case PROP_DEVTOOLS_ENABLED:
      if (self->devtools_enabled != g_value_get_boolean (value))
        {
          self->devtools_enabled = g_value_get_boolean (value);
          session_send_devtools_state (self);
        }
      break;
    case PROP_PROXY:
      chromium_session_set_proxy (self, g_value_get_string (value));
      break;
    case PROP_PROXY_BYPASS:
      chromium_session_set_proxy_bypass (self, g_value_get_string (value));
      break;
    case PROP_ACCEPT_LANGUAGE:
      chromium_session_set_accept_language (self, g_value_get_string (value));
      break;
    case PROP_AUTO_RESIZE:
      {
        gboolean enabled = g_value_get_boolean (value);

        if (self->auto_resize != enabled)
          {
            self->auto_resize = enabled;
            session_send_auto_resize (self);
          }
      }
      break;
    case PROP_AUTO_RESIZE_MIN_WIDTH:
      self->auto_resize_min_width = g_value_get_int (value);
      goto bounds_changed;
    case PROP_AUTO_RESIZE_MIN_HEIGHT:
      self->auto_resize_min_height = g_value_get_int (value);
      goto bounds_changed;
    case PROP_AUTO_RESIZE_MAX_WIDTH:
      self->auto_resize_max_width = g_value_get_int (value);
      goto bounds_changed;
    case PROP_AUTO_RESIZE_MAX_HEIGHT:
      self->auto_resize_max_height = g_value_get_int (value);
    bounds_changed:
      session_send_auto_resize (self);
      break;
    case PROP_USER_AGENT:
      chromium_session_set_user_agent (self, g_value_get_string (value));
      break;
    case PROP_AUTOPLAY_POLICY:
      chromium_session_set_autoplay_policy (self,
                                            g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property, pspec);
    }
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

  /* The claim outlives the process only while a helper holds the directory;
   * a disposed session lets go of the path so another one may try it. */
  release_data_directory (self);

  if (self->child)
    {
      /* A helper that announced its own exit is inside CefShutdown() right now,
       * and its profile lock is already gone — SIGKILLing it here would cut
       * that shutdown short at the exact moment we are no longer watching. The
       * pending child_waited holds a reference, so the object still goes away
       * with the process. */
      if (!self->helper_is_exiting)
        {
          gboolean ephemeral = self->child_ephemeral;

          g_subprocess_force_exit (self->child);
          if (ephemeral)
            remove_child_scratch (self->child);
        }
      g_clear_object (&self->child);
    }

  close_listener (self);
  drop_connection (self);

  /* The retry is armed against a descriptor shortage that outlives this object,
   * and it holds no reference of its own: it goes here rather than firing into a
   * session that has already been finalized. */
  g_clear_handle_id (&self->accept_retry_id, g_source_remove);

  /* The menu is the view's child, so it goes with the view; asking is already
   * off, and its ::closed must not answer a helper that is going away. */
  self->menu_pending = FALSE;
  menu_release (self);

  /* Native plugins first: their session-signal handlers are pages of code the
   * manager's unloading unmaps, and the emissions below must not reach them.
   * Same rule as ::closed; both paths can run, and unloading twice is a
   * no-op. The Web frontend flushes whatever storage is dirty on its way
   * out. */
  chromium_plugin_native_unload_all (self->plugin_manager);
  chromium_plugin_web_detach (self->plugin_manager);
  g_clear_object (&self->plugin_manager);

  /* A half-streamed source belongs to a helper that is going away, and the
   * window is this side's own: close both with the session. */
  source_transfer_reset (self);
  if (self->source_window)
    gtk_window_destroy (GTK_WINDOW (self->source_window));

  if (self->a11y_proxy)
    {
      if (self->a11y_handler != 0)
        g_signal_handler_disconnect (self->a11y_proxy, self->a11y_handler);
      self->a11y_handler = 0;
      g_clear_object (&self->a11y_proxy);
    }
  /* The in-flight proxy call still holds a ref and will run its callback; the
   * pending flag is what tells it the session is on its way out. */
  self->a11y_proxy_pending = FALSE;

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
  g_free (self->data_directory);
  g_clear_pointer (&self->claimed_data_directory, g_free);
  g_free (self->proxy);
  g_free (self->proxy_bypass);
  g_free (self->accept_language);
  g_free (self->autoplay_policy);
  g_hash_table_destroy (self->permission_held);
  g_hash_table_destroy (self->js_dialog_held);
  g_hash_table_destroy (self->file_dialog_held);
  g_hash_table_destroy (self->certificate_held);
  g_hash_table_destroy (self->auth_held);
  g_hash_table_destroy (self->client_cert_held);
  g_hash_table_destroy (self->scheme_held);
  g_hash_table_destroy (self->intercept_held);
  g_clear_pointer (&self->browser_table, g_hash_table_unref);
  g_clear_pointer (&self->schemes, g_ptr_array_unref);
  g_free (self->user_agent);
  g_clear_pointer (&self->plugins_sent, g_hash_table_unref);
  g_clear_pointer (&self->browser_urls, g_hash_table_unref);
  g_clear_pointer (&self->background_browsers, g_hash_table_unref);
  g_clear_pointer (&self->browser_surfaces, g_hash_table_unref);
  g_clear_pointer (&self->surface_browser_ids, g_hash_table_unref);
  g_clear_pointer (&self->seen_toplevels, g_hash_table_unref);
  if (self->unpaired_surfaces)
    {
      g_queue_free_full (self->unpaired_surfaces, g_object_unref);
      self->unpaired_surfaces = NULL;
    }
  g_clear_pointer (&self->pending_windowed_ids, g_queue_free);

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
