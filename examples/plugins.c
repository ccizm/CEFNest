/* A host's plugin management page: the scan as rows, the permission table as
 * switches, and the action as a window of its own.
 *
 * Everything here is the component's public API — no plugin machinery is
 * reached past it. What the page deliberately does NOT decide for anyone
 * else is policy: the grant here is this page's answer to ::grant-request,
 * and a build with different UI opinions is still the same component
 * underneath.
 *
 *   ./cefnest-plugins <plugin-dir> [plugin-dir...]
 *
 * Each argument becomes a search path; the rows below are the model. The
 * session starts but is never navigated — this example is about the page. */

#include <gtk/gtk.h>

#include "chromium-session.h"

static ChromiumSession *session = NULL;
static ChromiumPluginManager *manager = NULL;
static GtkWindow *main_window = NULL;

/* ::grant-request, the parked shape: the manager keeps the enable waiting
 * until this page answers through the request object, so a dialog can run on
 * the main loop without the enable failing underneath it.
 *
 * The rows are one check button per declared-but-ungranted permission. Allow
 * grants the ticked ones and then settle()s, which decides from the table the
 * host just wrote; Deny leaves the table alone. Closing the window is a deny.
 * An invalidated request (the plugin was disabled, or a rescan dropped it)
 * makes all three answers no-ops, so this dialog needs no cancellation path of
 * its own. */
typedef struct {
  ChromiumPluginPermissionRequest *request;
  GPtrArray *checks;   /* GtkCheckButton*, owned by the dialog */
} GrantPrompt;

static void
grant_prompt_free (GrantPrompt *prompt)
{
  g_clear_object (&prompt->request);
  g_clear_pointer (&prompt->checks, g_ptr_array_unref);
  g_free (prompt);
}

static void
grant_prompt_answer (GtkDialog *dialog, int response, gpointer data)
{
  GrantPrompt *prompt = data;

  if (response == GTK_RESPONSE_ACCEPT)
    {
      const char *id = chromium_plugin_permission_request_get_id (prompt->request);

      for (guint i = 0; i < prompt->checks->len; i++)
        {
          GtkCheckButton *check = g_ptr_array_index (prompt->checks, i);

          if (gtk_check_button_get_active (check))
            chromium_plugin_manager_grant (manager, id,
                                           g_object_get_data (G_OBJECT (check),
                                                              "permission"),
                                           NULL);
        }
      chromium_plugin_permission_request_settle (prompt->request);
    }
  else
    chromium_plugin_permission_request_deny (prompt->request);

  gtk_window_destroy (GTK_WINDOW (dialog));
}

static void
on_grant_request (ChromiumPluginManager *m, const char *id,
                  const char *const *permissions,
                  ChromiumPluginPermissionRequest *request, gpointer data)
{
  GrantPrompt *prompt = g_new0 (GrantPrompt, 1);
  GtkWidget *dialog;
  GtkWidget *box;
  g_autofree char *ask = NULL;

  prompt->request = g_object_ref (request);
  prompt->checks = g_ptr_array_new ();

  dialog = gtk_dialog_new_with_buttons ("Permissions",
                                        main_window,
                                        GTK_DIALOG_MODAL |
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                        "_Deny", GTK_RESPONSE_REJECT,
                                        "_Allow", GTK_RESPONSE_ACCEPT,
                                        NULL);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 16);
  gtk_widget_set_margin_end (box, 16);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 16);
  ask = g_strdup_printf ("Plugin \"%s\" asks for:", id);
  gtk_box_append (GTK_BOX (box), gtk_label_new (ask));
  for (guint i = 0; permissions && permissions[i]; i++)
    {
      GtkWidget *check = gtk_check_button_new_with_label (permissions[i]);

      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), TRUE);
      g_object_set_data_full (G_OBJECT (check), "permission",
                              g_strdup (permissions[i]), g_free);
      g_ptr_array_add (prompt->checks, check);
      gtk_box_append (GTK_BOX (box), check);
    }
  gtk_box_append (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                  box);
  g_object_set_data_full (G_OBJECT (dialog), "grant-prompt", prompt,
                          (GDestroyNotify) grant_prompt_free);
  g_signal_connect (dialog, "response", G_CALLBACK (grant_prompt_answer),
                    prompt);
  /* Closing the window is the deny button without the click. */
  g_signal_connect_swapped (dialog, "destroy",
                            G_CALLBACK (chromium_plugin_permission_request_deny),
                            request);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
on_plugin_error (ChromiumPluginManager *m, const char *id,
                 const char *message, gpointer data)
{
  g_printerr ("plugins: %s: %s\n", id, message);
}

static void
action_clicked (GtkButton *button, gpointer data)
{
  const char *id = g_object_get_data (G_OBJECT (button), "plugin-id");

  /* ::plugin-action fires from this call; the popup window below opens from
     that signal, the same way the demo host does it. */
  chromium_plugin_manager_activate_action (manager, id);
}

static void
on_plugin_action (ChromiumPluginManager *m, const char *id, gpointer data)
{
  g_autoptr (ChromiumPlugin) plugin =
    chromium_plugin_manager_lookup (m, id);
  const char *popup = plugin ? chromium_plugin_get_popup_uri (plugin) : NULL;

  if (!popup || !*popup)
    return;
  g_printerr ("plugins: opening %s\n", popup);
  chromium_session_new_browser (session, popup, TRUE);
}

/* One row per plugin, built by the list box's own factory: name and version,
 * the state a person needs (the refusal token, or what was granted), the
 * two-way enabled switch, and an action button when there is a popup. */
static GtkWidget *
create_row (gpointer item, gpointer data)
{
  ChromiumPlugin *plugin = CHROMIUM_PLUGIN (item);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new (NULL);
  GtkWidget *subtitle = gtk_label_new (NULL);
  GtkWidget *enable = gtk_switch_new ();
  const char *const *declared = chromium_plugin_get_permissions (plugin);
  g_autofree char *what = NULL;
  ChromiumPluginState state = chromium_plugin_get_state (plugin);

  g_object_set (title, "xalign", 0.0, NULL);
  gtk_label_set_markup
    (GTK_LABEL (title),
     g_markup_printf_escaped ("<b>%s</b> <small>%s · %s</small>",
                              chromium_plugin_get_name (plugin),
                              chromium_plugin_get_version (plugin),
                              chromium_plugin_get_kind (plugin) ?
                                chromium_plugin_get_kind (plugin) : "?"));

  if (state == CHROMIUM_PLUGIN_STATE_REFUSED)
    what = g_strdup_printf ("refused: %s",
                            chromium_plugin_get_error_message (plugin));
  else if (chromium_plugin_get_granted_permissions (plugin)[0])
    what = g_strdup_printf ("granted: %s",
                            chromium_plugin_get_granted_permissions (plugin)[0]);
  else if (declared[0])
    what = g_strdup_printf ("declared: %s", declared[0]);
  else
    what = g_strdup ("no permissions declared");
  g_object_set (subtitle, "xalign", 0.0, NULL);
  gtk_label_set_text (GTK_LABEL (subtitle), what);
  gtk_widget_add_css_class (subtitle, "dim-label");

  /* The switch IS the enabled property, two-way: the model may refuse the
     write (a refused plugin stays refused) and the switch reads back what
     the machine decided. */
  g_object_bind_property (plugin, "enabled", enable, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  gtk_box_append (GTK_BOX (column), title);
  gtk_box_append (GTK_BOX (column), subtitle);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_box_append (GTK_BOX (box), column);
  gtk_box_append (GTK_BOX (box), enable);

  if (state != CHROMIUM_PLUGIN_STATE_REFUSED &&
      chromium_plugin_get_popup_uri (plugin))
    {
      GtkWidget *action = gtk_button_new_with_label ("打开");

      g_object_set_data_full (G_OBJECT (action), "plugin-id",
                              g_strdup (chromium_plugin_get_id (plugin)),
                              g_free);
      g_signal_connect (action, "clicked", G_CALLBACK (action_clicked),
                        NULL);
      gtk_box_append (GTK_BOX (box), action);
    }

  return box;
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWidget *window = gtk_application_window_new (app);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  GtkWidget *list = gtk_list_box_new ();
  GListModel *model;
  GError *error = NULL;

  manager = chromium_session_get_plugin_manager (session);
  g_signal_connect (manager, "grant-request", G_CALLBACK (on_grant_request),
                    NULL);
  g_signal_connect (manager, "plugin-error", G_CALLBACK (on_plugin_error),
                    NULL);
  g_signal_connect (manager, "plugin-action", G_CALLBACK (on_plugin_action),
                    NULL);

  model = chromium_plugin_manager_get_plugins (manager);
  gtk_list_box_bind_model (GTK_LIST_BOX (list), model, create_row,
                           NULL, NULL);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroll),
                                              400);
  gtk_window_set_child (GTK_WINDOW (window), scroll);
  gtk_window_set_title (GTK_WINDOW (window), "Plugins");
  gtk_window_set_default_size (GTK_WINDOW (window), 520, 480);
  main_window = GTK_WINDOW (window);

  for (char **dir = (char **) data; dir && *dir; dir++)
    {
      if (!chromium_plugin_manager_add_search_path (manager, *dir, &error))
        {
          g_printerr ("plugins: %s: %s\n", *dir, error->message);
          g_clear_error (&error);
        }
    }

  if (!chromium_plugin_manager_scan (manager, &error))
    {
      g_printerr ("plugins: scan: %s\n", error ? error->message : "unknown");
      g_clear_error (&error);
    }

  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  GtkApplication *app;
  int status;

  if (argc < 2)
    {
      g_printerr ("usage: cefnest-plugins <plugin-dir> [plugin-dir...]\n");
      return 1;
    }

  session = chromium_session_new (NULL);

  app = g_object_new (GTK_TYPE_APPLICATION, "application-id",
                      "org.cefnest.Plugins", "flags",
                      G_APPLICATION_NON_UNIQUE, NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), argv + 1);
  status = g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);
  g_clear_object (&session);
  return status;
}
