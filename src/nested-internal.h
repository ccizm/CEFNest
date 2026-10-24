#pragma once

#include <gtk/gtk.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "nested.h"

G_BEGIN_DECLS

/* Version ceilings for the globals nested_compositor_new() advertises, and
 * the plane count every dmabuf structure sizes its arrays with. A wl_pointer
 * and wl_keyboard are created at the version their wl_seat was bound at, so
 * 8 here is what lets the seat hand out a pointer that can carry
 * axis_value120 — the only wheel channel that survives a fractional detent. */
#define NESTED_SEAT_VERSION     8
#define NESTED_DMABUF_VERSION   5
#define NESTED_DMABUF_MAX_PLANES 4
#define NESTED_DATA_DEVICE_MANAGER_VERSION 3
#define NESTED_TEXT_INPUT_VERSION 1
#define NESTED_CURSOR_SHAPE_VERSION 1

/* The largest wl_shm pool a client may hand over. A 4K frame in ARGB8888 is
 * about 33 MB, so this still leaves room for a handful of them. The cap is here
 * because an shm buffer is copied out of the pool on every commit: without it
 * one client could make the host allocate and read as much as it liked, once a
 * frame. */
#define NESTED_SHM_MAX_POOL ((int32_t) 256 * 1024 * 1024)

/* What all the live pools of one client may add up to: four maximal pools. A real
 * shm session measures two frames in flight — 4.98 MB for a 1000x653 window, and
 * 66 MB if that window were 4K — so this is a ceiling for a client that means
 * harm, not a budget a page runs into. The per-pool cap alone left the total
 * unbounded: one connection could hand over as many maximal pools as it liked,
 * and the host copies every one of them out on each commit. */
#define NESTED_SHM_MAX_CLIENT ((gsize) 4 * NESTED_SHM_MAX_POOL)

/* How many popups deep one may hang off another. A menu inside a menu is real,
 * and no interface a page can reach gets anywhere near thirty; the cap is here
 * because the tree under a surface is walked recursively, both to place every
 * popup of a window (xdg_popups_replace) and to build the paint stack. A client
 * that could nest without limit could make that walk however deep it liked, and
 * the host shares its stack with everything else in the process. */
#define NESTED_POPUP_MAX_DEPTH 32

typedef struct _Pool Pool;
typedef struct _Buffer Buffer;
typedef struct _BufferUser BufferUser;
typedef struct _TextInput TextInput;
typedef struct _DataSource DataSource;
typedef struct _ClipboardTransfer ClipboardTransfer;
typedef struct _Drag Drag;

typedef struct _TextInput TextInput;

struct _Pool {
  int fd;
  size_t size;
  guint8 *map;                 /* read-only mmap of fd, or NULL */
  guint refcount;
  /* Which connection's shm budget this sits in. The compositor is only ever
   * looked at while it is alive: it destroys its clients before it drops the
   * budget table, and a client going away destroys the pool resource that holds
   * the last reference here. */
  struct wl_client *client;
  NestedCompositor *compositor;
  /* Only to answer a pool whose file went away under the map (see
   * pool_map_is_whole). A buffer outlives the pool object it was made from — the
   * client destroys the pool as soon as it has its buffers — so this is cleared
   * with the resource and a pool with no resource left can only drop the map. */
  struct wl_resource *resource;
};

/* A (fourcc, modifier) pair advertised through zwp_linux_dmabuf_v1. */
typedef struct {
  uint32_t fourcc;
  uint64_t modifier;
} DmabufFormat;

/* The placement rules a client writes into an xdg_positioner. They are copied
 * out at get_popup and at every reposition, because the protocol lets the
 * client destroy the positioner as soon as it has been named, and a later
 * re-place still has to work from them. */
typedef struct {
  int32_t size_width, size_height;
  int32_t rect_x, rect_y, rect_width, rect_height;
  int32_t offset_x, offset_y;
  uint32_t anchor, gravity, constraint;
} PositionerRules;

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
  /* Software (shm) path: a persistent frame buffer so a commit only copies the
   * damaged rectangle out of the pool instead of pread()ing the whole plane. */
  guint8 *shm_shadow;
  gsize shm_shadow_len;
  int shm_shadow_width;
  int shm_shadow_height;
  int shm_shadow_stride;
  uint32_t shm_shadow_format;
  /* Damage as the shadow consumes it: one rectangle in buffer pixels, worked out
   * by the commit that carries it. */
  GdkRectangle shm_damage;
  gboolean shm_damage_valid;
  /* Damage as the client states it, which is twice over and never in the units
   * the shadow wants: wl_surface.damage is surface-local, wl_surface.damage_buffer
   * is buffer pixels, and the factor between them is the buffer scale of the
   * commit — so both wait here until the commit converts the first. */
  GdkRectangle pending_damage_surface, pending_damage_buffer;
  gboolean pending_damage_surface_valid, pending_damage_buffer_valid;
  int width;                 /* surface-local size, after crop and scale */
  int height;
  int natural_width;         /* buffer size divided by scale */
  int natural_height;
  int32_t geom_x;            /* xdg_surface.set_window_geometry */
  int32_t geom_y;
  int32_t geom_width;
  int32_t geom_height;
  int32_t scale;             /* wl_surface.set_buffer_scale, current */
  int32_t pending_scale;     /* ... and the copy that commit makes current */

  char *app_id;
  char *title;

  /* Popup placement. The rules are copied out of the client's xdg_positioner
   * when it names them, and popup_x/popup_y are what they work out to: the
   * surface's top-left, in the parent surface's logical pixels. A toplevel is
   * at 0,0 with no parent. */
  PositionerRules positioner;
  /* The parent keeps its children in the order they are drawn, so the last one
   * is the topmost. The child holds the reference that keeps the parent
   * alive, which is what makes popup_parent safe to read. */
  NestedSurface *popup_parent;
  struct wl_list popup_children;  /* NestedSurface.popup_link */
  struct wl_list popup_link;
  int popup_x;
  int popup_y;

  /* The wl_data_device drag icon: the one surface whose place is decided by the
   * pointer rather than by a window, and the one for which
   * wl_surface.set_offset still means something. The protocol puts its top-left
   * at the cursor hotspot and lets the offset move it from there, so popup_x
   * and popup_y are rewritten from the pointer whenever either changes. */
  int32_t pending_offset_x, pending_offset_y;
  int32_t offset_x, offset_y;

  uint32_t configure_serial;
  unsigned int is_toplevel : 1;
  unsigned int is_popup : 1;
  unsigned int mapped : 1;
  unsigned int is_drag_icon : 1;
  /* xdg_popup.grab arrived: while it is held the client expects the pointer and
   * keyboard over this surface for the whole window, and a press that lands
   * outside it is meant to dismiss the menu. */
  unsigned int popup_grabbed : 1;
  /* Set on the toplevel a view is showing while its window is off the screen:
   * frame callbacks queue but nothing answers them, so the page's rAF — and
   * with it its whole render pipeline — stalls until the window is mapped
   * again. Popups read it through their popup_parent chain, the way
   * surface_is_painted() finds its root. */
  unsigned int frames_suspended : 1;
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
  guint frame_watchdog_id;      /* armed while a waiting client has no answer */
  gboolean clock_alive;         /* FALSE once the widget's clock missed a cycle */
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
  struct wl_global *global_primary_selection;
  struct wl_global *global_text_input;
  struct wl_global *global_cursor_shape;

  struct wl_list outputs;       /* OutputResource.link */
  struct wl_list surfaces;      /* NestedSurface.nested_link */
  struct wl_list seat_clients;  /* SeatClient.link */
  struct wl_list text_inputs;   /* TextInput.link */
  /* Bytes of live wl_shm pool per connection, against NESTED_SHM_MAX_CLIENT. */
  GHashTable *shm_budget;       /* struct wl_client * -> gsize */

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
  struct wl_list primary_devices;  /* PrimaryDevice.link */
  DataSource *primary_source;
  ClipboardTransfer *primary_transfer;
  GdkClipboard *primary_clipboard;
  gulong primary_handler;
  GBytes *primary_bytes;
  char *primary_mime;
  guint primary_echo;
  guint primary_replay_id;
  Drag *drag;                       /* the wl_data_device drag in progress, or NULL */
  ClipboardTransfer *clipboard_transfer;
  /* The payload of a drag that left the widget, read out of the client's
   * wl_data_source for the host to carry on. Its own slot because a copy can be
   * in flight at the same moment, and one payload must not take the other's
   * pipe away. */
  ClipboardTransfer *drag_out_transfer;
  GdkClipboard *clipboard;          /* the embedding widget's, may be NULL */
  gulong clipboard_handler;         /* notify::content */
  GBytes *clipboard_bytes;
  char *clipboard_mime;
  guint clipboard_echo;             /* ignores the notify our own write causes */
  guint selection_replay_id;        /* pending idle, see nested-data-device.c */

  int width;                  /* configured size, in widget logical pixels */
  int height;
  double scale;               /* device pixels per logical pixel */
  uint32_t serial;
  uint64_t frame_count;

  /* Commit accounting behind CHROMIUM_FRAME_METRICS and the selftest's --perf
   * report: which path the clients actually take, and how many bytes each
   * commit moves. shm_bytes_copied is pool -> shadow traffic (damage
   * rectangles and full reads); shm_bytes_wrapped is the full-frame memcpy a
   * fresh GBytes makes per commit before gdk_memory_texture_new() wraps it. */
  uint64_t shm_commits;
  uint64_t dmabuf_commits;
  uint64_t shm_bytes_copied;
  uint64_t shm_bytes_wrapped;

  NestedSurface *primary;
  /* What the page asked for vs what the host window actually is: the states
   * array advertises the latter only. */
  gboolean window_fullscreen_requested;
  gboolean window_fullscreen;

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
  /* seat_retarget fast-path bookkeeping (nested-seat.c): what the last full
   * retarget saw. tree_generation moves on every surface-tree change, popup
   * grab change and popup reposition, so equality with
   * retarget_tree_generation plus unchanged inputs and a pointer still inside
   * the focused surface proves a full recomputation would return exactly the
   * focuses already in place. */
  guint tree_generation;
  guint retarget_tree_generation;
  NestedSurface *retarget_input_surface;
  unsigned int retarget_pointer_wanted : 1;
  unsigned int retarget_keyboard_wanted : 1;
  unsigned int retarget_drag_active : 1;
  /* The window the widget draws its pointer in: whatever a client asks the
   * cursor to be, it is set on this GdkSurface. NULL until the view maps. */
  GdkSurface *cursor_target;
  /* The toplevel whose host view last took input. With several GTK windows
   * sharing one compositor, the pointer/keyboard target cannot be inferred from
   * one global pointer coordinate; the view that owns the event names it. */
  NestedSurface *input_surface;
  /* Fingerprint of the mapped-toplevel set, folded into the same walk that
   * picks the primary. ::toplevels-changed only fires when this moves: commits
   * of an already-painted window call nested_update_primary() every frame and
   * must not re-run the host's surface assignment with them. */
  guint toplevels_fingerprint;
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
  /* Set before dispose() drops the clients. wl_display_destroy_clients() runs
   * every wl_resource destroy callback, and those callbacks all report through
   * the functions that emit ::frame, ::toplevels-changed and
   * ::text-input-state — so without this the embedding widget is handed a paint
   * request out of an object that has already lost its frame clock, its GSource
   * and half of its surface list, and its handler reads the state that is gone.
   * The state updates still happen; only the calls out to the host stop, because
   * there is no host left to show a picture to. */
  unsigned int disposing : 1;
};

/* ------------------------------------------------------------------ *
 * Cross-module helpers                                               *
 * ------------------------------------------------------------------ */

/* wl_resource_create() failing means the library could not allocate. Every
 * caller here would go straight on to wl_resource_set_implementation() with the
 * result, which takes the process down with it, so the failure is turned into
 * the protocol's own no_memory error and the caller gets NULL to bail on. */
static inline struct wl_resource *
resource_create (struct wl_client *client, const struct wl_interface *interface,
                 int version, uint32_t id)
{
  struct wl_resource *resource = wl_resource_create (client, interface, version, id);

  if (!resource)
    wl_client_post_no_memory (client);

  return resource;
}

/* nested.c */
void           nested_flush            (NestedCompositor *self);
void           nested_update_primary   (NestedCompositor *self);
/* The compositor's own serial counter and event clock: wl_pointer.enter,
 * wl_data_device.enter and a popup grab all cite serials from here, and a
 * client can only answer one of them if there is a single source of them. */
uint32_t       nested_next_serial      (NestedCompositor *self);
uint32_t       nested_now_msec         (void);
/* A surface tree change (a popup mapped, a toplevel gone) both retargets the
 * seat and tells the widget to draw again. */
void           nested_surfaces_changed (NestedCompositor *self);
/* Whether a host listens to NestedCompositor::drag-out, and the way to emit it.
 * A payload is only worth reading out of a client's drag source if somebody can
 * carry it on, so the seat asks before it asks the client for anything. */
gboolean       nested_has_drag_out_host  (NestedCompositor *self);
void           nested_emit_drag_out      (NestedCompositor *self, const char *mime,
                                          GBytes *bytes, char **types);
/* The verdict on a drag the host carried in, told to it when the drag is
 * retired: a host has to answer its own compositor's drop with success or
 * failure, and only the destination's requests say which it was. */
void           nested_emit_drag_in_end   (NestedCompositor *self, gboolean taken,
                                          const char *mime);
/* The painted surface under a point in the widget's logical pixels, or NULL.
 * Popups sit on the toplevel, so this is where the pointer learns what it is
 * really over. */
NestedSurface *nested_compositor_surface_at (NestedCompositor *self,
                                             double x, double y);
/* Where the keyboard belongs: the topmost popup that asked for a grab, since
 * that is the client's own idea of a modal menu, and otherwise the toplevel. */
NestedSurface *nested_compositor_keyboard_target (NestedCompositor *self);
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
/* TRUE when @serial is a press @client was given and still holds: the grab a
 * wl_data_device.start_drag has to cite before a drag may start. */
gboolean    seat_client_grab_is_current (NestedCompositor *self,
                                         struct wl_client *client,
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
void        primary_selection_manager_bind (struct wl_client *client, void *data,
                                            uint32_t version, uint32_t id);
void        data_device_shutdown     (NestedCompositor *self);
/* A drag takes the pointer away from the seat until the button that started it
 * is released: the position goes to wl_data_device.motion and nothing to
 * wl_pointer. The button one says whether the drag consumed it. */
gboolean    nested_compositor_drag_active (NestedCompositor *self);
void        nested_compositor_drag_motion (NestedCompositor *self);
gboolean    nested_compositor_drag_button (NestedCompositor *self, uint32_t button,
                                           gboolean pressed);
/* Move the drag icon to wherever the pointer and its own offset now put it. */
void        nested_compositor_drag_move_icon (NestedCompositor *self);
/* The surface a drag carries as its icon, whether or not it has pixels yet: it
 * is drawn above every window and takes no input while the drag holds the
 * pointer. */
NestedSurface *nested_compositor_drag_icon (NestedCompositor *self);

/* nested-xdg.c */
void        output_bind           (struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id);
void        output_send_state_all (NestedCompositor *self);
void        xdg_wm_base_bind      (struct wl_client *client, void *data,
                                   uint32_t version, uint32_t id);
void        xdg_surface_configure (NestedSurface *self);
void        nested_request_window_fullscreen (NestedSurface *self,
                                      gboolean enter);
/* Work a popup's copied-out positioner rules into popup_x/popup_y. Called when
 * the client names the rules and again whenever the surface it hangs off
 * changes size, because the anchor rectangle lives in the parent. */
void        xdg_popup_replace     (NestedSurface *self);
/* Reposition every popup mapped over @surface, bottommost first. */
void        xdg_popups_replace    (NestedSurface *surface);
/* The client's grab is over: xdg_popup.popup_done, which is how it learns the
 * menu it opened has been dismissed. */
void        xdg_popups_dismiss    (NestedCompositor *self,
                                   NestedSurface *keep);

/* nested-text-input.c */
void        text_input_manager_bind  (struct wl_client *client, void *data,
                                      uint32_t version, uint32_t id);
void        text_input_retarget      (NestedCompositor *self, NestedSurface *surface);
void        text_input_forget_surface (NestedCompositor *self, NestedSurface *surface);

G_END_DECLS
