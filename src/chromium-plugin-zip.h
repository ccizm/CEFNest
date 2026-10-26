/* Plugin package reading: a zip archive, or a .crx around one. Internal to
 * the library — install_async() is the only consumer, and a host unpacks
 * nothing here itself. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _ChromiumZip ChromiumZip;

/* Opens @path as a .crx (the CRX3 shell is skipped: "Cr24", a version, a
 * protobuf header whose length names where the zip starts) or as a plain
 * zip. Everything else refuses. */
ChromiumZip *chromium_zip_open (const char *path, GError **error);

/* The names in the central directory, in archive order. Caller owns the
 * array and the strings. */
char       **chromium_zip_names (ChromiumZip *zip);

/* One entry's bytes. Directories come back as NULL data with len 0. Caller
 * owns the data. */
gboolean     chromium_zip_read (ChromiumZip *zip, const char *name,
                                char **data, gsize *length, GError **error);

void         chromium_zip_free (ChromiumZip *zip);

/* Not a GObject — the cleanup function is declared by hand so the readers
 * of this header can use g_autoptr without a GObject type behind it. */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ChromiumZip, chromium_zip_free)

G_END_DECLS
