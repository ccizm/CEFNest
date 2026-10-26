/* The plugin model without a compositor, a helper or a display: what a manifest
 * becomes, what refuses one, what the permission table does to an enable, and
 * what a rescan keeps.
 *
 * The runtime half — content scripts on a page, a native registry, the channel
 * traffic — is not here because none of it is wired yet. What this can pin down
 * is the part the host's own UI binds to. */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "chromium-plugin.h"
/* The install root is session-wired plumbing: the session derives it from
 * the data directory, and these tests drive the same setter directly. */
#include "chromium-plugin-private.h"

typedef struct {
  char *dir;
  ChromiumPluginManager *manager;
  GPtrArray *errors;   /* "id message" for every ::plugin-error */
  GPtrArray *grants;   /* every ::grant-request's permission list */
  GPtrArray *rows;     /* the references require_plugin() hands out */
  /* The last parked request, referenced: the tests answer it by hand. */
  ChromiumPluginPermissionRequest *request;
} Fixture;

static void
remove_tree (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name;

  if (!dir)
    return;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *child = g_build_filename (path, name, NULL);

      if (g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_tree (child);
      else
        g_unlink (child);
    }

  g_dir_close (dir);
  g_rmdir (path);
}

static void
on_plugin_error (ChromiumPluginManager *manager, const char *id,
                 const char *message, gpointer data)
{
  Fixture *fx = data;

  g_ptr_array_add (fx->errors, g_strdup_printf ("%s %s", id, message));
}

/* The host that answers for the names it is handed, which is the shape a real
 * grant dialog is: it decides before the emission returns. It never touches the
 * request object — granting the whole set inside the emission is an answer on
 * its own. */
static void
on_grant_request (ChromiumPluginManager *manager, const char *id,
                  const char *const *permissions,
                  ChromiumPluginPermissionRequest *request, gpointer data)
{
  Fixture *fx = data;
  guint i;

  g_ptr_array_add (fx->grants, g_strdup (id));

  for (i = 0; permissions && permissions[i]; i++)
    chromium_plugin_manager_grant (manager, id, permissions[i], NULL);
}

/* The dialog-on-the-main-loop shape: keep the request and answer later. */
static void
on_grant_park (ChromiumPluginManager *manager, const char *id,
               const char *const *permissions,
               ChromiumPluginPermissionRequest *request, gpointer data)
{
  Fixture *fx = data;

  g_ptr_array_add (fx->grants, g_strdup (id));
  g_set_object (&fx->request, request);
}

static void
fixture_set_up (Fixture *fx, gconstpointer data)
{
  GError *error = NULL;

  fx->dir = g_dir_make_tmp ("cefnest-plugin-XXXXXX", &error);
  g_assert_nonnull (fx->dir);

  fx->manager = chromium_plugin_manager_new ();
  fx->errors = g_ptr_array_new_with_free_func (g_free);
  fx->grants = g_ptr_array_new_with_free_func (g_free);
  fx->rows = g_ptr_array_new_with_free_func (g_object_unref);

  g_signal_connect (fx->manager, "plugin-error",
                    G_CALLBACK (on_plugin_error), fx);
}

static void
fixture_tear_down (Fixture *fx, gconstpointer data)
{
  remove_tree (fx->dir);
  g_free (fx->dir);
  g_clear_object (&fx->manager);
  g_clear_object (&fx->request);
  g_clear_pointer (&fx->errors, g_ptr_array_unref);
  g_clear_pointer (&fx->grants, g_ptr_array_unref);
  g_clear_pointer (&fx->rows, g_ptr_array_unref);
}

/* The installed glib still spells g_strv_length()'s argument non-const, and the
 * model hands out const arrays because nothing outside it may resize them. */
static guint
strv_length (const char *const *str_array)
{
  return g_strv_length ((char **) str_array);
}

/* A package directory under any parent, because the collision test needs roots
 * that are not the fixture's own. */
static void
write_plugin (const char *parent, const char *id, const char *manifest)
{
  g_autofree char *dir = g_build_filename (parent, id, NULL);
  g_autofree char *path = NULL;

  g_mkdir_with_parents (dir, 0755);

  if (manifest)
    {
      path = g_build_filename (dir, CHROMIUM_PLUGIN_MANIFEST_NAME, NULL);
      g_file_set_contents (path, manifest, -1, NULL);
    }
}

static void
add_plugin (Fixture *fx, const char *id, const char *manifest)
{
  write_plugin (fx->dir, id, manifest);
}

static gboolean
scanned (Fixture *fx)
{
  GError *error = NULL;
  gboolean ok;

  ok = chromium_plugin_manager_scan (fx->manager, &error);
  g_assert_no_error (error);
  return ok;
}

/* lookup() hands out an owned reference, so the fixture keeps it: a row a test
 * points at has to survive the rescan that replaces it in the model. */
static ChromiumPlugin *
require_plugin (Fixture *fx, const char *id)
{
  ChromiumPlugin *plugin = chromium_plugin_manager_lookup (fx->manager, id);

  g_assert_nonnull (plugin);
  g_ptr_array_add (fx->rows, plugin);
  return plugin;
}

static gboolean
errors_report (Fixture *fx, const char *what)
{
  guint i;

  for (i = 0; i < fx->errors->len; i++)
    if (g_str_has_suffix (g_ptr_array_index (fx->errors, i), what))
      return TRUE;

  return FALSE;
}

static const char *const kGoodManifest =
  "{"
  "  \"manifest_version\": 3,"
  "  \"name\": \"Demo\","
  "  \"version\": \"1.0\","
  "  \"description\": \"A demonstration\","
  "  \"permissions\": [\"storage\", \"tabs\"],"
  "  \"content_scripts\": [{ \"matches\": [\"https://example.com/*\"],"
  "                         \"js\": [\"cs.js\"], \"run_at\": \"document_idle\" }],"
  "  \"action\": { \"default_title\": \"Demo\", \"default_popup\": \"popup.html\" },"
  "  \"options_ui\": { \"page\": \"options.html\", \"open_in_tab\": true },"
  "  \"icons\": { \"16\": \"i16.png\", \"64\": \"icons/i64.png\" },"
  "  \"cefnest\": { \"type\": \"web\", \"popup\": { \"width\": 400, \"height\": 600 } }"
  "}";

/* The install root is where a profile's own plugins live, and the rules it
   adds to the scan are the ones the data directory's contract implies: a
   root that does not exist yet is an empty list (a fresh profile has
   installed nothing), a root that does exists is scanned before every
   host-added search path (the user's own install wins a duplicate id), and
   a host that adds the same path explicitly gets it scanned once. */
static void
test_install_root (Fixture *fx, gconstpointer data)
{
  g_autofree char *root = NULL;
  g_autofree char *second = NULL;
  ChromiumPlugin *plugin;
  GError *error = NULL;

  /* Missing: an empty list, not a failed scan. */
  root = g_build_filename (fx->dir, "not-created-yet", NULL);
  _chromium_plugin_manager_set_install_root (fx->manager, root);
  g_assert_true (scanned (fx));
  g_assert_cmpuint (g_list_model_get_n_items (
                      chromium_plugin_manager_get_plugins (fx->manager)), ==, 0);

  /* Created: scanned first, ahead of a search path that holds the same id. */
  g_mkdir_with_parents (root, 0755);
  write_plugin (root, "demo", kGoodManifest);
  second = g_build_filename (fx->dir, "second", NULL);
  write_plugin (second, "demo",
                "{"
                "  \"manifest_version\": 3,"
                "  \"name\": \"Second\","
                "  \"version\": \"2.0\","
                "  \"cefnest\": { \"type\": \"web\" }"
                "}");
  chromium_plugin_manager_add_search_path (fx->manager, second, &error);
  g_assert_no_error (error);

  g_assert_true (scanned (fx));
  g_assert_cmpuint (g_list_model_get_n_items (
                      chromium_plugin_manager_get_plugins (fx->manager)), ==, 2);
  /* Two rows named demo: the install root's row is the valid first one, the
     search path's row is the refused duplicate. */
  plugin = require_plugin (fx, "demo");
  g_assert_cmpstr (chromium_plugin_get_name (plugin), ==, "Demo");
  /* The row's root is the package's own directory, one level under the
     install root — and it is the install root's package, not the search
     path's, that kept the valid row. */
  {
    g_autofree char *expected = g_build_filename (root, "demo", NULL);

    g_assert_cmpstr (chromium_plugin_get_root (plugin), ==, expected);
  }
  g_assert_true (errors_report (fx, "duplicate-id"));
  /* remove_range, not a bare len=0: the array's free func owns the strings,
     and truncating the length directly would leak every one of them. */
  g_ptr_array_remove_range (fx->errors, 0, fx->errors->len);

  /* Clearing the root drops its row on the next scan; the search path's
     copy is all that is left, and it takes over as the valid one. */
  _chromium_plugin_manager_set_install_root (fx->manager, NULL);
  g_assert_true (scanned (fx));
  g_assert_cmpuint (g_list_model_get_n_items (
                      chromium_plugin_manager_get_plugins (fx->manager)), ==, 1);
  plugin = require_plugin (fx, "demo");
  g_assert_cmpstr (chromium_plugin_get_name (plugin), ==, "Second");
  g_assert_false (errors_report (fx, "duplicate-id"));
}

static void
test_search_path (void)
{
  g_autoptr (ChromiumPluginManager) manager = chromium_plugin_manager_new ();
  GError *error = NULL;

  /* Absolute or it resolves against whatever directory the process was started
   * in, which is a different answer for a launcher, a test and an icon click. */
  g_assert_false (chromium_plugin_manager_add_search_path (manager,
                                                           "relative/plugins",
                                                           &error));
  g_assert_nonnull (error);
  g_assert_cmpint (error->domain, ==, G_IO_ERROR);
  g_assert_cmpint (error->code, ==, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (chromium_plugin_manager_add_search_path (manager,
                                                           "/definitely/not/here",
                                                           &error));
  g_assert_cmpint (error->code, ==, G_IO_ERROR_NOT_DIRECTORY);
  g_clear_error (&error);

  g_assert_true (chromium_plugin_manager_add_search_path (manager, "/", &error));
  g_assert_no_error (error);

  /* The same path twice is one path: a host that rebuilds its configuration
   * must not end up scanning, and complaining about, it twice. */
  g_assert_true (chromium_plugin_manager_add_search_path (manager, "/", NULL));
}

static void
test_scan_model (Fixture *fx, gconstpointer data)
{
  GListModel *model;
  g_autoptr (ChromiumPlugin) one = NULL;
  g_autoptr (ChromiumPlugin) two = NULL;
  int width, height;

  add_plugin (fx, "beta", kGoodManifest);
  add_plugin (fx, "alpha", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));

  model = chromium_plugin_manager_get_plugins (fx->manager);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 2);

  /* Ordered by id, so a bound list is stable across scans without the host
   * sorting anything. */
  one = g_list_model_get_item (model, 0);
  two = g_list_model_get_item (model, 1);
  g_assert_cmpstr (chromium_plugin_get_id (one), ==, "alpha");
  g_assert_cmpstr (chromium_plugin_get_id (two), ==, "beta");

  g_assert_cmpint (chromium_plugin_get_state (one), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpstr (chromium_plugin_get_name (one), ==, "Demo");
  g_assert_cmpstr (chromium_plugin_get_version (one), ==, "1.0");
  g_assert_cmpstr (chromium_plugin_get_description (one), ==, "A demonstration");
  g_assert_cmpstr (chromium_plugin_get_kind (one), ==, "web");
  g_assert_null (chromium_plugin_get_error_message (one));
  g_assert_false (chromium_plugin_get_enabled (one));
  g_assert_true (chromium_plugin_get_options_open_in_tab (one));

  {
    const char *const *permissions = chromium_plugin_get_permissions (one);

    g_assert_nonnull (permissions);
    g_assert_cmpuint (strv_length (permissions), ==, 2);
    g_assert_true (g_strv_contains (permissions, "storage"));
    g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (one)),
                      ==, 0);
  }

  chromium_plugin_get_popup_size (one, &width, &height);
  g_assert_cmpint (width, ==, 400);
  g_assert_cmpint (height, ==, 600);

  /* A property read through g_object_get hands back a copy, which is what makes
   * it safe to bind into a row that outlives this emission. */
  {
    g_autofree char *name = NULL;
    gboolean enabled = TRUE;

    g_object_get (one, "name", &name, "enabled", &enabled, NULL);
    g_assert_cmpstr (name, ==, "Demo");
    g_assert_false (enabled);
  }

  /* A directory with no manifest is not a plugin, but it is still a directory
   * somebody is looking at. */
  add_plugin (fx, "empty-dir", NULL);
  g_assert_true (scanned (fx));
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 3);
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx,
                                                                     "empty-dir")),
                   ==, "no-manifest");
  g_assert_cmpint (chromium_plugin_get_state (require_plugin (fx, "empty-dir")),
                   ==, CHROMIUM_PLUGIN_STATE_REFUSED);
}

static void
test_refusals (Fixture *fx, gconstpointer data)
{
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));

  add_plugin (fx, "nofile", NULL);
  /* The name is the host part of every URI the plugin could be given. */
  add_plugin (fx, "Bad_Name", kGoodManifest);
  add_plugin (fx, "noversion", "{ \"manifest_version\": 3, \"name\": \"x\" }");
  add_plugin (fx, "mv2", "{ \"manifest_version\": 2, \"name\": \"x\", \"version\": \"1\" }");
  add_plugin (fx, "brokentype",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"cefnest\": { \"type\": \"legacy\" } }");
  add_plugin (fx, "nosuchtype",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\" }");
  add_plugin (fx, "escaping",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"cefnest\": { \"type\": \"web\" },"
              "  \"action\": { \"default_popup\": \"../../etc/passwd\" } }");
  add_plugin (fx, "notjson", "{ this is not json");

  g_assert_true (scanned (fx));

  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "nofile")),
                   ==, "no-manifest");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "Bad_Name")),
                   ==, "bad-id");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "noversion")),
                   ==, "bad-manifest");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "mv2")),
                   ==, "bad-manifest");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "brokentype")),
                   ==, "unsupported-type");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "nosuchtype")),
                   ==, "bad-manifest");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "notjson")),
                   ==, "bad-manifest");

  /* A path that leaves the plugin directory refuses the package, and takes its
   * pages with it: a refusal must not leave a loadable URI behind. */
  {
    ChromiumPlugin *escaping = require_plugin (fx, "escaping");

    g_assert_cmpstr (chromium_plugin_get_error_message (escaping), ==,
                     "bad-manifest");
    g_assert_null (chromium_plugin_get_popup_uri (escaping));
    g_assert_null (chromium_plugin_get_options_uri (escaping));
  }

  /* Every one of those is also on the signal, with the id it belongs to. */
  g_assert_true (errors_report (fx, "no-manifest"));
  g_assert_true (errors_report (fx, "bad-id"));
  g_assert_true (errors_report (fx, "unsupported-type"));
  g_assert_true (errors_report (fx, "Bad_Name bad-id"));

  /* A refused plugin cannot be talked out of it. */
  {
    GError *error = NULL;

    g_assert_false (chromium_plugin_manager_enable (fx->manager, "nofile", &error));
    g_assert_cmpint (error->domain, ==, G_IO_ERROR);
    g_assert_cmpint (error->code, ==, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull (strstr (error->message, "no-manifest"));
    g_clear_error (&error);

    g_assert_false (chromium_plugin_manager_enable (fx->manager, "absent", &error));
    g_assert_cmpint (error->code, ==, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);
  }
}

static void
test_unsupported_fields (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;

  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));

  /* background.service_worker is the one MV3 field this component acts on
     since the resident-page shape: the path lands in the plugin and no
     unsupported-field is reported for it. Every other background shape is
     still named as unsupported, and omnibox is on the never-acted-on list. */
  add_plugin (fx, "withsw",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"background\": { \"service_worker\": \"bw.js\" },"
              "  \"omnibox\": { \"key_binding\": \"ctrl+k\" },"
              "  \"cefnest\": { \"type\": \"web\" } }");
  add_plugin (fx, "withpage",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"background\": { \"page\": \"bg.html\" },"
              "  \"cefnest\": { \"type\": \"web\" } }");
  g_assert_true (scanned (fx));

  plugin = require_plugin (fx, "withsw");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_false (errors_report (fx, "unsupported-field:background.service_worker"));
  g_assert_true (errors_report (fx, "unsupported-field:omnibox"));
  /* The resident page is the synthesized wrapper, not the script itself. */
  g_assert_cmpstr (chromium_plugin_get_background_uri (plugin), ==,
                   "cefnest-extension://withsw/__cefnest_background__.html");
  plugin = require_plugin (fx, "withpage");
  g_assert_true (errors_report (fx, "unsupported-field:background"));
  g_assert_null (chromium_plugin_get_background_uri (plugin));

  /* The two frontends are alternatives, and library is the marker that says
   * so: a web package naming one is refused. A native package with content
   * scripts is not a mix — the plan's own example plugin is one .so plus a
   * cs.js, and it scans. */
  add_plugin (fx, "native-with-cs",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"content_scripts\": [{ \"matches\": [\"<all_urls>\"],"
              "                         \"js\": [\"cs.js\"] }],"
              "  \"cefnest\": { \"type\": \"native\", \"library\": \"p.so\" } }");
  add_plugin (fx, "web-with-lib",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"cefnest\": { \"type\": \"web\", \"library\": \"p.so\" } }");
  add_plugin (fx, "native-nolib",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"cefnest\": { \"type\": \"native\" } }");
  g_assert_true (scanned (fx));

  {
    ChromiumPlugin *native_cs = require_plugin (fx, "native-with-cs");

    g_assert_cmpint (chromium_plugin_get_state (native_cs), ==,
                     CHROMIUM_PLUGIN_STATE_SCANNED);
    g_assert_cmpstr (chromium_plugin_get_kind (native_cs), ==, "native");
  }
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "web-with-lib")),
                   ==, "bad-manifest");
  g_assert_cmpstr (chromium_plugin_get_error_message (require_plugin (fx, "native-nolib")),
                   ==, "bad-manifest");
  g_assert_cmpstr (chromium_plugin_get_kind (require_plugin (fx, "native-nolib")),
                   ==, "native");
}

static void
test_popup_size (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  int width, height;

  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));

  add_plugin (fx, "huge",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"action\": { \"default_popup\": \"popup.html\" },"
              "  \"cefnest\": { \"type\": \"web\","
              "                 \"popup\": { \"width\": 4000, \"height\": 900 } } }");
  add_plugin (fx, "none",
              "{ \"manifest_version\": 3, \"name\": \"x\", \"version\": \"1\","
              "  \"cefnest\": { \"type\": \"web\" } }");
  g_assert_true (scanned (fx));

  plugin = require_plugin (fx, "huge");
  chromium_plugin_get_popup_size (plugin, &width, &height);
  g_assert_cmpint (width, ==, CHROMIUM_PLUGIN_POPUP_MAX_WIDTH);
  g_assert_cmpint (height, ==, CHROMIUM_PLUGIN_POPUP_MAX_HEIGHT);
  g_assert_true (errors_report (fx, "popup-size-clamped"));

  /* Unspecified is the default, and a plugin with no popup has none — both
   * getters answer NULL rather than a URI built out of nothing. */
  plugin = require_plugin (fx, "none");
  chromium_plugin_get_popup_size (plugin, &width, &height);
  g_assert_cmpint (width, ==, CHROMIUM_PLUGIN_POPUP_DEFAULT_WIDTH);
  g_assert_cmpint (height, ==, CHROMIUM_PLUGIN_POPUP_DEFAULT_HEIGHT);
  g_assert_null (chromium_plugin_get_popup_uri (plugin));
  g_assert_null (chromium_plugin_get_options_uri (plugin));
}

static void
test_uris (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  g_autofree char *uri = NULL;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));

  plugin = require_plugin (fx, "demo");

  g_assert_cmpstr (chromium_plugin_get_popup_uri (plugin), ==,
                   "cefnest-extension://demo/popup.html");
  g_assert_cmpstr (chromium_plugin_get_options_uri (plugin), ==,
                   "cefnest-extension://demo/options.html");

  uri = chromium_plugin_build_uri (plugin, "img/icon.png", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (uri, ==, "cefnest-extension://demo/img/icon.png");
  g_clear_pointer (&uri, g_free);

  /* The forms that put the path somewhere other than where it was written, or
   * that arrive in a form normalisation can move: every one of them has to be
   * refused here as well as in the helper. */
  {
    const char *const bad[] = {
      NULL,
      "",
      "/etc/passwd",
      "../secret",
      "a/../../secret",
      "./relative-escape/../../x",
      "a/../b",
      "..%2F..%2Fetc%2Fpasswd",
      "a\\..\\b",
      "C:\\windows\\system32",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (bad); i++)
      {
        uri = chromium_plugin_build_uri (plugin, bad[i], &error);
        g_assert_null (uri);
        g_assert_nonnull (error);
        g_assert_cmpint (error->domain, ==, G_IO_ERROR);
        g_assert_cmpint (error->code, ==, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error (&error);
      }
  }

  /* Icons come off the disk, not off the scheme: the origin is served inside the
   * helper process and this side cannot fetch from it. */
  {
    g_autofree char *icon = chromium_plugin_icon_path (plugin, "16");
    g_autofree char *nested_icon = chromium_plugin_icon_path (plugin, "64");
    g_autofree char *path = g_build_filename (fx->dir, "demo", "i16.png", NULL);
    g_autofree char *nested = g_build_filename (fx->dir, "demo", "icons",
                                                "i64.png", NULL);

    g_assert_cmpstr (icon, ==, path);
    g_assert_cmpstr (nested_icon, ==, nested);
    g_assert_null (chromium_plugin_icon_path (plugin, "128"));
    g_assert_null (chromium_plugin_icon_path (plugin, ""));
  }
}

static void
test_grant_machine (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");

  /* Nobody is listening: the request goes out, nothing is granted, and an
   * unanswered request is a deny. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_cmpint (error->code, ==, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_nonnull (strstr (error->message, "storage"));
  g_assert_nonnull (strstr (error->message, "tabs"));
  g_clear_error (&error);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpuint (fx->grants->len, ==, 0);

  /* Half of it. */
  g_assert_true (chromium_plugin_manager_grant (fx->manager, "demo", "storage",
                                                &error));
  g_assert_no_error (error);
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_cmpint (error->code, ==, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&error);

  /* A name the manifest never declared cannot be honoured, so accepting it would
     be a table entry that does nothing. */
  g_assert_false (chromium_plugin_manager_grant (fx->manager, "demo", "clipboardRead",
                                                 &error));
  g_assert_cmpint (error->code, ==, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_true (chromium_plugin_manager_grant (fx->manager, "demo", "tabs", &error));
  g_assert_no_error (error);

  /* Idempotent both ways: enabling twice and granting twice answer the same as
     the first time, because a UI that re-sends a click must not see a failure. */
  g_assert_true (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_no_error (error);
  g_assert_true (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_true (chromium_plugin_manager_grant (fx->manager, "demo", "tabs", &error));
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
  g_assert_true (chromium_plugin_get_enabled (plugin));
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 2);

  /* Turning it off keeps the grants: the host decides which of them to take
     back, and that is revoke(). */
  chromium_plugin_manager_disable (fx->manager, "demo");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 2);

  /* ...and taking one back takes the plugin out of service with it. */
  g_assert_true (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_no_error (error);
  g_assert_true (chromium_plugin_manager_revoke (fx->manager, "demo", "storage",
                                                 &error));
  g_assert_no_error (error);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);

  /* An unknown id and an already-off plugin are the same news to a host that
     asked for it not to run. */
  chromium_plugin_manager_disable (fx->manager, "absent");
  chromium_plugin_manager_disable (fx->manager, "demo");
}

static void
test_grant_handler (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");

  /* The other shape of host: the one that answers the request before returning
     from it gets an enabled plugin out of the same call. */
  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_request),
                    fx);
  g_assert_true (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_no_error (error);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
  g_assert_cmpuint (fx->grants->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fx->grants, 0), ==, "demo");
}

static void
count_notify (GObject *object, GParamSpec *pspec, gpointer data)
{
  guint *counter = data;

  (*counter)++;
}

static void
test_enabled_property (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  guint changed = 0;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");

  /* A switch bound two ways writes the property rather than calling the verb, so
   * the write has to run the same machine — and read back what it settled on,
   * which is "still off" while nothing is granted. */
  g_object_set (plugin, "enabled", TRUE, NULL);
  g_assert_false (chromium_plugin_get_enabled (plugin));

  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_request),
                    fx);
  g_object_set (plugin, "enabled", TRUE, NULL);
  g_assert_true (chromium_plugin_get_enabled (plugin));

  g_object_set (plugin, "enabled", FALSE, NULL);
  g_assert_false (chromium_plugin_get_enabled (plugin));

  /* Both properties move together, or a row bound to one sits there looking
   * enabled while the model says otherwise. */
  g_signal_connect (plugin, "notify::state", G_CALLBACK (count_notify),
                    &changed);
  g_object_set (plugin, "enabled", TRUE, NULL);
  g_assert_cmpuint (changed, >, 0);
}

static void
test_permission_parked (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  ChromiumPluginPermissionRequest *request;
  GError *error = NULL;
  guint changed = 0;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");

  /* A host that keeps the request instead of answering inside the emission
   * parks the enable: FALSE with PENDING, the plugin still scanned, and the
   * request carrying the rows the host was asked about. */
  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_park),
                    fx);
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpuint (fx->grants->len, ==, 1);
  request = fx->request;
  g_assert_nonnull (request);
  g_assert_cmpstr (chromium_plugin_permission_request_get_id (request), ==,
                   "demo");
  g_assert_cmpuint (
    strv_length (chromium_plugin_permission_request_get_permissions (request)),
    ==, 2);

  /* A second enable while the first is parked decides from the table — empty,
   * so denied — and asks nobody a second time. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&error);
  g_assert_cmpuint (fx->grants->len, ==, 1);

  /* accept() fills the table and settles the waiting enable; ::changed is how
   * the host learns the parked call landed. */
  g_signal_connect (fx->manager, "changed", G_CALLBACK (count_notify),
                    &changed);
  chromium_plugin_permission_request_accept (request);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 2);
  g_assert_cmpuint (changed, >, 0);

  /* Settled is settled: the three pens answer nothing and change nothing. */
  chromium_plugin_permission_request_accept (request);
  chromium_plugin_permission_request_deny (request);
  chromium_plugin_permission_request_settle (request);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
}

static void
test_permission_answers (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  ChromiumPluginPermissionRequest *request;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");
  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_park),
                    fx);

  /* deny() leaves the table alone and fails the enable. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  chromium_plugin_permission_request_deny (fx->request);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 0);

  /* The per-permission path: the host ticks one row, settle() decides from the
   * table — one short, so still scanned — and the next enable asks again. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  request = g_object_ref (fx->request);
  g_assert_true (chromium_plugin_manager_grant (fx->manager, "demo", "storage",
                                                NULL));
  chromium_plugin_permission_request_settle (request);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_object_unref (request);

  /* With everything granted, settle() is accept() without the granting. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  g_assert_true (chromium_plugin_manager_grant (fx->manager, "demo", "tabs",
                                                NULL));
  chromium_plugin_permission_request_settle (fx->request);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
}

static void
test_permission_invalidated (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  ChromiumPluginPermissionRequest *first;
  GError *error = NULL;
  g_autofree char *dir = NULL;
  guint before;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");
  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_park),
                    fx);

  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  first = g_object_ref (fx->request);

  /* The host said no by disabling the plugin: the request is history, and the
   * pens are safe no-ops rather than writes to a table nobody is waiting on. */
  chromium_plugin_manager_disable (fx->manager, "demo");
  chromium_plugin_permission_request_accept (first);
  chromium_plugin_permission_request_deny (first);
  chromium_plugin_permission_request_settle (first);
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 0);
  g_object_unref (first);

  /* Nobody answered the second enable, and then the row was dropped by a
   * rescan: that request is over too, without an event — the row it belonged
   * to is gone. */
  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  before = fx->grants->len;
  dir = g_build_filename (fx->dir, "demo", NULL);
  remove_tree (dir);
  g_assert_true (scanned (fx));
  g_assert_cmpuint (fx->grants->len, ==, before);
  chromium_plugin_permission_request_accept (fx->request);
  chromium_plugin_permission_request_settle (fx->request);
  g_assert_cmpuint (g_list_model_get_n_items (
                      chromium_plugin_manager_get_plugins (fx->manager)),
                    ==, 0);
}

static void
test_permission_disposed_manager (Fixture *fx, gconstpointer data)
{
  GError *error = NULL;
  g_autoptr (ChromiumPluginPermissionRequest) request = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));
  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_park),
                    fx);

  g_assert_false (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&error);
  request = g_object_ref (fx->request);

  /* The manager goes first: the request the host kept is inert, and the verbs
   * must not reach into it. */
  g_clear_object (&fx->manager);
  chromium_plugin_permission_request_accept (request);
  chromium_plugin_permission_request_deny (request);
  chromium_plugin_permission_request_settle (request);
}

static void
test_rescan_keeps_state (Fixture *fx, gconstpointer data)
{
  ChromiumPlugin *plugin;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (scanned (fx));

  g_signal_connect (fx->manager, "grant-request", G_CALLBACK (on_grant_request),
                    fx);
  g_assert_true (chromium_plugin_manager_enable (fx->manager, "demo", &error));
  g_assert_no_error (error);

  /* A rescan is what a host does after an unpack, and it must not silently
     un-do what the person just did. */
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_ENABLED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 2);

  /* The same id with a manifest that now fails keeps nothing: grants belong to a
     package that no longer describes itself. */
  add_plugin (fx, "demo", "{ \"manifest_version\": 3 }");
  g_assert_true (scanned (fx));
  plugin = require_plugin (fx, "demo");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_REFUSED);
  g_assert_cmpuint (strv_length (chromium_plugin_get_granted_permissions (plugin)),
                    ==, 0);

  /* And the plugin a host was holding is not the one in the model any more: the
     row it points at is the object that was replaced. */
  {
    GListModel *model = chromium_plugin_manager_get_plugins (fx->manager);
    g_autoptr (ChromiumPlugin) current = g_list_model_get_item (model, 0);

    g_assert_true (current != NULL);
    g_assert_cmpint (chromium_plugin_get_state (current), ==,
                     CHROMIUM_PLUGIN_STATE_REFUSED);
  }
}

static void
test_duplicate_id (Fixture *fx, gconstpointer data)
{
  /* Sibling tmp directories, not ones inside the first search path: a scan of
     fx->dir would otherwise list them as their own no-manifest refusals and the
     count would be measuring the fixture instead of the collision. */
  g_autofree char *second = g_dir_make_tmp ("cefnest-plugin-second-XXXXXX", NULL);
  g_autofree char *third = g_dir_make_tmp ("cefnest-plugin-third-XXXXXX", NULL);
  ChromiumPlugin *plugin;
  GListModel *model;
  guint n, i, kept = 0, refused = 0;

  g_assert_nonnull (second);
  g_assert_nonnull (third);

  /* Three search paths, one package name. Two are enough to lose the rule, but
   * a three-way tie is the case where a sort with no tie-break can hand back
   * any of the three as the winner — the assertion below is that it is always
   * the first path the host registered, and that the other two stay visible. */
  write_plugin (fx->dir, "demo", kGoodManifest);
  write_plugin (second, "demo", kGoodManifest);
  write_plugin (third, "demo", kGoodManifest);

  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, second,
                                                          NULL));
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, third,
                                                          NULL));
  g_assert_true (scanned (fx));

  model = chromium_plugin_manager_get_plugins (fx->manager);
  n = g_list_model_get_n_items (model);
  g_assert_cmpuint (n, ==, 3);
  g_assert_true (errors_report (fx, "duplicate-id"));

  for (i = 0; i < n; i++)
    {
      g_autoptr (ChromiumPlugin) row = g_list_model_get_item (model, i);

      if (chromium_plugin_get_state (row) == CHROMIUM_PLUGIN_STATE_REFUSED)
        {
          g_assert_cmpstr (chromium_plugin_get_error_message (row), ==,
                           "duplicate-id");
          refused++;
        }
      else
        kept++;
    }

  g_assert_cmpuint (kept, ==, 1);
  g_assert_cmpuint (refused, ==, 2);

  /* lookup() answers with the winner, so the row a host's switch writes on is
   * the one that can be enabled. */
  plugin = require_plugin (fx, "demo");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
  {
    g_autofree char *root = g_build_filename (fx->dir, "demo", NULL);

    g_assert_cmpstr (chromium_plugin_get_root (plugin), ==, root);
  }

  remove_tree (second);
  remove_tree (third);
}

static void
test_unreadable_path (Fixture *fx, gconstpointer data)
{
  g_autofree char *gone = g_build_filename (fx->dir, "unmounted", NULL);
  ChromiumPlugin *plugin;
  GError *error = NULL;

  add_plugin (fx, "demo", kGoodManifest);
  g_mkdir_with_parents (gone, 0755);

  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, gone, NULL));
  g_assert_true (chromium_plugin_manager_add_search_path (fx->manager, fx->dir,
                                                          NULL));

  /* A path that was there when it was added and is not now is a scan failure —
     and the other paths still get scanned, because three directories should not
     go unlisted because a fourth was unplugged. */
  g_rmdir (gone);
  g_assert_false (chromium_plugin_manager_scan (fx->manager, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);

  plugin = require_plugin (fx, "demo");
  g_assert_cmpint (chromium_plugin_get_state (plugin), ==,
                   CHROMIUM_PLUGIN_STATE_SCANNED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/plugin/search-path", test_search_path);
  g_test_add ("/plugin/install-root", Fixture, NULL, fixture_set_up,
              test_install_root, fixture_tear_down);
  g_test_add ("/plugin/scan-model", Fixture, NULL, fixture_set_up,
              test_scan_model, fixture_tear_down);
  g_test_add ("/plugin/refusals", Fixture, NULL, fixture_set_up,
              test_refusals, fixture_tear_down);
  g_test_add ("/plugin/unsupported-fields", Fixture, NULL, fixture_set_up,
              test_unsupported_fields, fixture_tear_down);
  g_test_add ("/plugin/popup-size", Fixture, NULL, fixture_set_up,
              test_popup_size, fixture_tear_down);
  g_test_add ("/plugin/uris", Fixture, NULL, fixture_set_up,
              test_uris, fixture_tear_down);
  g_test_add ("/plugin/grant-machine", Fixture, NULL, fixture_set_up,
              test_grant_machine, fixture_tear_down);
  g_test_add ("/plugin/grant-handler", Fixture, NULL, fixture_set_up,
              test_grant_handler, fixture_tear_down);
  g_test_add ("/plugin/enabled-property", Fixture, NULL, fixture_set_up,
              test_enabled_property, fixture_tear_down);
  g_test_add ("/plugin/permission-parked", Fixture, NULL, fixture_set_up,
              test_permission_parked, fixture_tear_down);
  g_test_add ("/plugin/permission-answers", Fixture, NULL, fixture_set_up,
              test_permission_answers, fixture_tear_down);
  g_test_add ("/plugin/permission-invalidated", Fixture, NULL, fixture_set_up,
              test_permission_invalidated, fixture_tear_down);
  g_test_add ("/plugin/permission-disposed-manager", Fixture, NULL,
              fixture_set_up, test_permission_disposed_manager,
              fixture_tear_down);
  g_test_add ("/plugin/rescan-keeps-state", Fixture, NULL, fixture_set_up,
              test_rescan_keeps_state, fixture_tear_down);
  g_test_add ("/plugin/duplicate-id", Fixture, NULL, fixture_set_up,
              test_duplicate_id, fixture_tear_down);
  g_test_add ("/plugin/unreadable-path", Fixture, NULL, fixture_set_up,
              test_unreadable_path, fixture_tear_down);

  return g_test_run ();
}
