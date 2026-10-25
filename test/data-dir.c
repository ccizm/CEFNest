/* The data directory's contract without a compositor or Chromium in the loop:
 * path validation happens before a view is wanted, the directory is created
 * 0700 and never overwritten, clear_data() refuses while a helper holds the
 * lock and removes the whole tree when none does.
 *
 * The cross-process half is a real second process holding the real flock —
 * that is the only way to prove the lock is what clear_data() consults rather
 * than a pid check that would pass with no helper at all. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "chromium-session.h"

static void
assert_error (const GError *error, GIOErrorEnum code, const char *needle)
{
  g_assert_nonnull (error);
  g_assert_cmpint (error->domain, ==, G_IO_ERROR);
  g_assert_cmpint (error->code, ==, code);
  g_assert_nonnull (strstr (error->message, needle));
}

static ChromiumSession *
session_with_directory (const char *dir)
{
  ChromiumSession *session = chromium_session_new (NULL);

  chromium_session_set_data_directory (session, dir);
  return session;
}

static void
test_property (void)
{
  ChromiumSession *session = chromium_session_new (NULL);
  char *dir = NULL;

  g_assert_null (chromium_session_get_data_directory (session));
  g_object_get (session, "data-directory", &dir, NULL);
  g_assert_null (dir);

  chromium_session_set_data_directory (session, "/tmp/cefnest-data-dir");
  g_assert_cmpstr (chromium_session_get_data_directory (session), ==,
                   "/tmp/cefnest-data-dir");
  g_object_get (session, "data-directory", &dir, NULL);
  g_assert_cmpstr (dir, ==, "/tmp/cefnest-data-dir");
  g_free (dir);

  /* Empty means "no directory", the same as NULL: a host that clears its
   * configuration entry should not have to special-case "". */
  chromium_session_set_data_directory (session, "");
  g_assert_null (chromium_session_get_data_directory (session));

  g_object_set (session, "data-directory", "/tmp/cefnest-other", NULL);
  g_assert_cmpstr (chromium_session_get_data_directory (session), ==,
                   "/tmp/cefnest-other");
  g_object_set (session, "data-directory", NULL, NULL);
  g_assert_null (chromium_session_get_data_directory (session));

  g_object_unref (session);
}

static void
test_relative_path (void)
{
  ChromiumSession *session = session_with_directory ("relative/profile");
  GError *error = NULL;

  /* Before the view check: start() is asked for a directory, not a browser. */
  g_assert_false (chromium_session_start (session, "", &error));
  assert_error (error, G_IO_ERROR_INVALID_ARGUMENT, "absolute");
  g_clear_error (&error);

  chromium_session_set_data_directory (session, "~/profile");
  g_assert_false (chromium_session_start (session, "", &error));
  assert_error (error, G_IO_ERROR_INVALID_ARGUMENT, "absolute");
  g_clear_error (&error);

  g_object_unref (session);
}

static void
test_creates_directory (void)
{
  g_autofree char *base = g_dir_make_tmp ("cefnest-data-dir-XXXXXX", NULL);
  g_autofree char *dir = g_build_filename (base, "profile", NULL);
  ChromiumSession *session = session_with_directory (dir);
  GError *error = NULL;
  GStatBuf stat_buf;

  /* No view is set, so this stops one step after the data-directory checks —
   * which is exactly how far a headless test can follow start(). */
  g_assert_false (chromium_session_start (session, "", &error));
  assert_error (error, G_IO_ERROR_FAILED, "no view set");
  g_clear_error (&error);

  g_assert_true (g_file_test (dir, G_FILE_TEST_IS_DIR));
  g_assert_cmpint (g_stat (dir, &stat_buf), ==, 0);
  g_assert_cmpuint (stat_buf.st_mode & 0777, ==, 0700);

  g_assert_true (chromium_session_clear_data (session, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));

  g_object_unref (session);
}

static void
test_file_is_not_a_directory (void)
{
  g_autofree char *base = g_dir_make_tmp ("cefnest-data-dir-XXXXXX", NULL);
  g_autofree char *leaf = g_build_filename (base, "leaf", NULL);
  ChromiumSession *session = session_with_directory (leaf);
  GError *error = NULL;

  g_assert_true (g_file_set_contents (leaf, "not a profile", -1, NULL));

  g_assert_false (chromium_session_start (session, "", &error));
  assert_error (error, G_IO_ERROR_NOT_DIRECTORY, "not a directory");
  g_clear_error (&error);

  /* A file where a directory was asked for belongs to somebody else: it is
   * still there, still a file, and still has its contents. */
  g_assert_true (g_file_test (leaf, G_FILE_TEST_IS_REGULAR));

  g_object_unref (session);
}

static void
test_clear_without_directory (void)
{
  ChromiumSession *session = chromium_session_new (NULL);
  g_autofree char *never = g_build_filename (g_get_tmp_dir (),
                                             "cefnest-never-created",
                                             NULL);
  GError *error = NULL;

  g_assert_false (chromium_session_clear_data (session, &error));
  assert_error (error, G_IO_ERROR_FAILED, "no data directory");
  g_clear_error (&error);

  /* Nothing to remove is success: the caller asked for "gone", and it is. */
  chromium_session_set_data_directory (session, never);
  g_assert_true (chromium_session_clear_data (session, &error));
  g_assert_no_error (error);

  g_object_unref (session);
}

static void
test_clear_removes_tree (void)
{
  g_autofree char *base = g_dir_make_tmp ("cefnest-data-dir-XXXXXX", NULL);
  g_autofree char *dir = g_build_filename (base, "profile", NULL);
  g_autofree char *sub = g_build_filename (dir, "browser", NULL);
  g_autofree char *deep = g_build_filename (sub, "IndexedDB", NULL);
  g_autofree char *a = g_build_filename (dir, "Cookies", NULL);
  g_autofree char *b = g_build_filename (deep, "db", NULL);
  ChromiumSession *session = session_with_directory (dir);
  GError *error = NULL;

  g_mkdir_with_parents (deep, 0700);
  g_assert_true (g_file_set_contents (a, "cookie", -1, NULL));
  g_assert_true (g_file_set_contents (b, "index", -1, NULL));

  g_assert_cmpstr (chromium_session_get_data_directory (session), ==, dir);
  g_assert_true (chromium_session_clear_data (session, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));

  /* The property is not part of what was cleared: the next start() rebuilds
   * the directory without the host setting it again. */
  g_assert_cmpstr (chromium_session_get_data_directory (session), ==, dir);

  g_object_unref (session);
}

static void
test_clear_refuses_a_held_lock (void)
{
  g_autofree char *base = g_dir_make_tmp ("cefnest-data-dir-XXXXXX", NULL);
  g_autofree char *dir = g_build_filename (base, "profile", NULL);
  g_autofree char *lock = g_build_filename (dir, ".cefnest.lock", NULL);
  g_autofree char *payload = g_build_filename (dir, "Cookies", NULL);
  ChromiumSession *session;
  int pipe_fds[2];
  pid_t pid;
  char byte;
  GError *error = NULL;

  g_mkdir_with_parents (dir, 0700);
  g_assert_true (g_file_set_contents (payload, "cookie", -1, NULL));
  g_assert_cmpint (pipe (pipe_fds), ==, 0);

  pid = fork ();
  g_assert_cmpint (pid, >=, 0);
  if (pid == 0)
    {
      int fd;

      close (pipe_fds[0]);
      fd = open (lock, O_CREAT | O_RDWR, 0600);
      if (fd < 0 || flock (fd, LOCK_EX) == -1)
        _exit (1);
      if (write (pipe_fds[1], "y", 1) != 1)
        _exit (1);
      for (;;)
        pause ();
    }

  close (pipe_fds[1]);
  g_assert_cmpint (read (pipe_fds[0], &byte, 1), ==, 1);
  close (pipe_fds[0]);

  session = session_with_directory (dir);
  g_assert_false (chromium_session_clear_data (session, &error));
  assert_error (error, G_IO_ERROR_BUSY, "in use");
  g_clear_error (&error);
  g_assert_true (g_file_test (payload, G_FILE_TEST_EXISTS));

  /* The kernel releases the lock with the holder: no stale state to clear by
   * hand, and the same call now succeeds. */
  g_assert_cmpint (kill (pid, SIGKILL), ==, 0);
  g_assert_cmpint (waitpid (pid, NULL, 0), ==, pid);

  g_assert_true (chromium_session_clear_data (session, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));

  g_object_unref (session);
}

int
main (void)
{
  test_property ();
  test_relative_path ();
  test_creates_directory ();
  test_file_is_not_a_directory ();
  test_clear_without_directory ();
  test_clear_removes_tree ();
  test_clear_refuses_a_held_lock ();

  g_print ("data-dir gate ok\n");
  return 0;
}
