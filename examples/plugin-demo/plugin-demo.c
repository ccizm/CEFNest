/* The native plugin the plugin gate installs. One exported symbol, two
 * registrations: a context-menu row the host routes back to on_menu_command,
 * and a subscription to the session's ::load-end that proves the registry's
 * connection bookkeeping is real (the unload path disconnects it — see the
 * "unloading (… connections)" line the gate greps).
 *
 * It links nothing but this library's public header. Everything it calls is
 * resolved from the host process when the host loads it, which is the whole
 * ABI: build this once against chromium-plugin.h and it runs against any
 * host whose CEFNEST_PLUGIN_API_VERSION is still 1. */

#include <stdio.h>

#include "chromium-plugin.h"
#include "chromium-session.h"

/* The menu row's callback: ChromiumPluginMenuCallback — the slot, the node
 * facts as one JSON object text (page/link/source URLs, the selection), and
 * whatever user_data the registration carried. */
static void
on_menu_command (int slot, const char *params_json, gpointer user_data)
{
  fprintf (stderr, "plugin-demo: menu command slot=%d params=%s\n",
           slot, params_json ? params_json : "null");
}

/* "load-end" is a session signal, so the handler has the signal's own
 * signature — session first, then (url, http_code), then user_data. */
static void
on_load_end (ChromiumSession *session, const char *url, int http_code,
             gpointer user_data)
{
  fprintf (stderr, "plugin-demo: page loaded %s (http=%d)\n",
           url ? url : "", http_code);
}

/* The one export. api_version is the load host's; a mismatch means this
 * binary was built against a different ABI, and the honest answer is to
 * register nothing — the load reports a plugin with no rows rather than one
 * that misbehaves. */
void
cefnest_plugin_register (ChromiumPluginRegistry *registry, guint api_version)
{
  if (api_version != CEFNEST_PLUGIN_API_VERSION)
    {
      fprintf (stderr, "plugin-demo: api version %u not supported\n",
               api_version);
      return;
    }

  chromium_plugin_registry_add_menu_item (registry,
                                          "插件示例：命中记录",
                                          0,
                                          G_CALLBACK (on_menu_command),
                                          NULL);
  chromium_plugin_registry_subscribe (registry, "load-end",
                                      G_CALLBACK (on_load_end), NULL);
}
