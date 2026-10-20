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
 * Two relays over one protocol. A copy moves text between the widget  *
 * clipboard and a client. A drag is the same relay with a pointer on  *
 * it, and here the two ends of it are usually the same client: an     *
 * HTML5 drag starts and finishes inside one page, so the payload      *
 * leaves Blink through wl_data_source.send and comes back into it     *
 * through wl_data_offer.receive.                                     *
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
  uint32_t dnd_actions;       /* the mask wl_data_source.set_actions named */
  /* A source with no resource is the compositor's own: the host picked a drag
   * up somewhere outside the window and is carrying it in, so the bytes are
   * already here instead of being asked for over a pipe. */
  GBytes *payload;
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

/* One drag in progress. There is only ever one of these: a widget has one
 * pointer, and a second wl_data_device.start_drag before the first has
 * concluded means the client lost track of it.
 *
 * The destination half is per-surface, the way the protocol makes it: entering
 * a surface introduces a wl_data_offer for the client to read, and leaving one
 * ends that. The drag itself outlives the pointer's hold on it — after the drop
 * the destination is still fetching the payload and still has to say finish —
 * so owns_pointer, not the existence of this object, is what tells the seat to
 * keep wl_pointer away from the surfaces underneath. */
struct _Drag {
  NestedCompositor *compositor;
  DataSource *source;              /* not owned; NULL is a drag with no payload */
  DataDevice *origin_device;       /* the wl_data_device that asked for the drag */
  NestedSurface *origin;
  NestedSurface *icon;
  uint32_t serial;                 /* start_drag's implicit grab, kept for the log */
  uint32_t source_actions;
  uint32_t dest_actions;           /* what the surface under it supports */
  uint32_t preferred_action;       /* what it would rather do */
  uint32_t chosen_action;
  DataDevice *dest_device;         /* only while a surface is entered */
  struct wl_resource *dest_offer;   /* ditto, and the client's to destroy */
  NestedSurface *dest_surface;
  char *accepted_mime;
  unsigned int owns_pointer : 1;
  unsigned int refused : 1;         /* the destination accepted no mime type */
  unsigned int dropped : 1;         /* wl_data_device.drop went out */
  unsigned int concluded : 1;       /* cancelled or dnd_finished went out */
  unsigned int handing_out : 1;     /* the host is being given its payload */
};

/* Where a drained payload goes when it is not a copy. NULL @bytes means the
 * client answered nothing, which is the same as not having asked. */
typedef void (*TransferFinish) (NestedCompositor *self, const char *mime,
                                GBytes *bytes);

struct _ClipboardTransfer {
  NestedCompositor *compositor;
  int fd;                 /* read end of the pipe, non-blocking */
  guint source_id;        /* the watch that drains it */
  GByteArray *data;
  char *mime;
  /* Which of the compositor's two slots this transfer holds, so finishing one
   * cannot leave the other pointing at freed memory. */
  ClipboardTransfer **slot;
  /* Where the payload goes. NULL is a copy: it mirrors onto the widget
   * clipboard. A drag leaving the page has somewhere else to go. */
  TransferFinish finish;
};

/* A copy is text, not a file: past that it is a client misbehaving, and the UI
 * process must not be talked into exhausting its memory. */
#define CLIPBOARD_MAX_BYTES (4 * 1024 * 1024)

static const struct wl_data_source_interface data_source_impl;
static const struct wl_data_offer_interface data_offer_impl;
static const struct wl_data_device_interface data_device_impl;

static void data_offer_resource_destroyed (struct wl_resource *resource);
static void compositor_advertise_selection (NestedCompositor *self);
static void compositor_clipboard_read_done (GObject *clipboard, GAsyncResult *result,
                                            gpointer user_data);

static void drag_cancel (Drag *drag);
static void drag_destroy (Drag *drag, gboolean tell_host);
static void drag_leave (Drag *drag);
static void drag_enter (Drag *drag, DataDevice *device, NestedSurface *surface);
static void drag_negotiate (Drag *drag);

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

/* Whether the bytes of this source are the compositor's own: a drag the host
 * picked up outside the window and is carrying in has no client on the source
 * side, so nothing may be sent to it and everything asked for is answered from
 * memory. */
static gboolean
data_source_is_host (const DataSource *source)
{
  return source->resource == NULL;
}

/* ------------------------------------------------------------ one client copy */

static void
clipboard_transfer_destroy (ClipboardTransfer *transfer)
{
  if (transfer->slot && *transfer->slot == transfer)
    *transfer->slot = NULL;

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

  if (transfer->finish)
    {
      TransferFinish finish = transfer->finish;
      char *mime = g_strdup (transfer->mime);
      GBytes *bytes = NULL;

      /* Destroyed before the handler runs, and not cancelled by it: the handler
       * calls the page's drag off, and a drag going away takes its unfinished
       * read with it — this one has already been taken off the slot. */
      if (mirror)
        bytes = g_bytes_new (transfer->data->data, transfer->data->len);

      transfer->source_id = 0;
      clipboard_transfer_destroy (transfer);
      finish (self, mime, bytes);
      if (bytes)
        g_bytes_unref (bytes);
      g_free (mime);
      return;
    }

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
              g_debug ("nested: a payload bigger than %u bytes: dropped",
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
    g_debug ("nested: a payload arrived empty");

  clipboard_transfer_finish (transfer, TRUE);
  return G_SOURCE_REMOVE;
}

static void
clipboard_transfer_start (NestedCompositor *self, DataSource *source, const char *mime,
                          TransferFinish finish, ClipboardTransfer **slot)
{
  ClipboardTransfer *transfer;
  int fds[2];

  if (pipe2 (fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
      g_warning ("nested: cannot make a pipe for %s: %s",
                 finish ? "a drag taken out of the page" : "the selection",
                 g_strerror (errno));
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
  transfer->finish = finish;
  transfer->slot = slot;
  *slot = transfer;

  transfer->source_id =
    g_unix_fd_add_full (G_PRIORITY_DEFAULT, fds[0],
                        G_IO_IN | G_IO_ERR | G_IO_HUP,
                        clipboard_transfer_on_data, transfer, NULL);
}

static void
client_selection_transfer (NestedCompositor *self, DataSource *source, const char *mime)
{
  clipboard_transfer_start (self, source, mime, NULL, &self->clipboard_transfer);
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

/* Introduce an offer object to a device's client and list the types it carries.
 * The caller names it right afterwards — wl_data_device.selection for a copy,
 * wl_data_device.enter for a drag — because neither can refer to an object the
 * client has not been given yet. */
static struct wl_resource *
data_offer_introduce (DataDevice *device, DataSource *source, GPtrArray *types)
{
  struct wl_client *client = wl_resource_get_client (device->resource);
  struct wl_resource *offer_resource;
  DataOffer *offer;
  gsize i;

  if (types->len == 0)
    return NULL;

  offer_resource = resource_create (client, &wl_data_offer_interface,
                                    wl_resource_get_version (device->resource), 0);
  if (!offer_resource)
    return NULL;

  offer = g_new0 (DataOffer, 1);
  offer->resource = offer_resource;
  offer->compositor = device->compositor;
  offer->source = source;
  wl_list_init (&offer->source_link);
  wl_list_init (&offer->device_link);
  wl_list_insert (&device->offers, &offer->device_link);
  if (source)
    wl_list_insert (&source->offers, &offer->source_link);
  wl_resource_set_implementation (offer_resource, &data_offer_impl, offer,
                                  data_offer_resource_destroyed);

  wl_data_device_send_data_offer (device->resource, offer_resource);

  for (i = 0; i < types->len; i++)
    wl_data_offer_send_offer (offer_resource, g_ptr_array_index (types, i));

  return offer_resource;
}

static void
data_device_advertise (DataDevice *device, DataSource *source)
{
  NestedCompositor *self = device->compositor;
  struct wl_resource *offer_resource;
  GPtrArray *types;
  gsize i;

  if (source && wl_resource_get_client (source->resource) ==
      wl_resource_get_client (device->resource))
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

  offer_resource = data_offer_introduce (device, source, types);
  g_ptr_array_free (types, TRUE);
  if (!offer_resource)
    return;

  /* The actions a copy could be used for. There is no drag to move anything
   * with it and nothing to ask about, so it is only ever copied. */
  if (wl_resource_get_version (device->resource) >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
    wl_data_offer_send_source_actions (offer_resource,
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

  wl_data_device_send_selection (device->resource, offer_resource);
  g_debug ("nested: offered the selection as id %u, version %u",
           wl_resource_get_id (offer_resource),
           wl_resource_get_version (offer_resource));
}

static gboolean
compositor_replay_selection (gpointer data)
{
  NestedCompositor *self = data;
  struct wl_list *link;

  self->selection_replay_id = 0;

  for (link = self->data_devices.next; link != &self->data_devices; link = link->next)
    {
      DataDevice *device = wl_container_of (link, device, link);

      data_device_advertise (device, self->selection_source);
    }

  return G_SOURCE_REMOVE;
}

/* Advertise from an idle, never from the middle of handling a client request:
 * a GTK client asks for its data device while still inside gtk_init(), before
 * GDK has a default display, and the clipboard it builds for the offer asks GDK
 * for one — gdk_display_should_use_portal() asserts on the NULL and aborts the
 * client. One loop later it has already had its roundtrip reply, so the offer
 * arrives in a dispatch where the display exists. Reading selection_source at
 * that point is the point: several requests in one batch coalesce into the
 * latest state. */
static void
compositor_advertise_selection (NestedCompositor *self)
{
  if (self->selection_replay_id)
    return;

  self->selection_replay_id = g_idle_add (compositor_replay_selection, self);
}

/* Each device gets its own offer object: the protocol has the client destroy
 * the previous one when a new selection arrives, so the compositor must not
 * retire it early or the client's destroy request lands on a dead id. */

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
        compositor_advertise_selection (self);
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
        compositor_advertise_selection (self);
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
    compositor_advertise_selection (self);
}

static void
compositor_on_clipboard_changed (GObject *clipboard, GParamSpec *pspec, gpointer data)
{
  compositor_paste_clipboard (data);
}

/* ------------------------------------------------------------------ *
 * The drag                                                           *
 *                                                                    *
 * A drag is a copy that the pointer carries. Where the payload comes   *
 * from does not change — it is still a wl_data_source asking its       *
 * client for bytes over a pipe — but who is told about it does: the    *
 * destination is whichever surface the cursor is over, it is introduced *
 * to a wl_data_offer of its own, and the seat stays out of it: while    *
 * a drag holds the pointer, nothing goes to wl_pointer at all.          *
 * ------------------------------------------------------------------ */

/* What a source can do when it never said: everything the protocol names.
 * Blink always says, but a client that does not is not refusing. */
#define DRAG_ALL_ACTIONS (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | \
                          WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | \
                          WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

static void
drag_update_destination (Drag *drag);

static int
round_to_pixel (double value)
{
  return (int) (value >= 0.0 ? value + 0.5 : value - 0.5);
}

/* The pointer's position in the destination surface's own pixels, which is what
 * wl_data_device.enter and .motion are measured in — the same subtraction
 * wl_pointer.motion uses, because a drag icon is not part of the surface it is
 * over. */
static void
drag_local (NestedCompositor *self, NestedSurface *surface, double *x, double *y)
{
  int left = 0, above = 0;

  nested_surface_get_position (surface, &left, &above);

  *x = self->pointer_x - left;
  *y = self->pointer_y - above;
}

/* The data device of the client that owns a surface: a drag is handed to a
 * surface, and the offer it is given has to live on that client's own device. */
static DataDevice *
device_for_surface (NestedCompositor *self, NestedSurface *surface)
{
  struct wl_client *client;
  DataDevice *device;

  if (!surface->wl_surface)
    return NULL;

  client = wl_resource_get_client (surface->wl_surface);

  wl_list_for_each (device, &self->data_devices, link)
    if (wl_resource_get_client (device->resource) == client)
      return device;

  return NULL;
}

/* The offers outlive the source they were made from: a destination still holding
 * one has to be left with an offer that reads as gone rather than one pointing
 * at freed memory. */
static void
data_source_offers_detach (DataSource *source)
{
  while (!wl_list_empty (&source->offers))
    {
      DataOffer *offer = wl_container_of (source->offers.next, offer, source_link);

      wl_list_remove (&offer->source_link);
      wl_list_init (&offer->source_link);
      offer->source = NULL;
    }
}

/* The source of a drag the host is carrying in: no client is on this end, the
 * payload is already in memory, and the mime list is whatever the host's own
 * drop offered. */
static DataSource *
data_source_host_new (NestedCompositor *self, const char *const *mime_types,
                      GBytes *payload)
{
  DataSource *source;
  gsize i;

  source = g_new0 (DataSource, 1);
  source->compositor = self;
  source->mime_types = g_ptr_array_new_with_free_func (g_free);
  source->payload = g_bytes_ref (payload);
  wl_list_init (&source->offers);

  for (i = 0; mime_types[i]; i++)
    g_ptr_array_add (source->mime_types, g_strdup (mime_types[i]));

  return source;
}

/* Which of the host's names can answer for @asked. A different promise about a
 * terminator is not a different payload, which is the licence the clipboard
 * mirror already takes. */
static const char *
data_source_host_pick (const DataSource *source, const char *asked)
{
  gsize i;

  for (i = 0; i < source->mime_types->len; i++)
    {
      const char *have = g_ptr_array_index (source->mime_types, i);

      if (mime_can_serve (have, asked))
        return have;
    }

  return NULL;
}

static void
data_source_host_destroy (DataSource *source)
{
  data_source_offers_detach (source);
  g_clear_pointer (&source->mime_types, g_ptr_array_unref);
  g_clear_pointer (&source->payload, g_bytes_unref);
  g_free (source);
}

/* The live drag this offer was made for, or NULL for a copy's offer and for one
 * whose drag has already been called off. */
static Drag *
drag_for_offer (DataOffer *offer)
{
  Drag *drag = offer->compositor->drag;

  /* After the drop the destination keeps reading from the offer it was handed,
   * so the drag still owns it even though it no longer holds a surface. */
  return drag && drag->dest_offer == offer->resource ? drag : NULL;
}

/* The action both ends can carry out. A destination's preference comes first,
 * because that is the one its own code is ready to do; after that the protocol's
 * recommended default, the first match in bit order. */
static uint32_t
drag_pick_action (uint32_t source_actions, uint32_t dest_actions,
                  uint32_t preferred_action)
{
  static const uint32_t order[] = {
    WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
    WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
    WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK,
  };
  uint32_t both = source_actions & dest_actions;
  gsize i;

  if (preferred_action != 0 && (both & preferred_action) == preferred_action)
    return preferred_action;

  for (i = 0; i < G_N_ELEMENTS (order); i++)
    if (both & order[i])
      return order[i];

  return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

static void
drag_negotiate (Drag *drag)
{
  uint32_t action = drag_pick_action (drag->source_actions, drag->dest_actions,
                                      drag->preferred_action);

  if (action == drag->chosen_action)
    return;

  drag->chosen_action = action;
  g_debug ("nested: drag settled on action 0x%x of source 0x%x and destination 0x%x",
           action, drag->source_actions, drag->dest_actions);

  if (drag->source && !data_source_is_host (drag->source) &&
      wl_resource_get_version (drag->source->resource) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION)
    wl_data_source_send_action (drag->source->resource, action);

  /* "This event will no longer be emitted after wl_data_device.drop": the
   * destination honors the last one it had. */
  if (!drag->dropped && drag->dest_offer &&
      wl_resource_get_version (drag->dest_offer) >= WL_DATA_OFFER_ACTION_SINCE_VERSION)
    wl_data_offer_send_action (drag->dest_offer, action);
}

static void
drag_leave (Drag *drag)
{
  /* Only the surface is let go: the offer it was introduced with is the
   * client's to destroy, and the protocol says it does at this very point. */
  if (drag->dest_device && drag->dest_surface && drag->dest_surface->wl_surface)
    wl_data_device_send_leave (drag->dest_device->resource);

  drag->dest_device = NULL;
  drag->dest_offer = NULL;
  g_clear_object (&drag->dest_surface);
  g_clear_pointer (&drag->accepted_mime, g_free);
  drag->refused = FALSE;
  drag->dest_actions = 0;
  drag->preferred_action = 0;
  drag->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

static void
drag_enter (Drag *drag, DataDevice *device, NestedSurface *surface)
{
  NestedCompositor *self = drag->compositor;
  struct wl_resource *offer_resource = NULL;
  GPtrArray *types;
  double x, y;
  gsize i;

  drag->dest_device = device;
  g_set_object (&drag->dest_surface, surface);

  /* Every surface is asked again: what the last one would take says nothing
   * about this one, and a drag that crosses a menu has to be renegotiated. */
  drag->accepted_mime = NULL;
  drag->refused = FALSE;
  drag->dest_actions = 0;
  drag->preferred_action = 0;
  drag->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;

  if (drag->source)
    {
      types = g_ptr_array_new ();
      for (i = 0; i < drag->source->mime_types->len; i++)
        g_ptr_array_add (types, g_ptr_array_index (drag->source->mime_types, i));

      /* Where a copy refuses to hand a client its own text back, a drag has to:
       * an HTML5 drag has one client on both ends of it by design, and the page
       * cannot find out what it is dropping onto unless it is given the offer. */
      offer_resource = data_offer_introduce (device, drag->source, types);
      g_ptr_array_free (types, TRUE);

      if (offer_resource &&
          wl_resource_get_version (offer_resource) >=
          WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
        wl_data_offer_send_source_actions (offer_resource, drag->source_actions);
    }

  drag->dest_offer = offer_resource;

  drag_local (self, surface, &x, &y);
  wl_data_device_send_enter (device->resource, nested_next_serial (self),
                             surface->wl_surface, wl_fixed_from_double (x),
                             wl_fixed_from_double (y), offer_resource);
  g_debug ("nested: dragged onto surface id %u offering %s, id %u",
           wl_resource_get_id (surface->wl_surface),
           drag->source ? (data_source_is_host (drag->source)
                           ? "the host's payload" : "a client's payload")
                        : "nothing (the drag has no source)",
           offer_resource ? wl_resource_get_id (offer_resource) : 0u);
}

/* The surface under the pointer is the destination: a new one gets an enter and
 * an offer, the one being left gets a leave, and the one already inside is told
 * where within it the pointer now is. */
static void
drag_update_destination (Drag *drag)
{
  NestedCompositor *self = drag->compositor;
  NestedSurface *target;
  DataDevice *device;
  double x, y;

  target = nested_compositor_surface_at (self, self->pointer_x, self->pointer_y);
  if (target && !target->wl_surface)
    target = NULL;

  if (target != drag->dest_surface)
    {
      if (drag->dest_surface)
        drag_leave (drag);

      if (!target)
        return;

      device = device_for_surface (self, target);
      if (!device)
        return;    /* its client never asked for a data device, so there is
                    * nothing to hand the payload to and no offer to make */

      drag_enter (drag, device, target);
      return;
    }

  if (!drag->dest_device || !drag->dest_surface)
    return;

  drag_local (self, drag->dest_surface, &x, &y);
  wl_data_device_send_motion (drag->dest_device->resource, nested_now_msec (),
                              wl_fixed_from_double (x), wl_fixed_from_double (y));
}

/* @tell_host is FALSE only for the one caller that is itself making a new
 * inbound drag: telling the host about the old verdict from there would let it
 * ask for a second drag while the first is still being put together. */
static void
drag_destroy (Drag *drag, gboolean tell_host)
{
  NestedCompositor *self = drag->compositor;
  gboolean from_host = drag->source && data_source_is_host (drag->source);
  /* The destination's own words are the only verdict worth passing on. A page was
   * measured reading a host drag's payload the moment it was entered and then
   * letting the drop go with nothing done to it, so a read says nothing about
   * taking; finish does, because it is only sent for a drop that was kept. */
  gboolean taken = drag->dropped && drag->concluded;
  char *mime = g_strdup (drag->accepted_mime);

  if (self->drag == drag)
    self->drag = NULL;

  /* A payload still being read for the host belongs to this drag: with the drag
   * gone there is nobody to hand it to, and the source it came from may already
   * be on its way out. */
  clipboard_transfer_cancel (self->drag_out_transfer);

  /* A payload the host handed over came with this drag and goes with it. A
   * client's source is the client's to destroy, and this is not the place. */
  if (from_host)
    {
      DataSource *source = drag->source;

      drag->source = NULL;
      data_source_host_destroy (source);
    }

  /* The icon and the surface it started from outlive the drag: their client
   * destroys them, and a surface that was an icon is not one any more. */
  if (drag->icon)
    drag->icon->is_drag_icon = FALSE;
  g_clear_object (&drag->icon);
  g_clear_object (&drag->origin);
  g_clear_object (&drag->dest_surface);
  g_clear_pointer (&drag->accepted_mime, g_free);
  g_free (drag);

  /* Told last: a host that reacts by bringing another drag in must not find the
   * one it is being told about still the live one. */
  if (from_host && tell_host)
    nested_emit_drag_in_end (self, taken, mime);
  g_free (mime);
}

static void
drag_cancel (Drag *drag)
{
  NestedCompositor *self = drag->compositor;

  if (drag->owns_pointer)
    drag_leave (drag);

  if (drag->source && !data_source_is_host (drag->source) && !drag->concluded)
    {
      drag->concluded = TRUE;
      wl_data_source_send_cancelled (drag->source->resource);
    }

  drag_destroy (drag, TRUE);

  /* Whatever the pointer was over when the drag gave up is news to the page. */
  nested_surfaces_changed (self);
}

/* The pointer goes back to the seat. The drag itself stays alive: the
 * destination is still reading the payload through the offer it was given, and
 * still has to say whether it is done with it. */
static void
drag_release_pointer (Drag *drag)
{
  drag->owns_pointer = FALSE;
  drag->dest_device = NULL;
  g_clear_object (&drag->dest_surface);
}

static void
drag_drop (Drag *drag)
{
  NestedCompositor *self = drag->compositor;

  /* The protocol's three reasons to call a drag off: it ended over no surface,
   * over one that accepted no mime type, or over one that chose no action. The
   * last two are only known once the destination has negotiated at all — a
   * client that says nothing on entering is taken as willing, because a page
   * that does not want the drop says so itself by never handling the event. */
  if (!drag->dest_surface || !drag->dest_device || drag->refused ||
      (drag->dest_actions != 0 &&
       drag->chosen_action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE))
    {
      g_debug ("nested: a drag ended where nothing would take its payload");
      drag_cancel (drag);
      return;
    }

  g_debug ("nested: dropped %s as action 0x%x",
           drag->accepted_mime ? drag->accepted_mime : "a drag",
           drag->chosen_action);

  /* The destination reads what it is being given after this, and answers with
   * wl_data_offer.finish once it has. */
  wl_data_device_send_drop (drag->dest_device->resource);
  drag->dropped = TRUE;
  drag_release_pointer (drag);

  if (drag->source && !data_source_is_host (drag->source) &&
      wl_resource_get_version (drag->source->resource) >=
      WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
    wl_data_source_send_dnd_drop_performed (drag->source->resource);

  nested_surfaces_changed (self);
}

/* ------------------------------------------------------------------ *
 * The drag, seen from the seat and the drawing                       *
 * ------------------------------------------------------------------ */

gboolean
nested_compositor_drag_active (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);

  return self->drag && self->drag->owns_pointer;
}

/* ------------------------------------------------------------ a drag leaving */

/* A payload read out of a drag is not a copy: nothing here touches the widget
 * clipboard, because the host is the one that has to decide what a drag leaving
 * the window means. The page's drag is called off as soon as the read ends —
 * whether or not anything came back — because its destination is not a surface
 * of ours and keeping it alive would leave a drag nobody can finish. */
static void
drag_out_finish (NestedCompositor *self, const char *mime, GBytes *bytes)
{
  char **types = nested_compositor_drag_get_types (self);
  gsize size = bytes ? g_bytes_get_size (bytes) : 0;

  /* The types are copied first: the client owns the source, and a drag it has
   * been told is cancelled is one it may destroy at any moment. */
  if (self->drag)
    drag_cancel (self->drag);

  if (size == 0)
    {
      g_debug ("nested: the drag left the page and nothing came back for %s", mime);
      g_strfreev (types);
      return;
    }

  g_debug ("nested: the drag left the page as %s (%" G_GSIZE_FORMAT " bytes, %u types)",
           mime, size, (unsigned) g_strv_length (types));

  nested_emit_drag_out (self, mime, bytes, types);
  g_strfreev (types);
}

char **
nested_compositor_drag_get_types (NestedCompositor *self)
{
  char **types;
  gsize i;

  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);

  if (!self->drag || !self->drag->source)
    return g_new0 (char *, 1);

  types = g_new0 (char *, self->drag->source->mime_types->len + 1);
  for (i = 0; i < self->drag->source->mime_types->len; i++)
    types[i] = g_strdup (g_ptr_array_index (self->drag->source->mime_types, i));

  return types;
}

gboolean
nested_compositor_drag_take_out (NestedCompositor *self)
{
  DataSource *source;
  const char *mime;

  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);

  if (!self->drag || !self->drag->owns_pointer)
    return FALSE;

  if (self->drag->handing_out)
    return FALSE;

  if (self->drag->source && data_source_is_host (self->drag->source))
    {
      /* The bytes are the host's own already, so there is nothing to read out of
       * a client here: the pointer walking off the page is a leave, and the drag
       * stays where it is for the pointer to come back with it. */
      return FALSE;
    }

  source = self->drag->source;
  if (!source || source->mime_types->len == 0)
    {
      g_debug ("nested: the drag left the page carrying nothing that could be taken");
      drag_cancel (self->drag);
      return FALSE;
    }

  self->drag->handing_out = TRUE;

  /* One type, not the list: this is the same single read a destination makes
   * after wl_data_offer.accept, and four representations of one thing are still
   * one payload. Asking before telling would be the other way round — a source
   * that has not been told which type is wanted is a source that has no reason
   * to answer. */
  mime = pick_text_mime (source->mime_types);
  wl_data_source_send_target (source->resource, mime);
  clipboard_transfer_start (self, source, mime, drag_out_finish,
                            &self->drag_out_transfer);

  g_debug ("nested: the pointer left the page mid-drag: taking %s of %u types",
           mime, source->mime_types->len);

  return TRUE;
}

/* ------------------------------------------------------------- a drag arriving */

/* A drag the host picked up outside this window and is now carrying over the
 * widget. Nothing about it came from a client: the bytes are already in this
 * process, and the one thing missing is that no client asked for them. So the
 * compositor becomes the source, introduces an offer to whichever surface the
 * pointer is over, and the stage-1 negotiation runs the other way round.
 *
 * Motion afterwards goes through nested_compositor_notify_pointer_motion(),
 * which is already the path that hands a live drag to the surface under the
 * pointer instead of the seat; the end comes from one of the two calls below. */
gboolean
nested_compositor_drag_from_host (NestedCompositor *self,
                                  const char *const *mime_types,
                                  GBytes *payload,
                                  gboolean can_move)
{
  DataSource *source;
  Drag *drag;
  gsize n_types = 0;

  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);
  g_return_val_if_fail (mime_types != NULL, FALSE);
  g_return_val_if_fail (payload != NULL, FALSE);

  if (mime_types[0] == NULL || g_bytes_get_size (payload) == 0)
    {
      g_debug ("nested: the host offered a drag with nothing in it");
      return FALSE;
    }

  if (self->drag)
    {
      if (!self->drag->owns_pointer)
        {
          /* Dropped and never finished: the destination went quiet after the
           * drop instead of saying what it did with the payload. The host has a
           * new drag in hand, so the old one is retired rather than left to
           * block it. */
          g_debug ("nested: the drag that was dropped never said it was done");
          drag_destroy (self->drag, FALSE);
        }
      else
        {
          g_debug ("nested: a drag already has the pointer, so the host cannot bring in a second");
          return FALSE;
        }
    }

  while (mime_types[n_types])
    n_types++;

  source = data_source_host_new (self, mime_types, payload);

  drag = g_new0 (Drag, 1);
  drag->compositor = self;
  drag->source = source;
  drag->owns_pointer = TRUE;
  drag->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;

  /* What the host's own drag is prepared to do. A drop of text out of another
   * application is nearly always a copy, and MOVE only when the source said the
   * original may go. */
  drag->source_actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                         WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
  if (can_move)
    drag->source_actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

  self->drag = drag;

  g_debug ("nested: the host brought a drag in: %" G_GSIZE_FORMAT " bytes of %u type%s",
           g_bytes_get_size (payload), (guint) n_types, n_types == 1 ? "" : "s");

  /* The pointer is already over something when the host's drag arrives, and a
   * surface is only told about a drag by being entered: without this the page
   * never fires a dragenter and its handlers stay shut however far it moves. */
  drag_update_destination (drag);
  nested_surfaces_changed (self);
  nested_flush (self);

  return TRUE;
}

/* The host let go over the page. What the destination does with the payload
 * after this is its own: it reads it through the offer it was given and answers
 * with wl_data_offer.finish, and that is what retires the drag. */
void
nested_compositor_drag_from_host_drop (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (!self->drag || !self->drag->source ||
      !data_source_is_host (self->drag->source))
    {
      g_debug ("nested: the host let go, and no drag of its was in flight");
      return;
    }

  drag_drop (self->drag);
  nested_flush (self);
}

/* The host's drag ended somewhere that is not this window, or was called off
 * halfway. The page gets the leave its surface is owed and no drop. */
void
nested_compositor_drag_from_host_cancel (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (!self->drag || !self->drag->source ||
      !data_source_is_host (self->drag->source))
    return;

  drag_cancel (self->drag);
  nested_flush (self);
}

NestedSurface *
nested_compositor_drag_icon (NestedCompositor *self)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), NULL);

  if (!self->drag || !self->drag->owns_pointer)
    return NULL;

  return self->drag->icon;
}

void
nested_compositor_drag_move_icon (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (!self->drag || !self->drag->icon)
    return;

  /* The protocol puts the icon's top-left at the pointer hotspot, and
   * wl_surface.set_offset moves it from there. Its place is therefore the
   * cursor's plus the offset, in the widget's own logical pixels — which is
   * what popup_x and popup_y hold for every other surface. */
  self->drag->icon->popup_x = round_to_pixel (self->pointer_x) + self->drag->icon->offset_x;
  self->drag->icon->popup_y = round_to_pixel (self->pointer_y) + self->drag->icon->offset_y;
}

void
nested_compositor_drag_motion (NestedCompositor *self)
{
  g_return_if_fail (NESTED_IS_COMPOSITOR (self));

  if (!self->drag || !self->drag->owns_pointer)
    return;

  /* A position outside the size the widget configured is a pointer that is no
   * longer over the page: the embedding widget cannot forward one, because its
   * surface stops having the pointer at its own edge. Checking it here rather
   * than only in the leave path is what lets a scripted seat — which feeds
   * coordinates, not crossings — reach the same code a real pointer does. */
  if (self->width > 0 && self->height > 0 &&
      (self->pointer_x < 0 || self->pointer_y < 0 ||
       self->pointer_x >= self->width || self->pointer_y >= self->height))
    {
      if (nested_has_drag_out_host (self) &&
          nested_compositor_drag_take_out (self))
        {
          nested_flush (self);
          return;
        }
    }

  nested_compositor_drag_move_icon (self);
  drag_update_destination (self->drag);

  /* Only an icon that reaches the screen makes this a picture change. */
  if (self->drag->icon && self->drag->icon->wl_surface &&
      self->drag->icon->mapped && self->drag->icon->texture)
    nested_surfaces_changed (self);
}

gboolean
nested_compositor_drag_button (NestedCompositor *self, uint32_t button, gboolean pressed)
{
  g_return_val_if_fail (NESTED_IS_COMPOSITOR (self), FALSE);

  if (!self->drag || !self->drag->owns_pointer)
    return FALSE;

  /* The grab is the drag's, so no button reaches the surfaces underneath while
   * it lasts: a second press means nothing, and the release is the drop. */
  if (pressed)
    {
      g_debug ("nested: button 0x%x pressed during a drag, which is not a click",
               button);
      return TRUE;
    }

  g_debug ("nested: button 0x%x released: the drag is let go", button);
  drag_drop (self->drag);

  return TRUE;
}

/* A client's data device is gone: give up the halves of the drag that travelled
 * through it before anything is sent, because the resource they would go on is
 * the one being destroyed. */
static void
drag_forget_device (NestedCompositor *self, DataDevice *device)
{
  Drag *drag = self->drag;
  gboolean is_origin, is_dest;

  if (!drag)
    return;

  is_origin = drag->origin_device == device;
  is_dest = drag->dest_device == device;

  if (!is_origin && !is_dest)
    return;

  if (is_dest)
    {
      drag->dest_device = NULL;
      drag->dest_offer = NULL;
      g_clear_object (&drag->dest_surface);
    }

  if (is_origin)
    {
      /* The source belongs to the client that dragged, so it is going with it:
       * nothing may be sent to it, and it is not ours to talk about any more. */
      drag->origin_device = NULL;
      drag->source = NULL;
      drag->concluded = TRUE;
    }

  drag_cancel (drag);
}

/* ------------------------------------------------------------- wl_data_offer */

static void
data_offer_accept (struct wl_client *client, struct wl_resource *resource,
                   uint32_t serial, const char *mime_type)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  Drag *drag = drag_for_offer (offer);

  /* A copy answers this by asking for the bytes when it wants them, so there is
   * nothing to tell anyone about what it intends to take. A drag does: the
   * source is kept informed during it, and for a drag inside one page that means
   * the page is told what its own code agreed to be handed. */
  if (!drag)
    return;

  g_clear_pointer (&drag->accepted_mime, g_free);
  drag->accepted_mime = mime_type ? g_strdup (mime_type) : NULL;
  drag->refused = mime_type == NULL;

  if (offer->source && !data_source_is_host (offer->source))
    wl_data_source_send_target (offer->source->resource, mime_type);
}

static void
data_offer_receive (struct wl_client *client, struct wl_resource *resource,
                    const char *mime_type, int32_t fd)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  NestedCompositor *self = offer->compositor;

  if (offer->source && data_source_is_host (offer->source))
    {
      const char *have = data_source_host_pick (offer->source, mime_type);

      if (!have)
        {
          g_debug ("nested: %s asked for, the host's drag carries %s", mime_type,
                   (const char *) g_ptr_array_index (offer->source->mime_types, 0));
          close (fd);
          return;
        }

      g_debug ("nested: handing the page %" G_GSIZE_FORMAT " bytes of the host's drag",
               g_bytes_get_size (offer->source->payload));

      clipboard_send_bytes (offer->source->payload, fd);
      return;
    }

  if (offer->source)
    {
      /* The owner of the selection, or of a drag, writes into the fd the
       * requester made; libwayland dups it, so our copy can go. Both ends being
       * one client is normal here: this is how a page's own drop event gets the
       * text it is dropping. */
      g_debug ("nested: passing %s to the client that owns it", mime_type);
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

/* These two are version 3 requests, so their slots had to be filled even before
 * a drag could make any use of them: a wl_resource implementation is indexed by
 * opcode and libwayland-server treats an empty slot as a bug in the compositor
 * rather than as a refusal. See docs/empty-request-slot.md. */
static void
data_offer_finish (struct wl_client *client, struct wl_resource *resource)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  Drag *drag = drag_for_offer (offer);

  if (!drag)
    {
      /* The protocol's answer to finishing a copy is the invalid_finish error,
       * and the client that asks for it is confused rather than dangerous. A
       * page killed over a version 3 request it was never obliged to send is a
       * worse outcome than a drag that was never there, so it is ignored —
       * which is also what the request looked like before drags worked at all. */
      g_debug ("nested: a client finished a copy, which is not a thing to finish");
      return;
    }

  if (!drag->dropped)
    {
      /* finish before the pointer was let go: the destination has the payload
       * already and is done negotiating. */
      g_debug ("nested: a drag was finished before the pointer let go of it");
      drag_cancel (drag);
      return;
    }

  g_debug ("nested: a drag concluded: the destination has what it was given");
  drag->concluded = TRUE;
  if (offer->source && !data_source_is_host (offer->source) &&
      wl_resource_get_version (offer->source->resource) >=
      WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION)
    wl_data_source_send_dnd_finished (offer->source->resource);

  drag_destroy (drag, TRUE);
}

static void
data_offer_set_actions (struct wl_client *client, struct wl_resource *resource,
                        uint32_t dnd_actions, uint32_t preferred_action)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  Drag *drag = drag_for_offer (offer);

  if (!drag)
    {
      g_debug ("nested: a client wants action 0x%x of 0x%x, and there is nothing to drop on",
               preferred_action, dnd_actions);
      return;
    }

  drag->dest_actions = dnd_actions;
  drag->preferred_action = preferred_action;
  drag_negotiate (drag);
}

static void
data_offer_resource_destroyed (struct wl_resource *resource)
{
  DataOffer *offer = wl_resource_get_user_data (resource);
  Drag *drag = drag_for_offer (offer);

  /* A destination that throws the offer away before the pointer is let go is
   * giving up on the drag — the protocol's own way of dismissing an ask. One
   * that throws it away afterwards has already had its drop, and cancelled
   * would be a lie: nothing is being called off. Either way the drag is over,
   * and this is the last moment at which the icon can be told it is not one. */
  if (drag)
    {
      drag->dest_offer = NULL;

      if (drag->dropped)
        {
          g_debug ("nested: a drag ended without the destination finishing");
          drag_destroy (drag, TRUE);
        }
      else
        drag_cancel (drag);
    }

  wl_list_remove (&offer->device_link);
  if (offer->source)
    wl_list_remove (&offer->source_link);
  g_free (offer);
}

static const struct wl_data_offer_interface data_offer_impl = {
  .accept = data_offer_accept,
  .receive = data_offer_receive,
  .destroy = data_offer_destroy,
  .finish = data_offer_finish,
  .set_actions = data_offer_set_actions,
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

  /* "If source is destroyed, the drag-and-drop session will be cancelled": the
   * drag is called off first, and its hold on this resource is dropped before
   * that happens, because a source being destroyed cannot be sent anything. */
  if (self->drag && self->drag->source == source)
    {
      self->drag->source = NULL;
      self->drag->concluded = TRUE;
      drag_cancel (self->drag);
    }

  /* The offers outlive the source: their clients still hold them and will
   * destroy them themselves. Detached, they fall back to the widget
   * clipboard, which still holds what was copied. */
  data_source_offers_detach (source);

  if (self->selection_source == source)
    {
      self->selection_source = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);

      /* The copying client is gone: the widget clipboard is the selection
       * again, and whoever is still connected has to be told. */
      compositor_advertise_selection (self);
    }

  g_clear_pointer (&source->mime_types, g_ptr_array_unref);
  g_free (source);
}

static void
data_source_set_actions (struct wl_client *client, struct wl_resource *resource,
                         uint32_t dnd_actions)
{
  DataSource *source = wl_resource_get_user_data (resource);
  NestedCompositor *self = source->compositor;
  Drag *drag = self->drag;

  /* The protocol wants this once, before start_drag, so there is usually no
   * drag to attach it to yet: it is the source that remembers. Chromium also
   * sends it for a plain clipboard source, which is why a mask from a source
   * that is not dragging is only recorded. */
  source->dnd_actions = dnd_actions;

  if (!drag || drag->source != source)
    {
      g_debug ("nested: a source supports actions 0x%x", dnd_actions);
      return;
    }

  drag->source_actions = dnd_actions != 0 ? dnd_actions : DRAG_ALL_ACTIONS;

  /* "or anytime the source side changes its offered actions": the destination
   * is told, then both are asked again what they can agree on. */
  if (drag->dest_offer &&
      wl_resource_get_version (drag->dest_offer) >=
      WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
    wl_data_offer_send_source_actions (drag->dest_offer, drag->source_actions);

  drag_negotiate (drag);
}

static const struct wl_data_source_interface data_source_impl = {
  .offer = data_source_offer,
  .destroy = data_source_destroy,
  .set_actions = data_source_set_actions,
};

/* ----------------------------------------------------------- wl_data_device */

static void
data_device_start_drag (struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *source_resource, struct wl_resource *origin,
                        struct wl_resource *icon, uint32_t serial)
{
  DataDevice *device = wl_resource_get_user_data (resource);
  NestedCompositor *self = device->compositor;
  NestedSurface *origin_surface;
  NestedSurface *icon_surface = NULL;
  DataSource *source = NULL;
  Drag *drag;

  /* A drag has to come from somewhere, and the surface it comes from has to be
   * one this compositor is drawing: both are the client naming a wrong object,
   * which surface_from_resource() reports. */
  origin_surface = surface_from_resource (resource, origin);
  if (!origin_surface)
    return;

  if (icon)
    {
      icon_surface = surface_from_resource (resource, icon);
      if (!icon_surface)
        return;

      if (icon_surface->xdg_surface)
        {
          /* The one error a drag is worth raising: a surface that is already a
           * window cannot also be an icon, and a client that tries has lost
           * track of what it is drawing with. */
          wl_resource_post_error (resource, WL_DATA_DEVICE_ERROR_ROLE,
                                  "the drag icon id %u is already a window",
                                  wl_resource_get_id (icon));
          return;
        }
    }

  if (source_resource)
    {
      if (!wl_resource_instance_of (source_resource, &wl_data_source_interface,
                                    &data_source_impl))
        {
          post_wrong_object (resource, source_resource, "wl_data_source");
          return;
        }
      source = wl_resource_get_user_data (source_resource);
    }

  /* One pointer, one drag: a second start_drag means the client lost the first,
   * and the seat cannot hold button events back for two of them. */
  if (self->drag)
    drag_cancel (self->drag);

  drag = g_new0 (Drag, 1);
  drag->compositor = self;
  drag->source = source;
  drag->origin_device = device;
  drag->origin = g_object_ref (origin_surface);
  drag->icon = icon_surface ? g_object_ref (icon_surface) : NULL;
  drag->serial = serial;
  drag->owns_pointer = TRUE;
  drag->source_actions = source && source->dnd_actions != 0 ? source->dnd_actions
                                                            : DRAG_ALL_ACTIONS;
  drag->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  if (icon_surface)
    icon_surface->is_drag_icon = TRUE;
  self->drag = drag;

  g_debug ("nested: a drag started over surface id %u carrying %u type%s",
           wl_resource_get_id (origin),
           source ? source->mime_types->len : 0u,
           source && source->mime_types->len == 1 ? "" : "s");

  /* The cursor is already over something when the drag begins, and a page that
   * drags inside itself has to be told about the surface it started on: without
   * this first enter it never fires a dragenter, and however far the pointer
   * then moves its drop handlers stay shut. */
  nested_compositor_drag_move_icon (self);
  drag_update_destination (drag);
  nested_surfaces_changed (self);
  nested_flush (self);
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
      compositor_advertise_selection (self);
      return;
    }

  compositor_advertise_selection (self);

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

  /* First the drag, because its offers are about to be destroyed and their
   * notifications would otherwise find a device they can no longer use. */
  drag_forget_device (device->compositor, device);

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
    compositor_advertise_selection (self);
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
  /* Nothing is sent on the way out: the display is going with these resources,
   * and a drag's surfaces belong to clients that are already gone. */
  if (self->drag)
    drag_destroy (self->drag, FALSE);

  if (self->clipboard_transfer)
    {
      self->clipboard_transfer->compositor = NULL;
      clipboard_transfer_cancel (self->clipboard_transfer);
    }
}
