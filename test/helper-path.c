/* The helper is a separate binary, so the first thing a component that has left
 * the tree it was built in has to get right is where it looks for it. This pins
 * that order without a browser, a window or a display: chromium_session_new()
 * answers with a path and chromium_session_get_helper_path() reports the same
 * string a failed start() would name in its error.
 *
 * It is also the only check that the two paths the build configures actually
 * reach the library: a target that forgets its defines still compiles, and then
 * every installed component goes looking for a bare "cefnest-helper" on somebody
 * else's PATH. */
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "chromium-session.h"

#ifndef HELPER_PATH
#define HELPER_PATH "cefnest-helper"
#endif

#ifndef HELPER_DEVELOPMENT
#define HELPER_DEVELOPMENT ""
#endif

/* A session that was told nothing, as the string it hands out. */
static char *
reported_path (void)
{
  ChromiumSession *session = chromium_session_new (NULL);
  char *path = g_strdup (chromium_session_get_helper_path (session));

  g_object_unref (session);
  return path;
}

int
main (void)
{
  char *path;
  const char *want;

  /* The default this build was configured with is an absolute path. Meson hands
   * an install directory back relative to the prefix, and joining the two is
   * what keeps a component from looking for its helper under whatever directory
   * the browser was started in — which no later assertion can tell apart from a
   * working install, because the development fallback underneath it is absolute
   * and does exist. */
  g_assert_true (g_path_is_absolute (HELPER_PATH));

  /* A path written down is taken as written, executable or not: a host that
   * names a helper has to see its own string in the error when it is wrong. */
  g_setenv ("CEFNEST_HELPER", "/tmp/cefnest-helper-missing", TRUE);
  g_unsetenv ("CHROMIUM_HELPER");
  path = reported_path ();
  g_assert_cmpstr (path, ==, "/tmp/cefnest-helper-missing");
  g_free (path);

  /* One that does not look like a path does not count as one, and the other
   * variable — which the demo also uses as a flag — is then the one that says
   * where it is. */
  g_setenv ("CEFNEST_HELPER", "cefnest-helper", TRUE);
  g_setenv ("CHROMIUM_HELPER", "/tmp/another-missing-helper", TRUE);
  path = reported_path ();
  g_assert_cmpstr (path, ==, "/tmp/another-missing-helper");
  g_free (path);

  /* And a host that hands the path to the constructor does not need either. */
  {
    ChromiumSession *session = chromium_session_new ("/tmp/explicit-helper");
    g_assert_cmpstr (chromium_session_get_helper_path (session), ==,
                     "/tmp/explicit-helper");
    g_object_unref (session);
  }

  /* Nothing written down: the configured install path while it exists, the
   * development tree only behind it, and the configured path again when neither
   * does — so a run can never pick up a leftover build by accident. */
  g_unsetenv ("CEFNEST_HELPER");
  g_unsetenv ("CHROMIUM_HELPER");
  path = reported_path ();

  /* Absolute, whatever the build was configured with. A prefix-relative
   * directory is what an install rule wants and what a compiled-in default is
   * not: the component would then look for its helper under wherever the
   * browser was started from. */
  g_assert_true (g_path_is_absolute (path));

  if (g_file_test (HELPER_PATH, G_FILE_TEST_IS_EXECUTABLE))
    want = HELPER_PATH;
  else if (*HELPER_DEVELOPMENT &&
           g_file_test (HELPER_DEVELOPMENT, G_FILE_TEST_IS_EXECUTABLE))
    want = HELPER_DEVELOPMENT;
  else
    want = HELPER_PATH;

  g_assert_cmpstr (path, ==, want);

  g_print ("helper-path gate ok: %s\n", path);
  g_free (path);
  return 0;
}
