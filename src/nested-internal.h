#pragma once

#include <gtk/gtk.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "nested.h"

G_BEGIN_DECLS

/* Version ceilings for the globals nested_compositor_new() advertises, and
 * the plane count every dmabuf structure sizes its arrays with. */
#define NESTED_SEAT_VERSION     5
#define NESTED_DMABUF_VERSION   5
#define NESTED_DMABUF_MAX_PLANES 4
#define NESTED_DATA_DEVICE_MANAGER_VERSION 3
#define NESTED_TEXT_INPUT_VERSION 1
#define NESTED_CURSOR_SHAPE_VERSION 1

typedef struct _Pool Pool;
typedef struct _Buffer Buffer;
typedef struct _BufferUser BufferUser;
typedef struct _TextInput TextInput;
typedef struct _DataSource DataSource;
typedef struct _ClipboardTransfer ClipboardTransfer;

typedef struct _TextInput TextInput;

struct _Pool {
  int fd;
  size_t size;
  guint refcount;
};

/* A (fourcc, modifier) pair advertised through zwp_linux_dmabuf_v1. */
typedef struct {
  uint32_t fourcc;
  uint64_t modifier;
} DmabufFormat;

struct _Buffer {
  Pool *pool;                 /* shm only */
  struct wl_resource *resource;
  struct wl_list users;       /* BufferUser.link */
  int32_t offset;             /* shm only */
  int32_t width;
  int32_t height;
  int32_t stride;             /* shm only */
  uint32_t format;            /* shm only */

  /* dmabuf planes. The fds arrive owned by us; once a texture has been built
   * they belong to that texture's destroy notify, because the texture may well
   * outlive the wl_buffer the client attached. */
  gboolean is_dmabuf;
  gboolean fds_transferred;
  int fd[NESTED_DMABUF_MAX_PLANES];
  uint32_t plane_offset[NESTED_DMABUF_MAX_PLANES];
  uint32_t plane_stride[NESTED_DMABUF_MAX_PLANES];
  int n_planes;
  uint32_t fourcc;
  uint64_t modifier;
  GdkTexture *texture;        /* dmabuf only: buffer contents are immutable */
};

/* A reference slot held by a surface. Keeping the list node inside the
 * surface (rather than a refcount on the buffer) lets destroying a client's
 * wl_buffer clear every pointer to it without touching freed surfaces. */
struct _BufferUser {
  Buffer *buffer;
  NestedSurface *surface;
  struct wl_list link;
};
struct _NestedSurface {
  GObject parent_instance;

  NestedCompositor *nested;
  struct wl_list nested_link;
  gboolean in_list;

  struct wl_resource *wl_surface;
  struct wl_resource *xdg_surface;
  struct wl_resource *toplevel;
  struct wl_resource *popup;
  struct wl_resource *viewport;
  struct wl_resource *fractional;
  struct wl_list fractional_links;    /* FractionalScale.link */

  struct wl_list frame_callbacks;
  BufferUser pending_user;
  gboolean pending_attach;

  /* Crop and scale state, in "natural" coordinates: buffer pixels divided by
   * the buffer scale, which is what wp_viewport.set_source uses. The pending
   * copy becomes current on wl_surface.commit, like the buffer. */
  struct { double x, y, width, height; } pending_src, src;
  struct { int width, height; } pending_dst, dst;

  GdkTexture *texture;
  int width;                 /* surface-local size, after crop and scale */
  int height;
  int natural_width;         /* buffer size divided by scale */
  int natural_height;
  int geom_width;
  int geom_height;
  int32_t scale;             /* wl_surface.set_buffer_scale */

  char *app_id;
  char *title;

  uint32_t configure_serial;
  unsigned int is_toplevel : 1;
  unsigned int is_popup : 1;
  unsigned int mapped : 1;
};

struct _NestedCompositor {
  GObject parent_instance;

  struct wl_display *display;
  struct wl_event_loop *loop;
  GSource *source;
  gulong source_id;
  guint frame_timer_id;
  GdkFrameClock *frame_clock;   /* the embedding widget's, may be NULL */
  gulong frame_clock_handler;
  char *socket_name;

  struct wl_global *global_compositor;
  struct wl_global *global_shm;
  struct wl_global *global_output;
  struct wl_global *global_seat;
  struct wl_global *global_xdg_wm_base;
  struct wl_global *global_dmabuf;
  struct wl_global *global_viewporter;
  struct wl_global *global_fractional;
  struct wl_global *global_data;
  struct wl_global *global_text_input;
  struct wl_global *global_cursor_shape;

  struct wl_list outputs;       /* OutputResource.link */
  struct wl_list surfaces;      /* NestedSurface.nested_link */
  struct wl_list seat_clients;  /* SeatClient.link */
  struct wl_list text_inputs;   /* TextInput.link */

  /* Format list for zwp_linux_dmabuf_v1, probed from the render node. Empty
   * means no dmabuf global at all: clients keep using wl_shm. */
  struct gbm_device *gbm;
  GArray *dmabuf_formats;       /* DmabufFormat */

  /* Feedback shares one immutable memfd holding the format table, in the
   * 16-byte (format, padding, modifier) layout the protocol mandates, and the
   * dev_t of the node we import on. */
  int dmabuf_table_fd;
  size_t dmabuf_table_size;
  uint64_t dmabuf_device;

  /* Clipboard bridge. selection_source is a client that owns the selection and
   * can be asked for its contents; clipboard_bytes is what the GTK clipboard
   * holds, buffered so a client can be served without blocking the loop. */
  struct wl_list data_devices;      /* DataDevice.link */
  DataSource *selection_source;
  ClipboardTransfer *clipboard_transfer;
  GdkClipboard *clipboard;          /* the embedding widget's, may be NULL */
  gulong clipboard_handler;         /* notify::content */
  GBytes *clipboard_bytes;
  char *clipboard_mime;
  guint clipboard_echo;             /* ignores the notify our own write causes */

  int width;                  /* configured size, in widget logical pixels */
  int height;
  double scale;               /* device pixels per logical pixel */
  uint32_t serial;
  uint64_t frame_count;

  NestedSurface *primary;

  /* Injected input state. The focus follows "primary"; xkb only tracks the
   * modifier bits, so the keymap is compiled but never used to resolve
   * keyvals — the client does that with the keymap we hand it. */
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  char *keymap_text;
  size_t keymap_size;
  GArray *pressed_keys;      /* uint32_t evdev codes */
  NestedSurface *pointer_focus;
  NestedSurface *keyboard_focus;
  /* The window the widget draws its pointer in: whatever a client asks the
   * cursor to be, it is set on this GdkSurface. NULL until the view maps. */
  GdkSurface *cursor_target;
  /* wl_pointer.set_cursor names a surface, and the cursor only changes when
   * that surface commits. Set when the request arrives, applied again on the
   * commit, and dropped as soon as a buffer is current. */
  NestedSurface *cursor_pending;
  int32_t cursor_hotspot_x;
  int32_t cursor_hotspot_y;
  /* The one text input the widget should compose for: entered, and enabled by
   * its client. NULL means keys go to the client as plain wl_keyboard.key. */
  TextInput *active_input;
  double pointer_x;
  double pointer_y;
  unsigned int pointer_wanted : 1;
  unsigned int keyboard_wanted : 1;
};

/* ------------------------------------------------------------------ *
 * Cross-module helpers                                               *
 * ------------------------------------------------------------------ */

/* nested.c */
void           nested_flush            (NestedCompositor *self);
void           nested_update_primary   (NestedCompositor *self);
void           nested_notify_text_input_state (NestedCompositor *self);
NestedSurface *surface_from_resource   (struct wl_resource *where,
                                        struct wl_resource *resource);
void           post_wrong_object       (struct wl_resource *where,
                                        struct wl_resource *what,
                                        const char *iface);

/* nested-seat.c */
void        seat_bind             (struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id);
void        seat_retarget         (NestedCompositor *self);
void        seat_clear_focus      (NestedCompositor *self, NestedSurface *surface);
void        nested_setup_keymap   (NestedCompositor *self);
gboolean    seat_resource_is_seat (struct wl_resource *resource);
gboolean    seat_resource_is_pointer (struct wl_resource *resource);
/* TRUE when @serial is the enter the client is currently inside, which is what
 * wp_cursor_shape_device_v1.set_shape insists on before a cursor may change. */
gboolean    seat_pointer_serial_is_current (struct wl_resource *pointer,
                                            uint32_t serial);

/* nested-cursor.c */
void        cursor_shape_manager_bind (struct wl_client *client, void *data,
                                       uint32_t version, uint32_t id);
/* wl_pointer.set_cursor: a cursor surface, or NULL to hide the pointer. */
void        cursor_pointer_set_cursor (NestedCompositor *self,
                                       struct wl_resource *pointer,
                                       struct wl_resource *surface,
                                       int32_t hotspot_x, int32_t hotspot_y);
/* Back to the widget's own pointer, as when nothing of ours is under it. */
void        cursor_reset              (NestedCompositor *self);
/* wl_surface.commit: a cursor surface named by set_cursor only takes effect
 * once the client has committed the buffer it attached to it. */
void        cursor_surface_committed  (NestedCompositor *self,
                                       NestedSurface *surface);

/* nested-dmabuf.c */
void        nested_setup_dmabuf         (NestedCompositor *self);
void        nested_shutdown_dmabuf      (NestedCompositor *self);
void        dmabuf_bind                 (struct wl_client *client, void *data,
                                         uint32_t version, uint32_t id);
GdkTexture *buffer_create_dmabuf_texture (Buffer *buffer);
struct wl_resource *buffer_resource_new (struct wl_client *client,
                                        Buffer *buffer, uint32_t id);

/* nested-data-device.c */
void        data_device_manager_bind (struct wl_client *client, void *data,
                                      uint32_t version, uint32_t id);
void        data_device_shutdown     (NestedCompositor *self);

/* nested-xdg.c */
void        output_bind           (struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id);
void        output_send_state_all (NestedCompositor *self);
void        xdg_wm_base_bind      (struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id);
void        xdg_surface_configure (NestedSurface *self);

/* nested-text-input.c */
void        text_input_manager_bind  (struct wl_client *client, void *data,
                                      uint32_t version, uint32_t id);
void        text_input_retarget      (NestedCompositor *self, NestedSurface *surface);
void        text_input_forget_surface (NestedCompositor *self, NestedSurface *surface);

G_END_DECLS
