#include <gtk/gtk.h>

#include "chromium-session.h"
#include "chromium-view.h"

/* Permission requests. A page's ask for a microphone, a location or the
 * clipboard is a question only the host can answer — and leaving it unanswered
 * costs more than silence: the page's promise stays pending, and a second
 * media ask never arrives until the first is answered.
 *
 * ::permission-request (id, kind, origin, permissions, raw) with the handler
 * returning TRUE means the host owns the answer and owes exactly one
 * permission_respond(); returning FALSE, or connecting nothing, gets a deny
 * from the session. ::permission-dismissed says Chromium took a still-held
 * prompt back (result "ignore") — answering that id now does nothing, so the
 * row has to go.
 *
 *   CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-permissions
 */
typedef struct {
  int id;
  GtkWidget *row;
} Ask;

typedef struct {
  GtkWidget *window;
  GtkWidget *list;
  ChromiumSession *session;
  GList *asks;
} Permissions;

static Permissions perms;

static void
ask_free (Ask *ask)
{
  g_free (ask);
}

static Ask *
find_ask (int id)
{
  GList *l;

  for (l = perms.asks; l; l = l->next)
    {
      Ask *ask = l->data;

      if (ask->id == id)
        return ask;
    }
  return NULL;
}

static void
ask_remove (Ask *ask)
{
  GList *link = g_list_find (perms.asks, ask);

  if (link)
    perms.asks = g_list_delete_link (perms.asks, link);
  if (perms.list && ask->row)
    gtk_box_remove (GTK_BOX (perms.list), ask->row);
  ask_free (ask);
}

static void
answer (GtkButton *button, gpointer data)
{
  Ask *ask = data;
  gboolean allow = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
                                                       "allow"));

  chromium_session_permission_respond (perms.session, ask->id,
                                       allow ? CHROMIUM_PERMISSION_ACCEPT
                                             : CHROMIUM_PERMISSION_DENY);
  ask_remove (ask);
}

static gboolean
session_permission_request (ChromiumSession *session, int id,
                            const char *kind, const char *origin,
                            GList *permissions, int raw, gpointer data)
{
  Ask *ask;
  GtkWidget *allow;
  GtkWidget *deny;
  g_autoptr (GString) names = g_string_new (NULL);
  GList *l;

  if (!perms.window)
    {
      /* No UI to ask with. FALSE is the answer a host without a prompt gives:
       * the session denies instead of leaving the promise pending. */
      g_printerr ("permission %d after window closed: denied\n", id);
      return FALSE;
    }

  /* The name list belongs to the session for the duration of this emission;
   * copy what a row needs into the label now. */
  for (l = permissions; l; l = l->next)
    {
      if (names->len)
        g_string_append (names, ", ");
      g_string_append (names, (const char *) l->data);
    }

  ask = g_new0 (Ask, 1);
  ask->id = id;
  ask->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

  {
    g_autofree char *text =
      g_strdup_printf ("%s wants %s: %s", origin && *origin ? origin : "a page",
                       kind && *kind ? kind : "permission",
                       names->len ? names->str : "(nothing named)");
    GtkWidget *label = gtk_label_new (text);

    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_box_append (GTK_BOX (ask->row), label);
  }

  allow = gtk_button_new_with_label ("Allow");
  deny = gtk_button_new_with_label ("Deny");
  g_object_set_data (G_OBJECT (allow), "allow", GINT_TO_POINTER (TRUE));
  g_object_set_data (G_OBJECT (deny), "allow", GINT_TO_POINTER (FALSE));
  g_signal_connect (allow, "clicked", G_CALLBACK (answer), ask);
  g_signal_connect (deny, "clicked", G_CALLBACK (answer), ask);
  gtk_box_append (GTK_BOX (ask->row), allow);
  gtk_box_append (GTK_BOX (ask->row), deny);

  gtk_box_append (GTK_BOX (perms.list), ask->row);
  perms.asks = g_list_append (perms.asks, ask);

  g_printerr ("permission %d kind=%s raw=%d [%s]\n", id, kind ? kind : "?",
              raw, names->str);
  return TRUE;
}

static void
session_permission_dismissed (ChromiumSession *session, int id,
                              const char *result, gpointer data)
{
  Ask *ask = find_ask (id);

  /* Chromium took the prompt back under the host's feet: the row would stay
   * on screen offering buttons that answer nothing. Media asks have no
   * dismiss callback at all — those rows leave through answer() only. */
  g_printerr ("permission %d dismissed (%s)\n", id, result ? result : "?");
  if (ask)
    ask_remove (ask);
}

static void
session_closed (ChromiumSession *session, const char *reason, gpointer data)
{
  g_printerr ("helper gone: %s\n", reason ? reason : "(no reason)");
}

static gboolean
on_close (GtkWindow *window, gpointer data)
{
  chromium_session_stop (perms.session);
  return FALSE;
}

static void
on_window_destroy (GtkWindow *window, gpointer data)
{
  perms.window = NULL;
  perms.list = NULL;
  g_list_free_full (perms.asks, (GDestroyNotify) ask_free);
  perms.asks = NULL;
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget *hint;
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ChromiumView *view;
  GError *error = NULL;
  const char *url = g_getenv ("CEFNEST_URL");

  perms.window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (perms.window), "CEFNest permissions");
  gtk_window_set_default_size (GTK_WINDOW (perms.window), 1000, 700);

  hint = gtk_label_new ("permission asks show up as rows; a page asking for a "
                        "second media permission waits for the first answer");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 6);
  gtk_widget_set_margin_top (hint, 3);
  gtk_box_append (GTK_BOX (root), hint);

  perms.list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (perms.list, 6);
  gtk_widget_set_margin_end (perms.list, 6);
  gtk_box_append (GTK_BOX (root), perms.list);

  view = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);
  gtk_box_append (GTK_BOX (root), GTK_WIDGET (view));
  gtk_window_set_child (GTK_WINDOW (perms.window), root);

  perms.session = chromium_session_new (NULL);
  chromium_session_set_view (perms.session, view);

  g_signal_connect (perms.session, "permission-request",
                    G_CALLBACK (session_permission_request), NULL);
  g_signal_connect (perms.session, "permission-dismissed",
                    G_CALLBACK (session_permission_dismissed), NULL);
  g_signal_connect (perms.session, "closed", G_CALLBACK (session_closed),
                    NULL);
  g_signal_connect (perms.window, "close-request", G_CALLBACK (on_close),
                    NULL);
  g_signal_connect (perms.window, "destroy", G_CALLBACK (on_window_destroy),
                    NULL);

  gtk_window_present (GTK_WINDOW (perms.window));

  if (!chromium_session_start (perms.session,
                               url && *url ? url : "https://cnb.cool/yanzhiming/CEFNest",
                               &error))
    {
      g_warning ("cannot start the helper: %s", error->message);
      g_clear_error (&error);
    }

  g_object_set_data_full (G_OBJECT (perms.window), "session",
                          g_object_ref (perms.session), g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("dev.cefnest.permissions",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
