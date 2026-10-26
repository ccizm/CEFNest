#define G_LOG_DOMAIN "chromium-plugin"

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "chromium-plugin.h"
#include "chromium-plugin-private.h"
#include "chromium-plugin-zip.h"

/* A manifest is a small text file. scan() reads whatever sits in a directory the
 * host agreed to look at, so the size has to be bounded before the bytes are,
 * and nobody needs a 1 MiB manifest. */
#define MAX_MANIFEST_BYTES (1u << 20)

G_DEFINE_ENUM_TYPE (ChromiumPluginState, chromium_plugin_state,
  G_DEFINE_ENUM_VALUE (CHROMIUM_PLUGIN_STATE_REFUSED, "refused"),
  G_DEFINE_ENUM_VALUE (CHROMIUM_PLUGIN_STATE_SCANNED, "scanned"),
  G_DEFINE_ENUM_VALUE (CHROMIUM_PLUGIN_STATE_ENABLED, "enabled"))

enum {
  PROP_0,
  PROP_ID,
  PROP_NAME,
  PROP_VERSION,
  PROP_DESCRIPTION,
  PROP_ROOT,
  PROP_KIND,
  PROP_STATE,
  PROP_ENABLED,
  PROP_PERMISSIONS,
  PROP_GRANTED_PERMISSIONS,
  PROP_ERROR_MESSAGE,
  PROP_POPUP_URI,
  PROP_OPTIONS_URI,
  PROP_POPUP_WIDTH,
  PROP_POPUP_HEIGHT,
  PROP_OPTIONS_OPEN_IN_TAB,
  N_PROPERTIES
};

static GParamSpec *plugin_properties[N_PROPERTIES];

struct _ChromiumPlugin {
  GObject parent_instance;

  char *id;
  char *name;
  char *version;
  char *description;
  char *root;
  char *kind;
  char *error_message;
  /* The manifest's cefnest.library and cefnest.symbol. The library is
   * validated against the same inside-the-root rule as every other file a
   * manifest names; only the native frontend ever reads either. */
  char *library;
  char *symbol;

  ChromiumPluginState state;

  /* Which add_search_path() entry this row came from. Not a property: it is the
   * tie-break that makes "the first search path wins" a rule rather than a
   * coin flip, since qsort gives no order at all for two equal ids. */
  guint source_path;

  /* The manifest's own list and the subset the host has granted. Both are
   * %NULL-terminated and never %NULL: an empty list is still an answer. */
  char **permissions;
  char **granted;

  char *popup_uri;
  char *options_uri;
  /* The manifest's background.service_worker path, relative to the root and
   * passed the same inside-the-root rule. The resident page that runs it is
   * the session's business (see chromium_session_new_background_browser());
   * this field is only the manifest's say-so. */
  char *background_page;
  char *background_uri;
  int popup_width;
  int popup_height;
  gboolean options_open_in_tab;

  /* Kept only for the icons map, which is the one manifest member read by a key
   * the host picks at draw time. */
  JsonObject *manifest;

  /* The store owns the plugin and the plugin needs its manager to enable itself
   * through a property write, so this side of the pair is weak. */
  GWeakRef manager;
};

G_DEFINE_TYPE (ChromiumPlugin, chromium_plugin, G_TYPE_OBJECT)

static void chromium_plugin_dispose      (GObject *object);
static void chromium_plugin_get_property (GObject *object, guint property_id,
                                          GValue *value, GParamSpec *pspec);
static void chromium_plugin_set_property (GObject *object, guint property_id,
                                          const GValue *value, GParamSpec *pspec);
static void plugin_set_error             (ChromiumPlugin *self,
                                          const char *token);
static void permission_request_invalidate (ChromiumPluginPermissionRequest *self);

/* ------------------------------------------------------------------ *
 * Paths                                                              *
 *                                                                    *
 * One rule for every manifest member that names a file: relative to   *
 * the plugin's own directory, with no way out of it. The helper       *
 * re-checks the same thing against realpath on every request, because *
 * a URL is normalised before anything sees it and "..%2F" is not      *
 * (measured 2026-10-25; see docs/plugins.md). This side is the        *
 * earlier of the two chances, not a substitute for the other.         *
 * ------------------------------------------------------------------ */

static gboolean
relative_path_is_inside (const char *path)
{
  const char *p;

  if (!path || !*path)
    return FALSE;

  /* Absolute names a file outside the plugin by construction; a backslash is a
   * separator on a platform this does not target and an escape attempt on the
   * one it does; a percent sign means the string is already URI-encoded, which
   * is the form that survives normalisation on its way to a ".." — and a caller
   * here has a filesystem-relative path to hand over, not a half-built URI. */
  if (path[0] == '/' || strchr (path, '%') || strchr (path, '\\'))
    return FALSE;

  for (p = path; *p; )
    {
      const char *end = strchr (p, '/');
      gsize length;

      if (!end)
        end = p + strlen (p);
      length = (gsize) (end - p);

      /* Empty, "." and ".." are the segment forms that put the path somewhere
       * other than where it was written. */
      if (length == 0 ||
          (length == 1 && p[0] == '.') ||
          (length == 2 && p[0] == '.' && p[1] == '.'))
        return FALSE;

      if (!*end)
        break;
      p = end + 1;
    }

  return TRUE;
}

char *
chromium_plugin_build_uri (ChromiumPlugin *self, const char *relative_path,
                           GError **error)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);

  if (!relative_path_is_inside (relative_path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "\"%s\" is not a path inside the plugin directory",
                   relative_path ? relative_path : "(null)");
      return NULL;
    }

  return g_strconcat (CHROMIUM_EXTENSION_URI_PREFIX, self->id, "/",
                      relative_path, NULL);
}

/* ------------------------------------------------------------------ *
 * Manifest reading                                                   *
 *                                                                    *
 * Refused is a state, not an absence: a package whose manifest fails  *
 * stays in the model with the reason on it. The person who unpacked    *
 * it is looking at a list where it is either missing or present and    *
 * doing nothing, and only one of those two can be explained.          *
 * ------------------------------------------------------------------ */

/* Why a manifest was refused, in the words ::plugin-error uses. The vocabulary
 * is the one documented with the manifest rules in
 * docs/plugins.md ("The manifest"). */
#define REFUSE_NO_MANIFEST      "no-manifest"
#define REFUSE_BAD_ID           "bad-id"
#define REFUSE_BAD_MANIFEST     "bad-manifest"
#define REFUSE_UNSUPPORTED_TYPE "unsupported-type"
#define REFUSE_DUPLICATE_ID     "duplicate-id"
#define NOTE_SIZE_CLAMPED       "popup-size-clamped"

typedef struct {
  char *id;
  char *message;
} PluginReport;

static void
report_free (gpointer data)
{
  PluginReport *report = data;

  g_clear_pointer (&report->id, g_free);
  g_clear_pointer (&report->message, g_free);
  g_free (report);
}

static void
report_add (GPtrArray *reports, const char *id, const char *message)
{
  PluginReport *report = g_new0 (PluginReport, 1);

  report->id = g_strdup (id);
  report->message = g_strdup (message);
  g_ptr_array_add (reports, report);
}

static void
report_named (GPtrArray *reports, const char *id, const char *prefix,
              const char *field)
{
  g_autofree char *message = g_strdup_printf ("%s%s", prefix, field);

  report_add (reports, id, message);
}

typedef enum {
  MEMBER_ABSENT,
  MEMBER_READ,
  MEMBER_BAD
} MemberRead;

/* json-glib's node types say "value" for every scalar, so the fundamental type
 * has to be asked for separately — and asking json_node_get_int() for a string
 * or a string for a number is a critical, not a NULL, which is exactly what the
 * test suite runs with G_DEBUG=fatal-criticals. */
static gboolean
node_is_object (JsonNode *node)
{
  return node != NULL && JSON_NODE_HOLDS_OBJECT (node);
}

static gboolean
node_is_array (JsonNode *node)
{
  return node != NULL && JSON_NODE_HOLDS_ARRAY (node);
}

static gboolean
node_is_value_of (JsonNode *node, GType type)
{
  return node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
         json_node_get_value_type (node) == type;
}

static gboolean
node_is_string (JsonNode *node)
{
  return node_is_value_of (node, G_TYPE_STRING);
}

static gboolean
node_is_boolean (JsonNode *node)
{
  return node_is_value_of (node, G_TYPE_BOOLEAN);
}

/* A parser writes a small integer as G_TYPE_INT, a large one as G_TYPE_INT64 and
 * anything with a fraction as G_TYPE_DOUBLE; a manifest's width is all three of
 * those possibilities. */
static gboolean
node_is_number (JsonNode *node)
{
  GType type;

  if (!node || !JSON_NODE_HOLDS_VALUE (node))
    return FALSE;

  type = json_node_get_value_type (node);
  return type == G_TYPE_INT || type == G_TYPE_INT64 || type == G_TYPE_DOUBLE;
}

static gint64
node_number (JsonNode *node)
{
  if (node_is_value_of (node, G_TYPE_DOUBLE))
    return (gint64) json_node_get_double (node);

  return json_node_get_int (node);
}

/* Three outcomes, because "the plugin did not ask" and "the plugin asked in a
 * shape that cannot be honoured" must not be the same answer: the first is
 * normal, the second refuses the package. A known field silently ignored is how
 * an extension ends up doing nothing for reasons nobody can name. */
static MemberRead
read_string (JsonObject *object, const char *member, const char **out)
{
  JsonNode *node;

  *out = NULL;
  if (!object || !json_object_has_member (object, member))
    return MEMBER_ABSENT;

  node = json_object_get_member (object, member);
  if (!node_is_string (node))
    return MEMBER_BAD;

  *out = json_node_get_string (node);
  return MEMBER_READ;
}

static MemberRead
read_object (JsonObject *object, const char *member, JsonObject **out)
{
  JsonNode *node;

  *out = NULL;
  if (!object || !json_object_has_member (object, member))
    return MEMBER_ABSENT;

  node = json_object_get_member (object, member);
  if (!node_is_object (node))
    return MEMBER_BAD;

  *out = json_node_get_object (node);
  return MEMBER_READ;
}

static MemberRead
read_array (JsonObject *object, const char *member, JsonArray **out)
{
  JsonNode *node;

  *out = NULL;
  if (!object || !json_object_has_member (object, member))
    return MEMBER_ABSENT;

  node = json_object_get_member (object, member);
  if (!node_is_array (node))
    return MEMBER_BAD;

  *out = json_node_get_array (node);
  return MEMBER_READ;
}

/* A member naming a file inside the plugin. @id turns it into the
 * cefnest-extension:// form; without one the relative path is what comes back. */
static MemberRead
read_path (JsonObject *object, const char *member, const char *id, char **out)
{
  const char *relative = NULL;
  MemberRead read = read_string (object, member, &relative);

  *out = NULL;
  if (read != MEMBER_READ)
    return read;

  if (!relative_path_is_inside (relative))
    return MEMBER_BAD;

  *out = id ? g_strconcat (CHROMIUM_EXTENSION_URI_PREFIX, id, "/", relative, NULL)
            : g_strdup (relative);
  return MEMBER_READ;
}

static char **
strings_from_array (JsonArray *array, gboolean *malformed)
{
  guint n = json_array_get_length (array);
  char **strings = g_new0 (char *, n + 1);
  guint i;

  *malformed = FALSE;

  for (i = 0; i < n; i++)
    {
      JsonNode *element = json_array_get_element (array, i);

      if (!node_is_string (element))
        {
          g_strfreev (strings);
          *malformed = TRUE;
          return g_new0 (char *, 1);
        }
      strings[i] = g_strdup (json_node_get_string (element));
    }

  return strings;
}

/* The MV3 keys this component will never act on. Naming them is the difference
 * between "this extension has a service worker and nothing here runs one" and a
 * plugin that silently does nothing. background is not on the list: since the
 * resident-page shape landed, background.service_worker is the one MV3 field
 * this component acts on (a hidden browser that runs the named script), and a
 * manifest carrying it only says so through _chromium_plugin_get_background_page(). */
static const char *const kUnsupportedFields[] = {
  "chrome_url_overrides",
  "omnibox",
  "commands",
  "devtools_page",
  "user_scripts",
};

static void
report_unsupported (GPtrArray *reports, const char *id, JsonObject *manifest)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (kUnsupportedFields); i++)
    {
      const char *field = kUnsupportedFields[i];

      if (json_object_has_member (manifest, field))
        report_named (reports, id, "unsupported-field:", field);
    }
}

static gboolean
id_is_usable (const char *id)
{
  gsize n = 0;

  if (!id)
    return FALSE;

  for (; *id; id++, n++)
    {
      char c = *id;

      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-'))
        return FALSE;
    }

  return n > 0 && n <= 64;
}

static ChromiumPlugin *
refused (ChromiumPlugin *plugin, GPtrArray *reports, const char *token)
{
  plugin_set_error (plugin, token);
  report_add (reports, plugin->id, token);
  return plugin;
}

/* Read one candidate directory. Never returns %NULL: whatever it finds, or fails
 * to find, is a plugin object whose state says so. */
static ChromiumPlugin *
read_plugin (const char *dir, GPtrArray *reports)
{
  ChromiumPlugin *plugin = g_object_new (CHROMIUM_TYPE_PLUGIN, NULL);
  g_autofree char *manifest_path = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) error = NULL;
  GStatBuf stats;
  JsonNode *root;
  JsonObject *manifest;
  JsonObject *cefnest = NULL;
  JsonObject *section = NULL;
  JsonArray *array = NULL;
  const char *text;
  gboolean malformed = FALSE;
  guint i;

  plugin->id = g_path_get_basename (dir);
  plugin->root = g_strdup (dir);

  /* The name is the host part of every URI this plugin could be handed, so a
   * directory that fails it could not be served at all — but it still gets a
   * row, because "the folder is there and the list says nothing about it" is the
   * confusing version of this. */
  if (!id_is_usable (plugin->id))
    {
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_BAD_ID);
    }

  manifest_path = g_build_filename (dir, CHROMIUM_PLUGIN_MANIFEST_NAME, NULL);

  if (g_stat (manifest_path, &stats) != 0)
    {
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_NO_MANIFEST);
    }

  if ((gsize) stats.st_size > MAX_MANIFEST_BYTES ||
      !g_file_get_contents (manifest_path, &contents, NULL, &error))
    {
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, -1, &error))
    {
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    }

  root = json_parser_get_root (parser);
  if (!node_is_object (root))
    {
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    }

  /* Refcounted out past the parser: the icons map is read by a key the host
   * picks at draw time, and walking an object's members would mean writing this
   * against a json-glib call whose shape changed in 1.8. */
  plugin->manifest = json_object_ref (json_node_get_object (root));
  manifest = plugin->manifest;

  report_unsupported (reports, plugin->id, manifest);

  /* Three members make a package something this can name, version and show. */
  {
    JsonNode *node = json_object_get_member (manifest, "manifest_version");

    if (!node)
      {
        plugin->name = g_strdup (plugin->id);
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);
      }
    if (node_is_number (node) && node_number (node) == 3)
      ;
    else if (node_is_string (node) &&
             g_strcmp0 (json_node_get_string (node), "3") == 0)
      ;
    else
      {
        plugin->name = g_strdup (plugin->id);
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);
      }
  }

  plugin->name = NULL;
  if (read_string (manifest, "name", &text) == MEMBER_READ && text && *text)
    plugin->name = g_strdup (text);
  if (read_string (manifest, "version", &text) == MEMBER_READ && text && *text)
    plugin->version = g_strdup (text);

  if (!plugin->name || !plugin->version)
    {
      g_clear_pointer (&plugin->name, g_free);
      plugin->name = g_strdup (plugin->id);
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    }

  {
    MemberRead read = read_string (manifest, "description", &text);

    if (read == MEMBER_BAD)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    if (read == MEMBER_READ)
      plugin->description = g_strdup (text);
  }

  /* Which frontend the package wants — and only one of them. */
  if (read_object (manifest, "cefnest", &cefnest) != MEMBER_READ ||
      read_string (cefnest, "type", &text) != MEMBER_READ)
    return refused (plugin, reports, REFUSE_BAD_MANIFEST);

  if (g_strcmp0 (text, "web") == 0 || g_strcmp0 (text, "native") == 0)
    plugin->kind = g_strdup (text);
  else
    /* A type this component has never heard of is not the same failure as a
     * broken manifest: it is a package written for something else, and the row
     * should be able to say which. */
    return refused (plugin, reports, REFUSE_UNSUPPORTED_TYPE);

  {
    g_autofree char *library = NULL;
    MemberRead read = read_path (cefnest, "library", NULL, &library);

    if (read == MEMBER_BAD)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    if (g_strcmp0 (plugin->kind, "native") == 0 && read != MEMBER_READ)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    if (g_strcmp0 (plugin->kind, "web") == 0 && read == MEMBER_READ)
      /* A web package naming a library has two answers to "what runs", and the
         permission table cannot be interpreted against both. */
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    if (read == MEMBER_READ)
      plugin->library = g_steal_pointer (&library);

    /* The entry point a native package exports. Optional, because every
       plugin this version loads uses the documented one; a non-string is the
       same kind of broken shape as a bad library path. */
    read = read_string (cefnest, "symbol", &text);
    if (read == MEMBER_BAD)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    plugin->symbol = g_strdup (read == MEMBER_READ ? text
                                                   : "cefnest_plugin_register");
  }

  {
    MemberRead read = read_array (manifest, "permissions", &array);

    if (read == MEMBER_BAD)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);
    if (read == MEMBER_READ)
      {
        g_strfreev (plugin->permissions);
        plugin->permissions = strings_from_array (array, &malformed);
        if (malformed)
          return refused (plugin, reports, REFUSE_BAD_MANIFEST);
      }
  }

  /* Content scripts are checked for shape only — which page they land on is the
   * web frontend's business and is P2. A script the manifest points at but that
   * cannot be reached is a package that cannot be run, so it refuses here. */
  {
    MemberRead read = read_array (manifest, "content_scripts", &array);

    if (read == MEMBER_BAD)
      return refused (plugin, reports, REFUSE_BAD_MANIFEST);

    for (i = 0; read == MEMBER_READ && i < json_array_get_length (array); i++)
      {
        JsonNode *entry = json_array_get_element (array, i);
        JsonObject *script = NULL;
        JsonArray *files = NULL;
        const char *run_at = NULL;
        guint j;

        /* A native package may ship content scripts too — the plan's own
           example plugin is one .so plus a cs.js. The Web frontend injects
           them; the native half runs beside it. */
        if (!node_is_object (entry))
          return refused (plugin, reports, REFUSE_BAD_MANIFEST);
        script = json_node_get_object (entry);

        if (read_array (script, "js", &files) == MEMBER_BAD ||
            read_string (script, "run_at", &run_at) == MEMBER_BAD)
          return refused (plugin, reports, REFUSE_BAD_MANIFEST);

        if (run_at && g_strcmp0 (run_at, "document_start") != 0 &&
            g_strcmp0 (run_at, "document_idle") != 0 &&
            g_strcmp0 (run_at, "document_end") != 0)
          return refused (plugin, reports, REFUSE_BAD_MANIFEST);

        for (j = 0; files && j < json_array_get_length (files); j++)
          {
            JsonNode *element = json_array_get_element (files, j);

            if (!node_is_string (element) ||
                !relative_path_is_inside (json_node_get_string (element)))
              return refused (plugin, reports, REFUSE_BAD_MANIFEST);
          }
      }
  }

  /* The two extension pages, both optional: a package with neither is a plugin
   * that only ever runs inside the host's own windows. */
  if (read_object (manifest, "action", &section) == MEMBER_READ &&
      read_path (section, "default_popup", plugin->id, &plugin->popup_uri) ==
      MEMBER_BAD)
    return refused (plugin, reports, REFUSE_BAD_MANIFEST);

  if (read_object (manifest, "options_ui", &section) == MEMBER_READ)
    {
      JsonNode *node;

      if (read_path (section, "page", plugin->id, &plugin->options_uri) ==
          MEMBER_BAD)
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);

      node = json_object_get_member (section, "open_in_tab");
      if (node && !node_is_boolean (node))
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);
      if (node)
        plugin->options_open_in_tab = json_node_get_boolean (node);
    }
  else if (read_path (manifest, "options_page", plugin->id,
                      &plugin->options_uri) == MEMBER_BAD)
    return refused (plugin, reports, REFUSE_BAD_MANIFEST);

  /* The resident background page, the one MV3 field this component acts on:
   * background.service_worker names a script, the session runs it in a hidden
   * browser for as long as the plugin is enabled. A background of any other
   * shape (MV2 page/scripts, a non-object) is back to being named as
   * unsupported — refused manifests are for shapes that cannot run at all,
   * and a plugin without a background can still run. */
  if (read_object (manifest, "background", &section) == MEMBER_BAD)
    return refused (plugin, reports, REFUSE_BAD_MANIFEST);
  if (section)
    {
      MemberRead read = read_string (section, "service_worker", &text);

      if (read == MEMBER_BAD)
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);
      if (read == MEMBER_READ)
        {
          if (!relative_path_is_inside (text) || !*text)
            return refused (plugin, reports, REFUSE_BAD_MANIFEST);
          plugin->background_page = g_strdup (text);
          plugin->background_uri =
            g_strconcat (CHROMIUM_EXTENSION_URI_PREFIX, plugin->id,
                         "/__cefnest_background__.html", NULL);
        }
      else
        report_named (reports, plugin->id, "unsupported-field:", "background");
    }

  /* The popup box: the manifest's numbers when it gave any, the defaults when it
   * gave none, never past the ceiling — and the clamp reported rather than
   * swallowed, because a page that asked for 4000x4000 wanted something that is
   * not what it is going to get. */
  if (plugin->popup_uri &&
      read_object (cefnest, "popup", &section) == MEMBER_READ && section)
    {
      JsonNode *node;

      node = json_object_get_member (section, "width");
      if (node_is_number (node))
        plugin->popup_width = (int) node_number (node);
      node = json_object_get_member (section, "height");
      if (node_is_number (node))
        plugin->popup_height = (int) node_number (node);

      if (plugin->popup_width < 1 || plugin->popup_height < 1)
        return refused (plugin, reports, REFUSE_BAD_MANIFEST);

      if (plugin->popup_width > CHROMIUM_PLUGIN_POPUP_MAX_WIDTH)
        {
          report_add (reports, plugin->id, NOTE_SIZE_CLAMPED);
          plugin->popup_width = CHROMIUM_PLUGIN_POPUP_MAX_WIDTH;
        }
      if (plugin->popup_height > CHROMIUM_PLUGIN_POPUP_MAX_HEIGHT)
        {
          report_add (reports, plugin->id, NOTE_SIZE_CLAMPED);
          plugin->popup_height = CHROMIUM_PLUGIN_POPUP_MAX_HEIGHT;
        }
    }

  return plugin;
}

/* ------------------------------------------------------------------ *
 * The plugin object                                                  *
 * ------------------------------------------------------------------ */

static void
chromium_plugin_class_init (ChromiumPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = chromium_plugin_dispose;
  object_class->get_property = chromium_plugin_get_property;
  object_class->set_property = chromium_plugin_set_property;

  /**
   * ChromiumPlugin:id:
   *
   * The directory name scan() found the manifest under, which is also the host
   * part of every URI this plugin gets. chromium_plugin_manager_scan() says what
   * makes one unusable.
   */
  plugin_properties[PROP_ID] =
    g_param_spec_string ("id", "Id", "The plugin identifier",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:name:
   *
   * The manifest's name, or the id for a package that never got far enough to
   * have one. This is what a list row shows.
   */
  plugin_properties[PROP_NAME] =
    g_param_spec_string ("name", "Name", "The display name from the manifest",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_VERSION] =
    g_param_spec_string ("version", "Version", "The manifest version string",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_DESCRIPTION] =
    g_param_spec_string ("description", "Description",
                         "What the plugin says it does",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_ROOT] =
    g_param_spec_string ("root", "Root",
                         "The directory the manifest was read from",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:kind:
   *
   * "web" or "native": which frontend the package asks for, and the only reason a
   * management page needs to know the two cannot be mixed. %NULL for a refused
   * package whose type could not be read.
   */
  plugin_properties[PROP_KIND] =
    g_param_spec_string ("kind", "Kind",
                         "The frontend the plugin runs on: web or native",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:state:
   *
   * #CHROMIUM_PLUGIN_STATE_REFUSED, #CHROMIUM_PLUGIN_STATE_SCANNED or
   * #CHROMIUM_PLUGIN_STATE_ENABLED. A refused plugin cannot leave that state:
   * the manifest is what was checked, and writing to the object does not re-read
   * it — only a scan does, and only the same directory can.
   */
  plugin_properties[PROP_STATE] =
    g_param_spec_enum ("state", "State", "What the model knows about this plugin",
                       CHROMIUM_TYPE_PLUGIN_STATE,
                       CHROMIUM_PLUGIN_STATE_SCANNED,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:enabled:
   *
   * The writable half of #ChromiumPlugin:state, for a switch bound straight into
   * a list row: writing it runs chromium_plugin_manager_enable() on the manager
   * that owns the plugin, grant request and all. It reads back whatever that
   * machine settled on, which is not necessarily what was written, so a two-way
   * binding has to be prepared for the switch to come back off.
   */
  plugin_properties[PROP_ENABLED] =
    g_param_spec_boolean ("enabled", "Enabled",
                          "Whether the plugin is enabled",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                          G_PARAM_EXPLICIT_NOTIFY);
  plugin_properties[PROP_PERMISSIONS] =
    g_param_spec_boxed ("permissions", "Permissions",
                        "The permissions the manifest declares",
                        G_TYPE_STRV, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_GRANTED_PERMISSIONS] =
    g_param_spec_boxed ("granted-permissions", "Granted permissions",
                        "The permissions the host has granted",
                        G_TYPE_STRV, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:error-message:
   *
   * The machine-readable reason the manifest was refused, or %NULL. It is the
   * same string ::plugin-error carried, and it is meant for a row's subtitle:
   * somebody who reads "unsupported-field:background.service_worker" next to a
   * row that does nothing knows their extension needs a background page this
   * component does not have, which is more than "it failed" tells them.
   */
  plugin_properties[PROP_ERROR_MESSAGE] =
    g_param_spec_string ("error-message", "Error message",
                         "Why the manifest was refused",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /**
   * ChromiumPlugin:popup-uri:
   *
   * The URI of the page the manifest names as action.default_popup, or %NULL for
   * a plugin with no popup. It is a document to load and nothing more: which
   * window or popover shows it, when, and who closes it is the host's decision —
   * see docs/plans/work-plan-2026-10-26.md 第三部分 5.
   */
  plugin_properties[PROP_POPUP_URI] =
    g_param_spec_string ("popup-uri", "Popup URI",
                         "cefnest-extension:// URI of the popup page, or NULL",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_OPTIONS_URI] =
    g_param_spec_string ("options-uri", "Options URI",
                         "cefnest-extension:// URI of the options page, or NULL",
                         NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_POPUP_WIDTH] =
    g_param_spec_int ("popup-width", "Popup width",
                      "Width the popup page asks for, already clamped",
                      1, CHROMIUM_PLUGIN_POPUP_MAX_WIDTH,
                      CHROMIUM_PLUGIN_POPUP_DEFAULT_WIDTH,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_POPUP_HEIGHT] =
    g_param_spec_int ("popup-height", "Popup height",
                      "Height the popup page asks for, already clamped",
                      1, CHROMIUM_PLUGIN_POPUP_MAX_HEIGHT,
                      CHROMIUM_PLUGIN_POPUP_DEFAULT_HEIGHT,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  plugin_properties[PROP_OPTIONS_OPEN_IN_TAB] =
    g_param_spec_boolean ("options-open-in-tab", "Options open in a tab",
                          "What the manifest asked with options_ui.open_in_tab",
                          FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES,
                                     plugin_properties);
}

static void
chromium_plugin_init (ChromiumPlugin *self)
{
  self->state = CHROMIUM_PLUGIN_STATE_SCANNED;
  self->permissions = g_new0 (char *, 1);
  self->granted = g_new0 (char *, 1);
  self->popup_width = CHROMIUM_PLUGIN_POPUP_DEFAULT_WIDTH;
  self->popup_height = CHROMIUM_PLUGIN_POPUP_DEFAULT_HEIGHT;
  g_weak_ref_init (&self->manager, NULL);
}

static void
chromium_plugin_dispose (GObject *object)
{
  ChromiumPlugin *self = CHROMIUM_PLUGIN (object);

  g_weak_ref_clear (&self->manager);

  if (self->manifest)
    {
      json_object_unref (self->manifest);
      self->manifest = NULL;
    }

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->version, g_free);
  g_clear_pointer (&self->description, g_free);
  g_clear_pointer (&self->root, g_free);
  g_clear_pointer (&self->kind, g_free);
  g_clear_pointer (&self->error_message, g_free);
  g_clear_pointer (&self->library, g_free);
  g_clear_pointer (&self->symbol, g_free);
  g_clear_pointer (&self->popup_uri, g_free);
  g_clear_pointer (&self->options_uri, g_free);
  g_clear_pointer (&self->background_page, g_free);
  g_clear_pointer (&self->background_uri, g_free);
  g_clear_pointer (&self->permissions, g_strfreev);
  g_clear_pointer (&self->granted, g_strfreev);

  G_OBJECT_CLASS (chromium_plugin_parent_class)->dispose (object);
}

static void
chromium_plugin_get_property (GObject *object, guint property_id, GValue *value,
                              GParamSpec *pspec)
{
  ChromiumPlugin *self = CHROMIUM_PLUGIN (object);

  switch (property_id)
    {
    case PROP_ID:
      g_value_set_string (value, self->id);
      break;
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case PROP_VERSION:
      g_value_set_string (value, self->version);
      break;
    case PROP_DESCRIPTION:
      g_value_set_string (value, self->description);
      break;
    case PROP_ROOT:
      g_value_set_string (value, self->root);
      break;
    case PROP_KIND:
      g_value_set_string (value, self->kind);
      break;
    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;
    case PROP_ENABLED:
      g_value_set_boolean (value, self->state == CHROMIUM_PLUGIN_STATE_ENABLED);
      break;
    case PROP_PERMISSIONS:
      g_value_take_boxed (value, g_strdupv (self->permissions));
      break;
    case PROP_GRANTED_PERMISSIONS:
      g_value_take_boxed (value, g_strdupv (self->granted));
      break;
    case PROP_ERROR_MESSAGE:
      g_value_set_string (value, self->error_message);
      break;
    case PROP_POPUP_URI:
      g_value_set_string (value, self->popup_uri);
      break;
    case PROP_OPTIONS_URI:
      g_value_set_string (value, self->options_uri);
      break;
    case PROP_POPUP_WIDTH:
      g_value_set_int (value, self->popup_width);
      break;
    case PROP_POPUP_HEIGHT:
      g_value_set_int (value, self->popup_height);
      break;
    case PROP_OPTIONS_OPEN_IN_TAB:
      g_value_set_boolean (value, self->options_open_in_tab);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
chromium_plugin_set_property (GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec)
{
  ChromiumPlugin *self = CHROMIUM_PLUGIN (object);

  switch (property_id)
    {
    case PROP_ENABLED:
      chromium_plugin_set_enabled (self, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
plugin_set_error (ChromiumPlugin *self, const char *token)
{
  g_clear_pointer (&self->error_message, g_free);
  self->error_message = g_strdup (token);
  self->state = CHROMIUM_PLUGIN_STATE_REFUSED;

  /* A refused package gets no pages either: the URI is derived from manifest
   * members that were not checked on the way to the refusal, and handing a host
   * a loadable address out of a manifest this component just rejected is the one
   * thing a refusal should never leave open. */
  g_clear_pointer (&self->popup_uri, g_free);
  g_clear_pointer (&self->options_uri, g_free);
}

static void
plugin_set_state (ChromiumPlugin *self, ChromiumPluginState state)
{
  self->state = state;
}

/* The two properties that read the same field travel together, and a host
 * binding a switch to one has to be told when the other moved. */
static void
plugin_notify_state (ChromiumPlugin *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), plugin_properties[PROP_STATE]);
  g_object_notify_by_pspec (G_OBJECT (self), plugin_properties[PROP_ENABLED]);
}

const char *
chromium_plugin_get_id (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->id;
}

const char *
chromium_plugin_get_name (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->name;
}

const char *
chromium_plugin_get_version (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->version;
}

const char *
chromium_plugin_get_description (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->description;
}

const char *
chromium_plugin_get_root (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->root;
}

const char *
chromium_plugin_get_kind (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->kind;
}

ChromiumPluginState
chromium_plugin_get_state (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), CHROMIUM_PLUGIN_STATE_REFUSED);
  return self->state;
}

const char *
chromium_plugin_get_error_message (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->error_message;
}

const char *const *
chromium_plugin_get_permissions (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return (const char *const *) self->permissions;
}

const char *const *
chromium_plugin_get_granted_permissions (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return (const char *const *) self->granted;
}

const char *
chromium_plugin_get_popup_uri (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->popup_uri;
}

const char *
chromium_plugin_get_options_uri (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->options_uri;
}

/* The page the session loads into the resident hidden browser: the scheme
   handler's synthesized wrapper around background_page, not the script
   itself — a .js served as a document would sit there as text. NULL for a
   manifest without background.service_worker. */
const char *
chromium_plugin_get_background_uri (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->background_uri;
}

const char *
_chromium_plugin_get_background_page (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->background_page;
}

void
chromium_plugin_get_popup_size (ChromiumPlugin *self, int *width, int *height)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN (self));

  if (width)
    *width = self->popup_width;
  if (height)
    *height = self->popup_height;
}

gboolean
chromium_plugin_get_options_open_in_tab (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), FALSE);
  return self->options_open_in_tab;
}

gboolean
chromium_plugin_get_enabled (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), FALSE);
  return self->state == CHROMIUM_PLUGIN_STATE_ENABLED;
}

char *
chromium_plugin_icon_path (ChromiumPlugin *self, const char *key)
{
  JsonObject *map;
  JsonNode *entry;
  const char *relative;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  g_return_val_if_fail (key != NULL, NULL);

  if (read_object (self->manifest, "icons", &map) != MEMBER_READ ||
      !json_object_has_member (map, key))
    return NULL;

  entry = json_object_get_member (map, key);
  if (!node_is_string (entry))
    return NULL;
  relative = json_node_get_string (entry);

  if (!relative_path_is_inside (relative))
    return NULL;

  return g_build_filename (self->root, relative, NULL);
}

void
chromium_plugin_set_enabled (ChromiumPlugin *self, gboolean enabled)
{
  g_autoptr (GObject) manager = NULL;
  GError *error = NULL;

  g_return_if_fail (CHROMIUM_IS_PLUGIN (self));

  /* A plugin held past its manager has no grant table to consult and nowhere to
   * report what it found, so the write is refused by leaving things as they are
   * rather than by enabling something nobody is keeping track of. */
  manager = g_weak_ref_get (&self->manager);
  if (!manager)
    {
      g_debug ("plugin: \"%s\" has no manager, ignoring enabled=%d",
               self->id, enabled);
      return;
    }

  if (enabled)
    {
      gboolean ok = chromium_plugin_manager_enable (CHROMIUM_PLUGIN_MANAGER (manager),
                                                   self->id, &error);

      if (!ok)
        g_debug ("plugin: enabling \"%s\" did not take: %s", self->id,
                 error ? error->message : "unknown");
      g_clear_error (&error);
    }
  else
    chromium_plugin_manager_disable (CHROMIUM_PLUGIN_MANAGER (manager), self->id);
}

/* ------------------------------------------------------------------ *
 * The manager                                                        *
 * ------------------------------------------------------------------ */

enum {
  SIG_CHANGED,
  SIG_GRANT_REQUEST,
  SIG_PLUGIN_ERROR,
  SIG_PLUGIN_MESSAGE,
  SIG_PLUGIN_ACTION,
  N_SIGNALS
};

static guint manager_signals[N_SIGNALS];

struct _ChromiumPluginManager {
  GObject parent_instance;

  GListStore *store;
  GPtrArray *search_paths;

  /* Where plugins are installed, derived by the session from its data
   * directory (<data-dir>/cefnest-plugins). Optional twice over: a session
   * without a data directory has no install root at all, and a fresh profile
   * has the directory not yet created — both are an empty plugin list, not a
   * scan failure. Scanned before every search path, so the user's own
   * install wins a duplicate id over one the host pointed at. */
  char *install_root;

  /* The session that created this manager, weak: set once, from
   * chromium_session_get_plugin_manager(). The registry verbs send their
   * commands through it, and the session routes helper events back in. */
  ChromiumSession *session;

  /* Parked enables, by plugin id. At most one per plugin: a second enable()
   * while the first is waiting decides from the table instead of asking a
   * second time. The value owns a reference; the request's own weak manager
   * pointer is how it reaches back to settle. */
  GHashTable *permission_requests;
};

G_DEFINE_TYPE (ChromiumPluginManager, chromium_plugin_manager, G_TYPE_OBJECT)

static void chromium_plugin_manager_dispose (GObject *object);

ChromiumPluginManager *
chromium_plugin_manager_new (void)
{
  return g_object_new (CHROMIUM_TYPE_PLUGIN_MANAGER, NULL);
}

GListModel *
chromium_plugin_manager_get_plugins (ChromiumPluginManager *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), NULL);
  return G_LIST_MODEL (self->store);
}

static void
plugin_set_manager (ChromiumPlugin *plugin, ChromiumPluginManager *manager)
{
  g_weak_ref_set (&plugin->manager, manager);
}

ChromiumPlugin *
chromium_plugin_manager_lookup (ChromiumPluginManager *self, const char *id)
{
  guint n, i;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), NULL);

  if (!id)
    return NULL;

  /* Linear, and deliberately: a scan's list is tens of entries, and sorted
   * order is what the model shows rather than what a lookup is keyed on — a
   * duplicate id is allowed to exist here precisely because the second one is
   * refused and needs to stay visible. The first row with the name wins, which
   * is the one scan() kept. */
  n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (i = 0; i < n; i++)
    {
      ChromiumPlugin *plugin = g_list_model_get_item (G_LIST_MODEL (self->store), i);

      if (g_strcmp0 (plugin->id, id) == 0)
        return plugin;

      g_object_unref (plugin);
    }

  return NULL;
}

gboolean
chromium_plugin_manager_add_search_path (ChromiumPluginManager *self,
                                         const char *dir, GError **error)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (dir != NULL, FALSE);

  /* Validated when the host says it rather than several scans later: a relative
   * search path resolves against whatever directory the process happens to be
   * started in, which is a different answer for a launcher, a test and a person
   * double-clicking an icon. */
  if (!g_path_is_absolute (dir))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Plugin search path \"%s\" is not absolute", dir);
      return FALSE;
    }

  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                   "Plugin search path \"%s\" is not a directory", dir);
      return FALSE;
    }

  {
    guint n = self->search_paths->len;
    guint i;

    for (i = 0; i < n; i++)
      if (g_strcmp0 (g_ptr_array_index (self->search_paths, i), dir) == 0)
        return TRUE;
  }

  g_ptr_array_add (self->search_paths, g_strdup (dir));
  return TRUE;
}

static gint
compare_plugins_by_id (gconstpointer a, gconstpointer b)
{
  const ChromiumPlugin *const *pa = (const ChromiumPlugin *const *) a;
  const ChromiumPlugin *const *pb = (const ChromiumPlugin *const *) b;
  gint cmp = g_strcmp0 ((*pa)->id, (*pb)->id);

  if (cmp != 0)
    return cmp;

  /* Ties are the interesting case, and qsort offers no order at all for two
   * equal keys: the duplicate-marking pass refuses whichever row lands second,
   * so without this the search path whose package wins would be decided by the
   * sort rather than by the order the host asked for. */
  if ((*pa)->source_path != (*pb)->source_path)
    return (gint) (*pa)->source_path - (gint) (*pb)->source_path;

  return 0;
}

static gboolean
all_declared_granted (ChromiumPlugin *plugin)
{
  guint i;

  for (i = 0; plugin->permissions[i]; i++)
    if (!g_strv_contains ((const char *const *) plugin->granted,
                          plugin->permissions[i]))
      return FALSE;

  return TRUE;
}

/* The names the manifest declares and the table does not have yet.
 *
 * The strings are borrowed out of plugin->permissions, so the caller frees the
 * container with g_free() and never with g_strfreev(): these are the plugin's own
 * permission names, and freeing one here empties the table every later check
 * reads. Borrowed is also what ::grant-request documents — the host may copy the
 * names, not keep the array. */
static char **
pending_permissions (ChromiumPlugin *plugin)
{
  GPtrArray *pending = g_ptr_array_new ();
  guint i;

  for (i = 0; plugin->permissions[i]; i++)
    if (!g_strv_contains ((const char *const *) plugin->granted,
                          plugin->permissions[i]))
      g_ptr_array_add (pending, plugin->permissions[i]);

  g_ptr_array_add (pending, NULL);
  return (char **) g_ptr_array_free (pending, FALSE);
}
/* What a rescan must not do: a plugin the user already turned on and the host
 * already granted comes back from a scan of the same directory as a brand new
 * object, and losing that would silently un-do both. A manifest that now fails
 * keeps nothing — grants belong to a package that no longer describes itself. */
static void
carry_over (ChromiumPlugin *plugin, ChromiumPlugin *previous)
{
  if (!previous || plugin->state == CHROMIUM_PLUGIN_STATE_REFUSED)
    return;

  g_strfreev (plugin->granted);
  plugin->granted = g_strdupv (previous->granted);

  if (previous->state == CHROMIUM_PLUGIN_STATE_ENABLED && all_declared_granted (plugin))
    plugin->state = CHROMIUM_PLUGIN_STATE_ENABLED;
}

/* One directory's candidates into @found. The first directory that cannot be
   read is the scan's one reported failure — the rest are still scanned,
   because a host with three directories does not want two of them to go
   unlisted because a fourth was unplugged. @optional is the install root's
   allowance in the other direction: a fresh profile has no plugins directory
   yet, and "nothing installed" is the right answer to that, not a failed
   scan. */
static void
scan_plugin_directory (const char *path, guint source_index, gboolean optional,
                       GPtrArray *found, GPtrArray *reports, GError **first)
{
  GDir *dir = NULL;
  GError *error = NULL;
  const char *name;

  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    dir = g_dir_open (path, 0, &error);
  else if (!optional)
    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                 "Plugin search path \"%s\" is not a directory", path);

  if (!dir)
    {
      if (error && !*first)
        *first = error;
      else
        g_clear_error (&error);
      return;
    }

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *candidate = NULL;
      ChromiumPlugin *plugin;

      /* A leading dot is somebody's editor backup or the directory itself;
         neither is a package, and neither was chosen by a plugin author. */
      if (name[0] == '.')
        continue;

      candidate = g_build_filename (path, name, NULL);
      if (!g_file_test (candidate, G_FILE_TEST_IS_DIR))
        continue;

      plugin = read_plugin (candidate, reports);
      plugin->source_path = source_index;
      g_ptr_array_add (found, plugin);
    }

  g_dir_close (dir);
}

gboolean
chromium_plugin_manager_scan (ChromiumPluginManager *self, GError **error)
{
  g_autoptr (GPtrArray) found = NULL;
  g_autoptr (GPtrArray) reports = NULL;
  g_autoptr (GHashTable) previous = NULL;
  GError *first = NULL;
  guint n, i;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);

  found = g_ptr_array_new_with_free_func (g_object_unref);
  reports = g_ptr_array_new_with_free_func (report_free);

  /* The install root first, so it holds source index 0 and wins the
     duplicate-id tie against every host-added path. A host that adds it as
     a search path too gets it scanned once: the loop below skips the same
     name. */
  if (self->install_root)
    scan_plugin_directory (self->install_root, 0, TRUE, found, reports,
                           &first);

  for (i = 0; i < self->search_paths->len; i++)
    {
      const char *path = g_ptr_array_index (self->search_paths, i);

      if (self->install_root && g_strcmp0 (path, self->install_root) == 0)
        continue;

      scan_plugin_directory (path, self->install_root ? i + 1 : i, FALSE,
                             found, reports, &first);
    }

  g_ptr_array_sort (found, compare_plugins_by_id);

  /* Two directories, one id: the first search path wins, and the loser is kept
     as a refused row rather than dropped, because a plugin that vanished between
     two scans is the thing a person will come asking about. */
  for (i = 1; i < found->len; i++)
    {
      ChromiumPlugin *plugin = g_ptr_array_index (found, i);
      ChromiumPlugin *before = g_ptr_array_index (found, i - 1);

      if (g_strcmp0 (plugin->id, before->id) == 0 &&
          plugin->state != CHROMIUM_PLUGIN_STATE_REFUSED)
        refused (plugin, reports, REFUSE_DUPLICATE_ID);
    }

  previous = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                    g_object_unref);
  n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (i = 0; i < n; i++)
    {
      ChromiumPlugin *plugin = g_list_model_get_item (G_LIST_MODEL (self->store), i);

      /* Only the first of a duplicated id is worth carrying state from. */
      if (plugin->id && !g_hash_table_contains (previous, plugin->id))
        g_hash_table_insert (previous, plugin->id, plugin);
      else
        g_object_unref (plugin);
    }

  for (i = 0; i < found->len; i++)
    {
      ChromiumPlugin *plugin = g_ptr_array_index (found, i);

      carry_over (plugin, g_hash_table_lookup (previous, plugin->id));
      plugin_set_manager (plugin, self);
    }

  g_list_store_remove_all (self->store);
  for (i = 0; i < found->len; i++)
    g_list_store_append (self->store, g_ptr_array_index (found, i));

  /* A parked enable survives a rescan only while its row does: the request is
     settled against a plugin looked up by id, and a row the scan dropped has
     no table to settle. The ids are copied because invalidating a request
     removes its key from the table this loop reads. */
  {
    GList *ids = g_hash_table_get_keys (self->permission_requests);
    GList *copy = g_list_copy_deep (ids, (GCopyFunc) g_strdup, NULL);
    GList *l;

    g_list_free (ids);
    for (l = copy; l; l = l->next)
      {
        ChromiumPlugin *plugin = chromium_plugin_manager_lookup (self, l->data);

        if (plugin)
          g_object_unref (plugin);
        else
          permission_request_invalidate (
            g_hash_table_lookup (self->permission_requests, l->data));
      }
    g_list_free_full (copy, g_free);
  }

  /* The list is the answer to "what is installed", so every complaint about it
   * goes out after the rows exist: a host that reacts to ::plugin-error by
   * looking the plugin up has to find it. */
  for (i = 0; i < reports->len; i++)
    {
      PluginReport *report = g_ptr_array_index (reports, i);

      g_signal_emit (self, manager_signals[SIG_PLUGIN_ERROR], 0, report->id,
                     report->message);
    }

  g_signal_emit (self, manager_signals[SIG_CHANGED], 0);

  if (first)
    {
      g_propagate_error (error, first);
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------ *
 * Permission requests                                                *
 * ------------------------------------------------------------------ */

struct _ChromiumPluginPermissionRequest {
  GObject parent_instance;

  GWeakRef manager;
  char *id;
  /* The pending set at the time of the request, owned. The table can grow
   * after this — grant() is the only way an entry becomes true — but the rows
   * the host was asked to draw stay the rows it was asked about. */
  char **permissions;
  /* Set by a settle, by disable(), by a scan that dropped the row, and by the
   * manager's dispose. A settled request is history: every verb is a no-op. */
  gboolean settled;
};

G_DEFINE_TYPE (ChromiumPluginPermissionRequest,
               chromium_plugin_permission_request, G_TYPE_OBJECT)

static void
chromium_plugin_permission_request_init (ChromiumPluginPermissionRequest *self)
{
  g_weak_ref_init (&self->manager, NULL);
}

static void
chromium_plugin_permission_request_finalize (GObject *object)
{
  ChromiumPluginPermissionRequest *self =
    CHROMIUM_PLUGIN_PERMISSION_REQUEST (object);

  g_weak_ref_clear (&self->manager);
  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->permissions, g_strfreev);
  G_OBJECT_CLASS (chromium_plugin_permission_request_parent_class)->finalize (object);
}

static void
chromium_plugin_permission_request_class_init (ChromiumPluginPermissionRequestClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = chromium_plugin_permission_request_finalize;
}

static ChromiumPluginPermissionRequest *
permission_request_new (ChromiumPluginManager *manager, const char *id,
                        char **pending)
{
  ChromiumPluginPermissionRequest *self =
    g_object_new (CHROMIUM_TYPE_PLUGIN_PERMISSION_REQUEST, NULL);

  g_weak_ref_set (&self->manager, manager);
  self->id = g_strdup (id);
  self->permissions = g_strdupv (pending);
  return self;
}

/* Takes the request out of the manager's table and marks it settled. The
 * caller must hold a reference: removing the table's may be the last one. */
static void
permission_request_invalidate (ChromiumPluginPermissionRequest *self)
{
  ChromiumPluginManager *manager;

  if (self->settled)
    return;
  self->settled = TRUE;
  manager = g_weak_ref_get (&self->manager);
  if (manager)
    {
      g_hash_table_remove (manager->permission_requests, self->id);
      g_object_unref (manager);
    }
}

typedef enum {
  PERMISSION_SETTLE_ACCEPT,   /* grant the pending set, then enable */
  PERMISSION_SETTLE_DENY,     /* the table stays as it is; no enable */
  PERMISSION_SETTLE_TABLE     /* decide from the table; no grants */
} PermissionSettleMode;

static void
permission_request_settle (ChromiumPluginPermissionRequest *self,
                           PermissionSettleMode mode)
{
  g_autoptr (ChromiumPluginPermissionRequest) hold = g_object_ref (self);
  ChromiumPluginManager *manager;
  ChromiumPlugin *plugin;
  guint i;

  if (self->settled)
    return;
  manager = g_weak_ref_get (&self->manager);
  if (!manager)
    {
      /* The manager went first: there is no table to settle against and no
         listener to tell. The request is simply over. */
      self->settled = TRUE;
      return;
    }

  if (mode == PERMISSION_SETTLE_ACCEPT)
    for (i = 0; self->permissions && self->permissions[i]; i++)
      chromium_plugin_manager_grant (manager, self->id, self->permissions[i],
                                     NULL);

  plugin = chromium_plugin_manager_lookup (manager, self->id);
  if (plugin)
    {
      if (mode != PERMISSION_SETTLE_DENY &&
          plugin->state == CHROMIUM_PLUGIN_STATE_SCANNED &&
          all_declared_granted (plugin))
        {
          plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_ENABLED);
          plugin_notify_state (plugin);
        }
      g_object_unref (plugin);
    }

  self->settled = TRUE;
  g_hash_table_remove (manager->permission_requests, self->id);
  g_signal_emit (manager, manager_signals[SIG_CHANGED], 0);
  g_object_unref (manager);
}

void
chromium_plugin_permission_request_accept (ChromiumPluginPermissionRequest *self)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_PERMISSION_REQUEST (self));

  permission_request_settle (self, PERMISSION_SETTLE_ACCEPT);
}

void
chromium_plugin_permission_request_deny (ChromiumPluginPermissionRequest *self)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_PERMISSION_REQUEST (self));

  permission_request_settle (self, PERMISSION_SETTLE_DENY);
}

void
chromium_plugin_permission_request_settle (ChromiumPluginPermissionRequest *self)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_PERMISSION_REQUEST (self));

  permission_request_settle (self, PERMISSION_SETTLE_TABLE);
}

const char *
chromium_plugin_permission_request_get_id (ChromiumPluginPermissionRequest *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_PERMISSION_REQUEST (self), NULL);

  return self->id;
}

const char *const *
chromium_plugin_permission_request_get_permissions (ChromiumPluginPermissionRequest *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_PERMISSION_REQUEST (self), NULL);

  return (const char *const *) self->permissions;
}

gboolean
chromium_plugin_manager_enable (ChromiumPluginManager *self, const char *id,
                                GError **error)
{
  g_autoptr (ChromiumPlugin) plugin = NULL;
  g_autoptr (ChromiumPluginPermissionRequest) created = NULL;
  ChromiumPluginPermissionRequest *parked;
  g_autofree char **pending = NULL;
  guint n_pending;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);

  /* Held, because the emission below hands the plugin to host code: a handler
     that rescans would drop the store's reference, and this call still has a
     state to set on the far side of it. */
  plugin = chromium_plugin_manager_lookup (self, id);
  if (!plugin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No plugin named \"%s\"", id ? id : "(null)");
      return FALSE;
    }

  if (plugin->state == CHROMIUM_PLUGIN_STATE_ENABLED)
    return TRUE;

  /* Nothing that can be done to an object can un-refuse it: the manifest is the
   * thing that was rejected, and re-reading the directory is the only way for it
   * to become acceptable. */
  if (plugin->state == CHROMIUM_PLUGIN_STATE_REFUSED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Plugin \"%s\" was refused: %s", id,
                   plugin->error_message ? plugin->error_message : "bad-manifest");
      return FALSE;
    }

  pending = pending_permissions (plugin);
  n_pending = g_strv_length (pending);

  if (n_pending > 0)
    {
      /* A parked enable already asked this plugin's question. The table, not a
         second prompt, answers this call; the parked request stays parked for
         the host that is still deciding — and settles an enable that by then
         has nowhere left to go. */
      parked = g_hash_table_lookup (self->permission_requests, id);
      if (parked)
        {
          if (!all_declared_granted (plugin))
            {
              g_autofree char *joined = g_strjoinv (", ", pending);

              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "Plugin \"%s\" needs permissions the host has not "
                           "granted: %s", id, joined);
              return FALSE;
            }
          /* Everything is granted now: this call completes the enable, and the
             parked request has nothing left to wait for. */
          plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_ENABLED);
          permission_request_invalidate (parked);
          plugin_notify_state (plugin);
          g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
          return TRUE;
        }

      /* Nobody connected: the table decides now, exactly as before. One that
         is connected gets the request and may answer now or later; an
         unanswered request no longer fails this enable, it parks it. */
      if (g_signal_has_handler_pending (self,
                                        manager_signals[SIG_GRANT_REQUEST],
                                        0, TRUE))
        {
          created = permission_request_new (self, id, pending);
          g_hash_table_insert (self->permission_requests, g_strdup (id),
                               g_object_ref (created));
          g_signal_emit (self, manager_signals[SIG_GRANT_REQUEST], 0, id,
                         pending, created);

          if (!created->settled)
            {
              /* The synchronous host shape: a handler that granted what it was
                 asked for before returning has already answered, even though it
                 never touched the request. Anything less than the whole set is
                 a pending answer, and the enable waits. */
              if (!all_declared_granted (plugin))
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Plugin \"%s\" enable waits on its permission "
                               "request", id);
                  return FALSE;
                }
              plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_ENABLED);
              permission_request_invalidate (created);
              plugin_notify_state (plugin);
              g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
              return TRUE;
            }
          if (plugin->state == CHROMIUM_PLUGIN_STATE_ENABLED)
            return TRUE;

          {
            g_autofree char *joined = g_strjoinv (", ", pending);

            g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         "Plugin \"%s\" needs permissions the host has not "
                         "granted: %s", id, joined);
            return FALSE;
          }
        }

      if (!all_declared_granted (plugin))
        {
          g_autofree char *joined = g_strjoinv (", ", pending);

          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "Plugin \"%s\" needs permissions the host has not "
                       "granted: %s", id, joined);
          return FALSE;
        }
    }

  plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_ENABLED);
  plugin_notify_state (plugin);
  g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
  return TRUE;
}

void
chromium_plugin_manager_disable (ChromiumPluginManager *self, const char *id)
{
  g_autoptr (ChromiumPlugin) plugin = NULL;
  ChromiumPluginPermissionRequest *parked;

  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));

  /* A parked enable is not a running plugin, so the state check below must not
     be what lets it stay answerable after the host said no. */
  parked = g_hash_table_lookup (self->permission_requests, id);
  if (parked)
    permission_request_invalidate (parked);

  plugin = chromium_plugin_manager_lookup (self, id);
  /* An unknown id and an already-disabled plugin are the same news to a host
     that asked for it not to run: nothing is running. */
  if (!plugin || plugin->state != CHROMIUM_PLUGIN_STATE_ENABLED)
    return;

  /* The grants stay. A plugin turned back on tomorrow does not get asked for the
     same permissions twice, and the host is the one that decides which of them
     should be taken away — that is revoke(). */
  plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_SCANNED);
  plugin_notify_state (plugin);
  g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
}

gboolean
chromium_plugin_manager_grant (ChromiumPluginManager *self, const char *id,
                               const char *permission, GError **error)
{
  g_autoptr (ChromiumPlugin) plugin = NULL;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (permission != NULL, FALSE);

  plugin = chromium_plugin_manager_lookup (self, id);
  if (!plugin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No plugin named \"%s\"", id ? id : "(null)");
      return FALSE;
    }

  if (plugin->state == CHROMIUM_PLUGIN_STATE_REFUSED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Plugin \"%s\" was refused: %s", id,
                   plugin->error_message ? plugin->error_message : "bad-manifest");
      return FALSE;
    }

  if (!g_strv_contains ((const char *const *) plugin->permissions, permission))
    {
      /* A name the manifest never asked for cannot be honoured later, so
         accepting it here would be a table entry that does nothing — which is
         exactly the thing a host would have to debug. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Plugin \"%s\" does not declare the permission \"%s\"", id,
                   permission);
      return FALSE;
    }

  if (g_strv_contains ((const char *const *) plugin->granted, permission))
    return TRUE;

  {
    guint n = g_strv_length (plugin->granted);
    char **extended = g_new0 (char *, n + 2);

    memcpy (extended, plugin->granted, sizeof (char *) * n);
    extended[n] = g_strdup (permission);
    /* The container only: extended[0..n-1] are the very strings the old array
       held, so this is the move revoke() makes in the other direction. Freeing
       them here would leave the table full of names that no longer exist — the
       same failure g_free() guards pending_permissions(). */
    g_free (plugin->granted);
    plugin->granted = extended;
  }

  g_object_notify_by_pspec (G_OBJECT (plugin),
                            plugin_properties[PROP_GRANTED_PERMISSIONS]);
  g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
  return TRUE;
}

gboolean
chromium_plugin_manager_revoke (ChromiumPluginManager *self, const char *id,
                                const char *permission, GError **error)
{
  g_autoptr (ChromiumPlugin) plugin = NULL;
  gboolean was_granted = FALSE;
  guint n, i;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (permission != NULL, FALSE);

  plugin = chromium_plugin_manager_lookup (self, id);
  if (!plugin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No plugin named \"%s\"", id ? id : "(null)");
      return FALSE;
    }

  n = g_strv_length (plugin->granted);
  for (i = 0; i < n; i++)
    if (g_strcmp0 (plugin->granted[i], permission) == 0)
      {
        was_granted = TRUE;
        break;
      }

  if (!was_granted)
    return TRUE;

  {
    char **kept = g_new0 (char *, n);
    guint j;

    for (i = 0, j = 0; i < n; i++)
      if (g_strcmp0 (plugin->granted[i], permission) != 0)
        kept[j++] = plugin->granted[i];

    g_free (plugin->granted);
    plugin->granted = kept;
  }

  /* Taking back a permission the manifest declares takes the plugin out of
     service with it: an enabled plugin running without a permission it asked for
     is the state the table exists to make impossible. A name it never declared
     cannot be in the table anyway. */
  if (plugin->state == CHROMIUM_PLUGIN_STATE_ENABLED &&
      !all_declared_granted (plugin))
    {
      plugin_set_state (plugin, CHROMIUM_PLUGIN_STATE_SCANNED);
      plugin_notify_state (plugin);
    }
  else
    g_object_notify_by_pspec (G_OBJECT (plugin),
                              plugin_properties[PROP_GRANTED_PERMISSIONS]);

  g_signal_emit (self, manager_signals[SIG_CHANGED], 0);
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * Installing: the disk work runs on a task thread, the model never   *
 * leaves the main loop                                               *
 * ------------------------------------------------------------------ */

typedef struct {
  char *id;
  char *root;      /* the install root, copied on the owning thread */
  char *package;   /* install only */
  GAsyncReadyCallback callback;
  gpointer callback_data;
} PluginWork;

static void
plugin_work_free (gpointer data)
{
  PluginWork *work = data;

  g_clear_pointer (&work->id, g_free);
  g_clear_pointer (&work->root, g_free);
  g_clear_pointer (&work->package, g_free);
  g_free (work);
}

/* Recursive removal for uninstall and for an install that half-failed.
   TRUE means "gone" — including the already-gone case. */
static gboolean
remove_tree_at (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name;

  if (dir)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);

          if (g_file_test (child, G_FILE_TEST_IS_DIR))
            remove_tree_at (child);
          else
            g_unlink (child);
        }
      g_dir_close (dir);
    }
  return g_rmdir (path) == 0 || errno == ENOENT;
}

static void
mkdir_parents (const char *base, const char *relative)
{
  char **segments = g_strsplit (relative, "/", 0);
  GString *path = g_string_new (base);

  for (guint i = 0; segments[i]; i++)
    {
      g_string_append_c (path, G_DIR_SEPARATOR);
      g_string_append (path, segments[i]);
      if (segments[i + 1])
        g_mkdir_with_parents (path->str, 0755);
    }

  g_mkdir_with_parents (path->str, 0755);
  g_string_free (path, TRUE);
  g_strfreev (segments);
}

static gboolean
install_extract (const char *package, const char *dest, GError **error)
{
  g_autoptr (ChromiumZip) zip = chromium_zip_open (package, error);

  if (!zip)
    return FALSE;

  g_auto (GStrv) names = chromium_zip_names (zip);
  gboolean wrote_manifest = FALSE;

  for (guint i = 0; names[i]; i++)
    {
      const char *name = names[i];
      const gsize length = strlen (name);

      if (length > 0 && name[length - 1] == '/')
        {
          /* A directory entry: its name minus the slash is the directory to
             make. */
          g_autofree char *dir = g_strndup (name, length - 1);

          mkdir_parents (dest, dir);
          continue;
        }

      g_autofree char *data = NULL;
      gsize size = 0;

      if (!chromium_zip_read (zip, name, &data, &size, error))
        return FALSE;
      if (size == 0)
        {
          /* A zero-byte entry is a file with nothing in it — still a file. */
        }

      mkdir_parents (dest, name);

      {
        g_autofree char *path = g_build_filename (dest, name, NULL);

        if (!g_file_set_contents (path, data ? data : "", (gssize) size,
                                  error))
          return FALSE;
      }
      if (g_strcmp0 (name, CHROMIUM_PLUGIN_MANIFEST_NAME) == 0)
        wrote_manifest = TRUE;
    }

  /* A package without a manifest is not refused here — scan() will show the
     row as refused with its reason, which is the same answer an unpacked
     directory gets. But an empty package (no entries at all) is an archive
     nobody meant to send. */
  if (names[0] == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "\"%s\" holds no entries", package);
      return FALSE;
    }
  (void) wrote_manifest;
  return TRUE;
}

static void
plugin_install_thread (GTask *task, gpointer source_object,
                       gpointer task_data, GCancellable *cancellable)
{
  PluginWork *work = task_data;
  g_autofree char *dest = g_build_filename (work->root, work->id, NULL);
  GError *error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR,
                                       G_IO_ERROR_CANCELLED, "cancelled");
      return;
    }

  if (g_file_test (dest, G_FILE_TEST_EXISTS))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                       "a plugin named \"%s\" is already "
                                       "installed; uninstall it first",
                                       work->id);
      return;
    }

  if (!install_extract (work->package, dest, &error))
    {
      /* Nothing half-extracted stays behind to be scanned as a refusal the
         host never asked for. */
      remove_tree_at (dest);
      g_task_return_error (task, error);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
plugin_uninstall_thread (GTask *task, gpointer source_object,
                         gpointer task_data, GCancellable *cancellable)
{
  PluginWork *work = task_data;
  g_autofree char *dest = g_build_filename (work->root, work->id, NULL);

  remove_tree_at (dest);
  g_task_return_boolean (task, TRUE);
}

static void
plugin_reload_thread (GTask *task, gpointer source_object,
                      gpointer task_data, GCancellable *cancellable)
{
  PluginWork *work = task_data;
  g_autofree char *dest = g_build_filename (work->root, work->id, NULL);

  /* Nothing to pre-read that scan() would not re-read; the check is what
     turns "reload a plugin that is not installed" into an error rather than
     a silent no-op. */
  if (!g_file_test (dest, G_FILE_TEST_IS_DIR))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                       "no plugin named \"%s\" is installed",
                                       work->id);
      return;
    }
  g_task_return_boolean (task, TRUE);
}

/* The task's callback runs on the main loop, which is where the model
   lives — so the scan happens here, before the caller's own callback sees
   the answer. */
static void
plugin_work_done (GObject *source_object, GAsyncResult *result,
                  gpointer user_data)
{
  ChromiumPluginManager *self = CHROMIUM_PLUGIN_MANAGER (source_object);
  PluginWork *work = g_task_get_task_data (G_TASK (result));
  GError *error = NULL;

  chromium_plugin_manager_scan (self, &error);
  if (error)
    {
      /* A path that could not be read is not the install's failure — the
         install itself succeeded or failed on its own merits, and the scan
         error rides the model's own report. */
      g_debug ("plugin: post-install scan: %s", error->message);
      g_clear_error (&error);
    }

  if (work->callback)
    work->callback (source_object, result, work->callback_data);
}

static void
plugin_work_run (ChromiumPluginManager *self, PluginWork *work,
                 int io_priority, GCancellable *cancellable,
                 GTaskThreadFunc thread)
{
  /* A fresh task per call, handed the wrapper as the caller-visible
     callback; the user's own callback travels in the work record. */
  GTask *task = g_task_new (self, cancellable, plugin_work_done, work);

  g_task_set_task_data (task, work, plugin_work_free);
  g_task_set_priority (task, io_priority);
  g_task_run_in_thread (task, thread);
  g_object_unref (task);
}

void
chromium_plugin_manager_install_async (ChromiumPluginManager *self,
                                       const char *package, const char *id,
                                       int io_priority,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));
  g_return_if_fail (package != NULL && *package);
  g_return_if_fail (id != NULL);

  PluginWork *work = g_new0 (PluginWork, 1);

  work->id = g_strdup (id);
  work->root = g_strdup (self->install_root);
  work->package = g_strdup (package);
  work->callback = callback;
  work->callback_data = user_data;

  /* Refused before a thread exists: an id that could never be a URI host,
     and a manager with nowhere to install. Both are the caller's setup
     mistakes, and the async shape should not soften them into a refused
     row. */
  if (!id_is_usable (id))
    {
      GTask *task = g_task_new (self, cancellable, plugin_work_done, work);

      g_task_set_task_data (task, work, plugin_work_free);
      g_task_return_new_error (task, G_IO_ERROR,
                                       G_IO_ERROR_INVALID_ARGUMENT,
                                       "\"%s\" is not a usable plugin id",
                                       id);
      g_object_unref (task);
      return;
    }
  if (!work->root || !g_file_test (work->root, G_FILE_TEST_IS_DIR))
    {
      GTask *task = g_task_new (self, cancellable, plugin_work_done, work);

      g_task_set_task_data (task, work, plugin_work_free);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "no install root: this session has "
                                       "no data directory");
      g_object_unref (task);
      return;
    }

  plugin_work_run (self, work, io_priority, cancellable,
                   plugin_install_thread);
}

gboolean
chromium_plugin_manager_install_finish (ChromiumPluginManager *self,
                                        GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
chromium_plugin_manager_uninstall_async (ChromiumPluginManager *self,
                                         const char *id, int io_priority,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));
  g_return_if_fail (id != NULL);

  PluginWork *work = g_new0 (PluginWork, 1);

  work->id = g_strdup (id);
  work->root = g_strdup (self->install_root);
  work->callback = callback;
  work->callback_data = user_data;

  if (!work->root)
    {
      GTask *task = g_task_new (self, cancellable, plugin_work_done, work);

      g_task_set_task_data (task, work, plugin_work_free);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "no install root: this session has "
                                       "no data directory");
      g_object_unref (task);
      return;
    }

  /* A plugin whose state lives in the Web frontend forgets it here, before
     a future install of the same id could inherit the old table. */
  _chromium_plugin_web_forget (self, id);

  plugin_work_run (self, work, io_priority, cancellable,
                   plugin_uninstall_thread);
}

gboolean
chromium_plugin_manager_uninstall_finish (ChromiumPluginManager *self,
                                          GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
chromium_plugin_manager_reload_async (ChromiumPluginManager *self,
                                      const char *id, int io_priority,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));
  g_return_if_fail (id != NULL);

  PluginWork *work = g_new0 (PluginWork, 1);

  work->id = g_strdup (id);
  work->root = g_strdup (self->install_root);
  work->callback = callback;
  work->callback_data = user_data;

  if (!work->root)
    {
      GTask *task = g_task_new (self, cancellable, plugin_work_done, work);

      g_task_set_task_data (task, work, plugin_work_free);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "no install root: this session has "
                                       "no data directory");
      g_object_unref (task);
      return;
    }

  plugin_work_run (self, work, io_priority, cancellable,
                   plugin_reload_thread);
}

gboolean
chromium_plugin_manager_reload_finish (ChromiumPluginManager *self,
                                       GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
chromium_plugin_manager_respond (ChromiumPluginManager *self, int reply_id,
                                 gboolean ok, const char *value)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));

  _chromium_plugin_web_respond (self, reply_id, ok, value);
}

void
chromium_plugin_manager_activate_action (ChromiumPluginManager *self,
                                         const char *id)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));
  g_return_if_fail (id != NULL);

  /* The host hears it first (its popup policy is its own), then the pages
     that registered a listener. */
  g_signal_emit (self, manager_signals[SIG_PLUGIN_ACTION], 0, id);
  chromium_plugin_web_deliver_action (self, id);
}

static void
chromium_plugin_manager_class_init (ChromiumPluginManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = chromium_plugin_manager_dispose;

  /**
   * ChromiumPluginManager::changed:
   *
   * The set of plugins, or one plugin's state. ::items-changed on
   * chromium_plugin_manager_get_plugins() is the same news about the set only,
   * so a host that bound a model needs neither.
   */
  manager_signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ChromiumPluginManager::grant-request:
   * @self: the manager
   * @id: the plugin asking
   * @permissions: the declared permissions not yet granted, as a
   *   %NULL-terminated string array owned by the manager for the duration of the
   *   emission
   * @request: the parked enable, a #ChromiumPluginPermissionRequest
   *
   * An enable was asked for while the plugin still declares permissions the host
   * has not granted. This is the moment to show a prompt — or to refuse.
   *
   * The prompt's answer goes through @request: accept() grants everything
   * pending and completes the enable, deny() fails it, settle() decides from
   * the table after the host granted or revoked individual names. A handler
   * that answers inside the emission gives enable() a result in the same call;
   * one that answers later leaves the enable parked (enable() returned %FALSE
   * with #G_IO_ERROR_PENDING) and the manager's ::changed announces the
   * settling. No handler at all keeps the old rule: the table decides
   * immediately, and an ungranted request is a deny.
   */
  manager_signals[SIG_GRANT_REQUEST] =
    g_signal_new ("grant-request", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_STRV,
                  CHROMIUM_TYPE_PLUGIN_PERMISSION_REQUEST);

  /**
   * ChromiumPluginManager::plugin-error:
   * @self: the manager
   * @id: the plugin the message is about
   * @message: the machine-readable reason
   *
   * A manifest was refused, a field was named as one this component will not
   * act on, or a popup size was clamped. Emitted after every scan that rebuilt
   * the model, so a handler that looks @id up finds the row it belongs to —
   * including for a refusal, which leaves the plugin in the list with
   * #ChromiumPlugin:error-message set to the same string.
   */
  manager_signals[SIG_PLUGIN_ERROR] =
    g_signal_new ("plugin-error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_STRING, G_TYPE_STRING);

  /**
   * ChromiumPluginManager::plugin-message:
   * @self: the manager
   * @id: the plugin the call came through
   * @api: the API the shim named ("storage.local.set", "tabs.query", …)
   * @args: the call's arguments as one JSON text
   * @reply_id: what chromium_plugin_manager_respond() answers through
   *
   * One call from a page. Returning %TRUE claims it: the emission stops, the
   * built-in handlers step aside, and respond() is the claimant's job — now
   * or long after, keyed on @reply_id. Returning %FALSE, or connecting
   * nothing, hands the call to the component's own handlers.
   */
  manager_signals[SIG_PLUGIN_MESSAGE] =
    g_signal_new ("plugin-message", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, g_signal_accumulator_true_handled, NULL, NULL,
                  G_TYPE_BOOLEAN, 4,
                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);

  /**
   * ChromiumPluginManager::plugin-action:
   * @self: the manager
   * @id: the plugin whose action was pressed
   *
   * The host's button for @id went down. What that shows — the popup in a
   * window of its own, a popover, nothing — is the host's policy, read off
   * the plugin's popup-uri and size. The plugin's own
   * chrome.action.onClicked listeners hear it in the same instant.
   */
  manager_signals[SIG_PLUGIN_ACTION] =
    g_signal_new ("plugin-action", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
chromium_plugin_manager_init (ChromiumPluginManager *self)
{
  self->store = g_list_store_new (CHROMIUM_TYPE_PLUGIN);
  self->search_paths = g_ptr_array_new_with_free_func (g_free);
  self->permission_requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_object_unref);
}

static void
chromium_plugin_manager_dispose (GObject *object)
{
  ChromiumPluginManager *self = CHROMIUM_PLUGIN_MANAGER (object);

  /* Every parked request dies with the manager: there is no table left to
     settle against, and the row it was asked about is going away. Each value
     is referenced for the walk, because invalidating it removes the table's
     own reference — possibly the last one until the host lets go. */
  if (self->permission_requests)
    {
      GPtrArray *requests = g_ptr_array_new_with_free_func (g_object_unref);
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->permission_requests);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        g_ptr_array_add (requests, g_object_ref (value));
      for (guint i = 0; i < requests->len; i++)
        permission_request_invalidate (g_ptr_array_index (requests, i));
      g_ptr_array_unref (requests);
      g_clear_pointer (&self->permission_requests, g_hash_table_unref);
    }

  /* Dropping the store here rather than in finalize() is what lets a plugin's
   * weak manager reference be dead before the plugins themselves go: nothing can
   * reach back into a manager that is being taken down. */
  g_clear_object (&self->store);
  g_clear_pointer (&self->search_paths, g_ptr_array_unref);
  g_clear_pointer (&self->install_root, g_free);
  /* The session link goes with it — the session clears it from its own
     dispose first; this is the belt to those braces. */
  self->session = NULL;

  G_OBJECT_CLASS (chromium_plugin_manager_parent_class)->dispose (object);
}

/* ------------------------------------------------------------------ *
 * The session link and the manifest facts the frontends read         *
 * ------------------------------------------------------------------ */

/* Weak, for the same reason the plugins' back-reference is: the session owns
 * the manager, and the manager outliving it would be a way to send commands
 * into a channel that is gone. */
void
_chromium_plugin_manager_set_session (ChromiumPluginManager *self,
                                      ChromiumSession *session)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));
  g_return_if_fail (self->session == NULL);

  /* A plain back-pointer, not a weak one: the session owns the manager and
   * clears this link from its own dispose before the manager goes, so the
   * manager can never dereference a dead session. (The weak-pointer form
   * was measured clearing itself mid-run — 2026-10-26, session=NULL by the
   * first plugin message — and the ownership that makes the weak form
   * redundant is already the dispose order above.) */
  self->session = session;
}

ChromiumSession *
_chromium_plugin_manager_get_session (ChromiumPluginManager *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self), NULL);
  return self->session;
}

/* The session derives this from its data directory and re-derives it when
   the property moves; the next scan() reads it. */
void
_chromium_plugin_manager_set_install_root (ChromiumPluginManager *self,
                                           const char *dir)
{
  g_return_if_fail (CHROMIUM_IS_PLUGIN_MANAGER (self));

  if (g_strcmp0 (self->install_root, dir) == 0)
    return;

  g_clear_pointer (&self->install_root, g_free);
  self->install_root = g_strdup (dir);
}

const char *
_chromium_plugin_get_library (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->library;
}

const char *
_chromium_plugin_get_symbol (ChromiumPlugin *self)
{
  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);
  return self->symbol;
}

JsonArray *
_chromium_plugin_get_content_scripts (ChromiumPlugin *self)
{
  JsonArray *array = NULL;

  g_return_val_if_fail (CHROMIUM_IS_PLUGIN (self), NULL);

  if (!self->manifest ||
      read_array (self->manifest, "content_scripts", &array) != MEMBER_READ)
    return NULL;

  return array;
}
