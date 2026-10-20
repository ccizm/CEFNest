#define G_LOG_DOMAIN "nested"

#define _GNU_SOURCE

#include "nested-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>

typedef struct _DataOffer DataOffer;
typedef struct _DataDevice DataDevice;

/* ------------------------------------------------------------------ *
 * wl_data_device_manager                                             *
 *                                                                    *
 * Clipboard only. A drag needs a pointer-owned icon and a second      *
 * surface to drop onto, and this compositor has neither, so           *
 * start_drag is ignored: answering a drag with a protocol error would *
 * kill the client for something as ordinary as selecting text.        *
 * ------------------------------------------------------------------ */

#define NESTED_DATA_DEVICE_MANAGER_VERSION 3

/* The names a text copy travels under, best first. The X11-era ones are here
 * because clients that predate the charset suffix still mean UTF-8. */
static const char *const text_mimes[] = {
  "text/plain;charset=utf-8",
  "text/plain;charset=UTF-8",
  "UTF8_STRING",
  "TEXT",
  "text/plain",
  "STRING",
};

struct _DataSource {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  GPtrArray *mime_types;      /* char *, owned */
  struct wl_list offers;      /* DataOffer.source_link */
};

struct _DataOffer {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  DataSource *source;         /* set: forward reads to a client's source */
  struct wl_list source_link;
  struct wl_list device_link;
};

struct _DataDevice {
  struct wl_resource *resource;
  NestedCompositor *compositor;
  struct wl_list link;
  struct wl_list offers;      /* DataOffer.device_link */
};

struct _ClipboardTransfer {
  NestedCompositor *compositor;
  int fd;                 /* read end of the pipe, non-blocking */
  guint source_id;        /* the watch that drains it */
  GByteArray *data;
  char *mime;
};

/* A copy is text, not a file: past that it is a client misbehaving, and the UI
 * process must not be talked into exhausting its memory. */
#define CLIPBOARD_MAX_BYTES (4 * 1024 * 1024)

static const struct wl_data_source_interface data_source_impl;
static const struct wl_data_offer_interface data_offer_impl;
static const struct wl_data_device_interface data_device_impl;

static void data_offer_resource_destroyed (struct wl_resource *resource);
static void compositor_advertise_selection (NestedCompositor *self, DataSource *source);
static void compositor_clipboard_read_done (GObject *clipboard, GAsyncResult *result,
                                            gpointer user_data);

static gboolean
mime_is_text (const char *mime)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
    if (!strcmp (mime, text_mimes[i]))
      return TRUE;

  return g_str_has_prefix (mime, "text/");
}

/* Any text type can be served as any other: the payload is the same UTF-8 and
 * the difference is only a promise about the missing terminator. */
static gboolean
mime_can_serve (const char *have, const char *asked)
{
  if (!strcmp (have, asked))
    return TRUE;

  return mime_is_text (have) && mime_is_text (asked);
}

static const char *
pick_text_mime (GPtrArray *types)
{
  gsize i, j;

  for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
    for (j = 0; j < types->len; j++)
      if (!strcmp ((const char *) g_ptr_array_index (types, j), text_mimes[i]))
        return text_mimes[i];

  return types->len > 0 ? g_ptr_array_index (types, 0) : NULL;
}

/* ------------------------------------------------------------ one client copy */

static void
clipboard_transfer_destroy (ClipboardTransfer *transfer)
{
  NestedCompositor *self = transfer->compositor;

  if (self && self->clipboard_transfer == transfer)
    self->clipboard_transfer = NULL;

  if (transfer->source_id != 0)
    g_source_remove (transfer->source_id);
  close (transfer->fd);
  g_byte_array_unref (transfer->data);
  g_free (transfer->mime);
  g_free (transfer);
}

static void
clipboard_transfer_cancel (ClipboardTransfer *transfer)
{
  if (transfer)
    clipboard_transfer_destroy (transfer);
}

static void
clipboard_transfer_finish (ClipboardTransfer *transfer, gboolean mirror)
{
  NestedCompositor *self = transfer->compositor;
  char *text = NULL;

  if (mirror && mime_is_text (transfer->mime))
    {
      /* Some clients count the terminator in the payload. */
      while (transfer->data->len > 0 && transfer->data->data[transfer->data->len - 1] == 0)
        transfer->data->len--;

      text = g_strndup ((const char *) transfer->data->data, transfer->data->len);
      {
        char *valid = g_utf8_make_valid (text, -1);

        g_free (text);
        text = valid;
      }
    }

  if (text && self && self->clipboard && !self->clipboard_echo)
    {
      self->clipboard_echo++;
      gdk_clipboard_set_text (self->clipboard, text);
      self->clipboard_echo--;

      /* The widget clipboard and the mirror have to agree: the copying client
       * may be gone by the time another one asks, and a paste must not depend
       * on the notification for a change we made ourselves. */
      g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
      g_clear_pointer (&self->clipboard_mime, g_free);

      /* Only non-empty text is mirrored: a 0-byte mirror reads as a selection
       * to the paste path and would keep it from re-reading the clipboard. */
      if (*text)
        {
          self->clipboard_bytes = g_bytes_new (text, strlen (text));
          self->clipboard_mime = g_strdup (text_mimes[0]);
        }
    }

  g_debug ("nested: a client copied %s (%u bytes), %s", transfer->mime,
           transfer->data->len, mirror ? (text ? "the widget clipboard now holds it"
                                               : "which is not bridged")
                                        : "and it was superseded");

  g_free (text);

  /* Reached from the watch itself: returning G_SOURCE_REMOVE detaches it, so
   * only the descriptor and the buffers are ours to free. */
  transfer->source_id = 0;
  clipboard_transfer_destroy (transfer);
}

static gboolean
clipboard_transfer_on_data (int fd, GIOCondition condition, gpointer user_data)
{
  ClipboardTransfer *transfer = user_data;
  guint8 chunk[8192];

  for (;;)
    {
      ssize_t n = read (fd, chunk, sizeof chunk);

      if (n > 0)
        {
          g_byte_array_append (transfer->data, chunk, (gsize) n);
          if (transfer->data->len > CLIPBOARD_MAX_BYTES)
            {
              g_debug ("nested: a copy bigger than %u bytes: dropped",
                       CLIPBOARD_MAX_BYTES);
              clipboard_transfer_finish (transfer, FALSE);
              return G_SOURCE_REMOVE;
            }
          continue;
        }

      if (n < 0 && errno == EAGAIN)
        return G_SOURCE_CONTINUE;
      if (n < 0 && errno == EINTR)
        continue;

      /* n == 0 means the writer is done; a negative other than EAGAIN means it
       * went wrong. Either way the payload is as complete as it will get. */
      break;
    }

  /* A finished copy looks exactly like a hangup: the client closed its write
   * end. Only speak up when nothing came through. */
  if (transfer->data->len == 0)
    g_debug ("nested: a copy arrived empty");

  clipboard_transfer_finish (transfer, TRUE);
  return G_SOURCE_REMOVE;
}

static void
client_selection_transfer (NestedCompositor *self, DataSource *source, const char *mime)
{
  ClipboardTransfer *transfer;
  int fds[2];

  if (pipe2 (fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
      g_warning ("nested: cannot make a pipe for the selection: %s", g_strerror (errno));
      return;
    }

  /* libwayland dups the fd while encoding, so the write end stays ours and we
   * are the ones who must close it. */
  wl_data_source_send_send (source->resource, mime, fds[1]);
  close (fds[1]);

  transfer = g_new0 (ClipboardTransfer, 1);
  transfer->compositor = self;
  transfer->mime = g_strdup (mime);
  transfer->data = g_byte_array_new ();
  transfer->fd = fds[0];
  self->clipboard_transfer = transfer;

  transfer->source_id =
    g_unix_fd_add_full (G_PRIORITY_DEFAULT, fds[0],
                        G_IO_IN | G_IO_ERR | G_IO_HUP,
                        clipboard_transfer_on_data, transfer, NULL);
}

/* ----------------------------------------------------- serving a client paste */

typedef struct {
  GBytes *bytes;
  int fd;
} ClipboardWrite;

static void
clipboard_write_run (GTask *task, gpointer source_object, gpointer data,
                     GCancellable *cancellable)
{
  ClipboardWrite *job = data;
  const guint8 *payload;
  gsize size, offset = 0;

  payload = g_bytes_get_data (job->bytes, &size);

  /* A slow target must not stall the event loop, which is why this runs on a
   * thread and why a short write simply continues. */
  while (offset < size)
    {
      ssize_t n = write (job->fd, payload + offset, size - offset);

      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }

      offset += (gsize) n;
    }

  g_task_return_boolean (task, offset == size);
}

static void
clipboard_write_done (GObject *object, GAsyncResult *result, gpointer data)
{
  ClipboardWrite *job = data;

  if (!g_task_propagate_boolean (G_TASK (result), NULL))
    g_debug ("nested: the paste target stopped reading");

  close (job->fd);
  g_bytes_unref (job->bytes);
  g_free (job);
}

static void
clipboard_send_bytes (GBytes *bytes, int fd)
{
  ClipboardWrite *job;
  GTask *task;

  job = g_new0 (ClipboardWrite, 1);
  job->bytes = g_bytes_ref (bytes);
  job->fd = fd;

  /* The thread function is handed the task data, not the callback data, so the
   * job has to be reachable through both. */
  task = g_task_new (NULL, NULL, clipboard_write_done, job);
  g_task_set_task_data (task, job, NULL);
  g_task_run_in_thread (task, clipboard_write_run);
  g_object_unref (task);
}

/* The mirror is taken from change notifications, and a widget clipboard that
 * is busy handing ownership back can report nothing at that instant. Reading
 * again when the paste is asked for is both fresher and the only way out of
 * that state, so a stale mirror costs a round trip instead of an empty paste. */
typedef struct {
  NestedCompositor *self;
  int fd;
} ClipboardPaste;

static void
clipboard_paste_read_done (GObject *clipboard, GAsyncResult *result, gpointer data)
{
  ClipboardPaste *paste = data;
  char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard), result, NULL);

  if (text && *text)
    {
      GBytes *bytes = g_bytes_new_take (text, strlen (text));

      clipboard_send_bytes (bytes, paste->fd);
      g_bytes_unref (bytes);
    }
  else
    {
      g_free (text);
      close (paste->fd);
      g_debug ("nested: the widget clipboard had nothing to paste");
    }

  g_object_unref (paste->self);
  g_free (paste);
}

static void
clipboard_paste_later (NestedCompositor *self, int fd)
{
  ClipboardPaste *paste;

  if (!self->clipboard)
    {
      close (fd);
      return;
    }

  paste = g_new0 (ClipboardPaste, 1);
  paste->self = g_object_ref (self);
  paste->fd = fd;
  gdk_clipboard_read_text_async (self->clipboard, NULL,
                                 clipboard_paste_read_done, paste);
}

/* ------------------------------------------------------------- the offers */

static void
data_device_advertise (DataDevice *device, DataSource *source)
{
  NestedCompositor *self = device->compositor;
  struct wl_client *client = wl_resource_get_client (device->resource);
  struct wl_resource *offer_resource;
  GPtrArray *types;
  DataOffer *offer;
  gsize i;

  if (source && wl_resource_get_client (source->resource) == client)
    return;                 /* nobody is handed back their own copy */

  if (!source && !self->clipboard_bytes)
    {
      /* A device that was handed an offer has to be told there is nothing
       * now, not left holding one whose owner is gone. */
      g_debug ("nested: nothing to offer a data device");
      wl_data_device_send_selection (device->resource, NULL);
      return;
    }

  types = g_ptr_array_new ();
  if (source)
    for (i = 0; i < source->mime_types->len; i++)
      g_ptr_array_add (types, g_ptr_array_index (source->mime_types, i));
  else
    for (i = 0; i < G_N_ELEMENTS (text_mimes); i++)
      g_ptr_array_add (types, (char *) text_mimes[i]);

  if (types->len == 0)
    {
      g_ptr_array_free (types, TRUE);
      return;
    }

  offer_resource = resource_create (client, &wl_data_offer_interface,
                                    wl_resource_get_version (device->resource), 0);
  if (!offer_resource)
    {
      g_ptr_array_free (types, TRUE);
      return;
    }

  offer = g_new0 (DataOffer, 1);
  offer->resource = offer_resource;
  offer->compositor = self;
  offer->source = source;
  wl_list_init (&offer->source_link);
  wl_list_init (&offer->device_link);
  wl_list_insert (&device->offers, &offer->device_link);
  if (source)
    wl_list_insert (&source->offers, &offer->source_link);
  wl_resource_set_implementation (offer_resource, &data_offer_impl, offer,
                                  data_offer_resource_destroyed);

  /* data_offer is the event that actually creates the object on the client:
   * selection only names it, so introducing the offer comes first. */
  wl_data_device_send_data_offer (device->resource, offer_resource);

  for (i = 0; i < types->len; i++)
    wl_data_offer_send_offer (offer_resource, g_ptr_array_index (types, i));
  g_ptr_array_free (types, TRUE);

  if (wl_resource_get_version (device->resource) >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
    wl_data_offer_send_source_actions (offer_resource,
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

  wl_data_device_send_selection (device->resource, offer_resource);
  g_debug ("nested: offered the selection as id %u, version %u",
           wl_resource_get_id (offer_resource),
           wl_resource_get_version (offer_resource));
}

/* Each device gets its own offer object: the protocol has the client destroy
 * the previous one when a new selection arrives, so the compositor must not
 * retire it early or the client's destroy request lands on a dead id. */
static void
compositor_advertise_selection (NestedCompositor *self, DataSource *source)
{
  struct wl_list *link;

  for (link = self->data_devices.next; link != &self->data_devices; link = link->next)
    {
      DataDevice *device = wl_container_of (link, device, link);

      data_device_advertise (device, source);
    }
}

/* ------------------------------------------------------------ the GTK clipboard */

static void
compositor_paste_clipboard (NestedCompositor *self)
{
  const char *const *types;
  const char *mime = NULL;
  gsize n_types = 0;
  gsize i;

  if (!self->clipboard || self->clipboard_echo)
    return;

  types = gdk_content_formats_get_mime_types (gdk_clipboard_get_formats (self->clipboard),
                                              &n_types);
  for (i = 0; i < n_types && !mime; i++)
    if (mime_is_text (types[i]))
      mime = types[i];

  if (!mime)
    {
      /* Images and file lists are not bridged: the clients get told there is
       * no selection rather than a stale text one. */
      g_debug ("nested: the widget clipboard holds no text (%u formats)",
               (guint) n_types);
      g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
      g_clear_pointer (&self->clipboard_mime, g_free);
      if (!self->selection_source)
        compositor_advertise_selection (self, NULL);
      return;
    }

  gdk_clipboard_read_text_async (self->clipboard, NULL,
                                 (GAsyncReadyCallback) compositor_clipboard_read_done, self);
}

static void
compositor_clipboard_read_done (GObject *clipboard, GAsyncResult *result, gpointer user_data)
{
  NestedCompositor *self = user_data;
  GError *error = NULL;
  char *text;

  text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (clipboard), result, &error);
  if (!text || !*text)
    {
      if (error)
        g_debug ("nested: the clipboard gave no text: %s", error->message);
      g_clear_error (&error);
      g_free (text);

      /* Nothing to mirror — and an empty mirror is worse than none, because a
       * client that is offered one never reaches the read-again-on-demand path
       * that would have got the real text a moment later. */
      g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
      g_clear_pointer (&self->clipboard_mime, g_free);
      if (!self->selection_source)
        compositor_advertise_selection (self, NULL);
      return;
    }

  g_clear_pointer (&self->clipboard_bytes, g_bytes_unref);
  g_clear_pointer (&self->clipboard_mime, g_free);

  self->clipboard_bytes = g_bytes_new (text, strlen (text));
  self->clipboard_mime = g_strdup (text_mimes[0]);
  g_free (text);

  g_debug ("nested: the widget clipboard holds %u bytes of text",
           (guint) g_bytes_get_size (self->clipboard_bytes));

  /* A client that owns the selection is served from its source, not from here. */
  if (!self->selection_source)
    compositor_advertise_selection (self, NULL);
}

static void
compositor_on_clipboard_changed (GObject *clipboard, GParamSpec *pspec, gpointer data)
{
  compositor_paste_clipboard (data);
}

/* ------------------------------------------------------------- wl_data_offer */

static void
data_offer_accept (struct wl_client *client, struct wl_resource *resource,
                   uint32_t serial, const char *mime_type)
{
  /* Only drags use it, and there are no drags here. */
}

static void
data_offer_receive (struct wl_client *client, struct wl_resource *resource,
                    const char *mime_type, int32_t fd)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  NestedCompositor *self = offer->compositor;

  if (offer->source)
    {
      /* The owner of the selection writes into the fd the requester made;
       * libwayland dups it, so our copy can go. */
      g_debug ("nested: passing %s to the client that copied it", mime_type);
      wl_data_source_send_send (offer->source->resource, mime_type, fd);
      close (fd);
      return;
    }

  if (self->clipboard_mime && !mime_can_serve (self->clipboard_mime, mime_type))
    {
      g_debug ("nested: %s asked for, the clipboard holds %s", mime_type,
               self->clipboard_mime);
      close (fd);
      return;
    }

  if (!self->clipboard_bytes)
    {
      g_debug ("nested: the mirror is empty, reading the clipboard for the paste");
      clipboard_paste_later (self, fd);
      return;
    }

  g_debug ("nested: pasting %u bytes of the widget clipboard",
           (guint) g_bytes_get_size (self->clipboard_bytes));
  clipboard_send_bytes (self->clipboard_bytes, fd);
}

static void
data_offer_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_offer_resource_destroyed (struct wl_resource *resource)
{
  DataOffer *offer = wl_resource_get_user_data (resource);

  wl_list_remove (&offer->device_link);
  if (offer->source)
    wl_list_remove (&offer->source_link);
  g_free (offer);
}

static const struct wl_data_offer_interface data_offer_impl = {
  .accept = data_offer_accept,
  .receive = data_offer_receive,
  .destroy = data_offer_destroy,
};

/* ------------------------------------------------------------ wl_data_source */

static void
data_source_offer (struct wl_client *client, struct wl_resource *resource,
                   const char *mime_type)
{
  DataSource *source = wl_resource_get_user_data (resource);
  gsize i;

  for (i = 0; i < source->mime_types->len; i++)
    if (!strcmp (g_ptr_array_index (source->mime_types, i), mime_type))
      return;

  g_ptr_array_add (source->mime_types, g_strdup (mime_type));
}

static void
data_source_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_source_resource_destroyed (struct wl_resource *resource)
{
  DataSource *source = wl_resource_get_user_data (resource);
  NestedCompositor *self = source->compositor;

  /* The offers outlive the source: their clients still hold them and will
   * destroy them themselves. Detached, they fall back to the widget
   * clipboard, which still holds what was copied. */
  while (!wl_list_empty (&source->offers))
    {
      DataOffer *offer = wl_container_of (source->offers.next, offer, source_link);

      wl_list_remove (&offer->source_link);
      wl_list_init (&offer->source_link);
      offer->source = NULL;
    }

  if (self->selection_source == source)
    {
      self->selection_source = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);

      /* The copying client is gone: the widget clipboard is the selection
       * again, and whoever is still connected has to be told. */
      compositor_advertise_selection (self, NULL);
    }

  g_clear_pointer (&source->mime_types, g_ptr_array_unref);
  g_free (source);
}

static const struct wl_data_source_interface data_source_impl = {
  .offer = data_source_offer,
  .destroy = data_source_destroy,
};

/* ----------------------------------------------------------- wl_data_device */

static void
data_device_start_drag (struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *source, struct wl_resource *origin,
                        struct wl_resource *icon, uint32_t serial)
{
  g_debug ("nested: ignoring a drag: a surface inside a widget has nowhere to drop");
}

static void
data_device_set_selection (struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *source_resource, uint32_t serial)
{
  NestedCompositor *self =
    ((DataDevice *) wl_resource_get_user_data (resource))->compositor;
  DataSource *source = source_resource ? wl_resource_get_user_data (source_resource) : NULL;
  const char *mime;

  if (self->selection_source && self->selection_source != source)
    wl_data_source_send_cancelled (self->selection_source->resource);

  clipboard_transfer_cancel (self->clipboard_transfer);
  self->selection_source = source;

  if (!source)
    {
      compositor_advertise_selection (self, NULL);
      return;
    }

  compositor_advertise_selection (self, source);

  mime = pick_text_mime (source->mime_types);
  if (mime)
    client_selection_transfer (self, source, mime);
  else
    g_debug ("nested: a client took a selection offering nothing we can bridge");
}

static void
data_device_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
data_device_resource_destroyed (struct wl_resource *resource)
{
  DataDevice *device = wl_resource_get_user_data (resource);

  while (!wl_list_empty (&device->offers))
    {
      DataOffer *offer = wl_container_of (device->offers.next, offer, device_link);

      wl_resource_destroy (offer->resource);
    }

  wl_list_remove (&device->link);
  g_free (device);
}

static const struct wl_data_device_interface data_device_impl = {
  .start_drag = data_device_start_drag,
  .set_selection = data_device_set_selection,
  .release = data_device_release,
};

/* ------------------------------------------------------ wl_data_device_manager */

static void
data_device_manager_create_data_source (struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *source_resource;
  DataSource *source;

  source = g_new0 (DataSource, 1);
  source->compositor = self;
  source->mime_types = g_ptr_array_new_with_free_func (g_free);
  wl_list_init (&source->offers);

  source_resource = resource_create (client, &wl_data_source_interface,
                                     wl_resource_get_version (resource), id);
  if (!source_resource)
    {
      g_clear_pointer (&source->mime_types, g_ptr_array_unref);
      g_free (source);
      return;
    }

  source->resource = source_resource;
  wl_resource_set_implementation (source_resource, &data_source_impl, source,
                                  data_source_resource_destroyed);
}

static void
data_device_manager_get_data_device (struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id, struct wl_resource *seat)
{
  NestedCompositor *self = wl_resource_get_user_data (resource);
  struct wl_resource *device_resource;
  DataDevice *device;

  device = g_new0 (DataDevice, 1);
  device->compositor = self;
  wl_list_init (&device->offers);

  device_resource = resource_create (client, &wl_data_device_interface,
                                     wl_resource_get_version (resource), id);
  if (!device_resource)
    {
      g_free (device);
      return;
    }

  device->resource = device_resource;
  wl_resource_set_implementation (device_resource, &data_device_impl, device,
                                  data_device_resource_destroyed);
  wl_list_insert (&self->data_devices, &device->link);

  /* A client that starts after the copy still has to be able to paste, and the
   * mirror may not have been taken while there was nothing to take. */
  if (!self->selection_source && !self->clipboard_bytes)
    compositor_paste_clipboard (self);
  else
    data_device_advertise (device, self->selection_source);
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
  .create_data_source = data_device_manager_create_data_source,
  .get_data_device = data_device_manager_get_data_device,
};

void
data_device_manager_bind (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = resource_create (client, &wl_data_device_manager_interface, version, id);
  if (!resource)
    return;

  wl_resource_set_implementation (resource, &data_device_manager_impl, data, NULL);
}

void
nested_compositor_set_clipboard (NestedCompositor *self, GdkClipboard *clipboard)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (self->clipboard == clipboard)
    return;

  if (self->clipboard)
    {
      if (self->clipboard_handler != 0)
        g_signal_handler_disconnect (self->clipboard, self->clipboard_handler);
      self->clipboard_handler = 0;
      g_clear_object (&self->clipboard);
    }

  if (!clipboard)
    return;

  self->clipboard = g_object_ref (clipboard);
  self->clipboard_handler =
    g_signal_connect (self->clipboard, "notify::content",
                      G_CALLBACK (compositor_on_clipboard_changed), self);

  /* Whatever is already on the clipboard becomes available to clients. */
  compositor_paste_clipboard (self);
}

/* Dispose path: the transfer's watch fires on the main loop, so it must not
 * reach for an object that is going away. */
void
data_device_shutdown (NestedCompositor *self)
{
  if (self->clipboard_transfer)
    {
      self->clipboard_transfer->compositor = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);
    }
}
