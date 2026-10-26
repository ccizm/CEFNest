#define G_LOG_DOMAIN "chromium-plugin"

#include <string.h>

#include <gio/gio.h>
#include <zlib.h>

#include "chromium-plugin-zip.h"

/* A minimal zip reader for plugin packages. What it trusts: the central
 * directory's offsets (an archive that lies about them is an archive nothing
 * else could read either). What it does not trust: entry names — no absolute
 * paths, no ".." segments, no backslashes, which is the same rule the plugin
 * root's other doors enforce; install_async() checks again when it writes.
 * Deflated entries inflate through zlib; stored entries are a memcpy. */

#define ZIP_EOCD_MAGIC   0x06054b50u
#define ZIP_CENTRAL_MAGIC 0x02014b50u
#define ZIP_LOCAL_MAGIC   0x04034b50u
#define ZIP_EOCD_MIN     22u
/* The comment can carry 65535 bytes and the EOCD sits after it. */
#define ZIP_EOCD_SCAN    (ZIP_EOCD_MIN + 65535u)

/* CRX3: "Cr24", version 3, then a 4-byte header length, then the header, then
 * the zip. Nothing in the header matters here — the signature work belongs to
 * a scheme this component deliberately does not have. */
#define CRX_MAGIC        0x34327243u  /* "Cr24", little-endian read */

typedef struct {
  char *name;
  guint16 method;
  guint32 crc;
  gsize local_offset;
  gsize packed_size;
  gsize size;
} ZipEntry;

struct _ChromiumZip {
  char *path;
  char *data;        /* the whole file; packages are small */
  gsize length;
  gsize zip_offset;  /* where the zip starts inside @data (CRX3 shell) */

  ZipEntry *entries;
  gsize n_entries;
};

static guint16
read16 (const char *p)
{
  return (guint16) ((guint8) p[0] | ((guint8) p[1] << 8));
}

static guint32
read32 (const char *p)
{
  return (guint32) ((guint8) p[0] | ((guint8) p[1] << 8) |
                    ((guint8) p[2] << 16) | ((guint8) p[3] << 24));
}

static gboolean
name_is_safe (const char *name)
{
  if (!*name || name[0] == '/' || strchr (name, '\\') || strstr (name, ".."))
    return FALSE;

  /* The segment rule the rest of the plugin paths keep: no empty and no dot
     segments, which ".." already covers and "." is too easy to mean. */
  for (const char *p = name, *seg = name; ; p++)
    {
      if (*p == '/' || *p == '\0')
        {
          if (p == seg)
            return FALSE;
          if (p - seg == 1 && seg[0] == '.')
            return FALSE;
          if (*p == '\0')
            break;
          seg = p + 1;
        }
    }

  return TRUE;
}

void
chromium_zip_free (ChromiumZip *zip)
{
  if (!zip)
    return;

  for (gsize i = 0; i < zip->n_entries; i++)
    g_free (zip->entries[i].name);
  g_free (zip->entries);
  g_free (zip->data);
  g_free (zip->path);
  g_free (zip);
}

ChromiumZip *
chromium_zip_open (const char *path, GError **error)
{
  g_autoptr (ChromiumZip) zip = g_new0 (ChromiumZip, 1);
  gsize length = 0;

  g_return_val_if_fail (path != NULL, NULL);

  zip->path = g_strdup (path);
  if (!g_file_get_contents (path, &zip->data, &length, error))
    return NULL;
  zip->length = length;

  if (zip->length >= 12 && read32 (zip->data) == CRX_MAGIC &&
      read32 (zip->data + 4) == 3)
    {
      const guint32 header = read32 (zip->data + 8);

      if ((gsize) 12 + header >= zip->length)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "\"%s\" has a CRX header longer than the file", path);
          return NULL;
        }
      zip->zip_offset = 12u + header;
    }

  /* The EOCD, scanned from the end because a self-extracting prefix would
     otherwise be indistinguishable from it. */
  const gsize scan = MIN (zip->length - zip->zip_offset, ZIP_EOCD_SCAN);
  if (scan < ZIP_EOCD_MIN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "\"%s\" is too small to be a package", path);
      return NULL;
    }
  const char *base = zip->data + zip->zip_offset;
  gsize eocd = 0;
  gboolean found = FALSE;
  for (gsize i = scan - ZIP_EOCD_MIN + 1; i-- > 0; )
    {
      if (read32 (base + i) == ZIP_EOCD_MAGIC)
        {
          eocd = i;
          found = TRUE;
          break;
        }
    }
  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "\"%s\" holds no zip archive", path);
      return NULL;
    }

  const guint16 count = read16 (base + eocd + 10);
  const guint32 offset = read32 (base + eocd + 16);

  zip->entries = g_new0 (ZipEntry, count ? count : 1);
  gsize at = offset;
  for (guint16 i = 0; i < count; i++)
    {
      if (at + 46 > zip->length - zip->zip_offset ||
          read32 (base + at) != ZIP_CENTRAL_MAGIC)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "\"%s\" has a broken central directory", path);
          return NULL;
        }

      const guint16 method = read16 (base + at + 10);
      const guint32 crc = read32 (base + at + 16);
      const gsize packed = read32 (base + at + 20);
      const gsize size = read32 (base + at + 24);
      const guint16 name_length = read16 (base + at + 28);
      const guint16 extra_length = read16 (base + at + 30);
      const guint16 comment_length = read16 (base + at + 32);
      const guint32 local = read32 (base + at + 42);

      if (at + 46 + (gsize) name_length > zip->length - zip->zip_offset)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "\"%s\" has a broken central directory", path);
          return NULL;
        }

      char *name = g_strndup (base + at + 46, name_length);
      if (!name_is_safe (name))
        {
          /* A package whose entries would not be safe to write is not a
             package this component installs; say why rather than skip the
             one entry and unpack the rest. */
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "\"%s\" holds an unsafe entry name: %s", path, name);
          g_free (name);
          return NULL;
        }

      zip->entries[zip->n_entries].name = name;
      zip->entries[zip->n_entries].method = method;
      zip->entries[zip->n_entries].crc = crc;
      zip->entries[zip->n_entries].local_offset = local;
      zip->entries[zip->n_entries].packed_size = packed;
      zip->entries[zip->n_entries].size = size;
      zip->n_entries++;

      at += 46u + name_length + extra_length + comment_length;
    }

  return g_steal_pointer (&zip);
}

char **
chromium_zip_names (ChromiumZip *zip)
{
  g_return_val_if_fail (zip != NULL, NULL);

  char **names = g_new0 (char *, zip->n_entries + 1);
  for (gsize i = 0; i < zip->n_entries; i++)
    names[i] = g_strdup (zip->entries[i].name);
  return names;
}

static gboolean
inflate_raw (const char *packed, gsize packed_size, char *out, gsize size,
             GError **error)
{
  z_stream stream;
  memset (&stream, 0, sizeof (stream));

  /* Negative windows bits: zip's deflate has no zlib header of its own. */
  if (inflateInit2 (&stream, -MAX_WBITS) != Z_OK)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "could not start inflation");
      return FALSE;
    }

  stream.next_in = (Bytef *) packed;
  stream.avail_in = packed_size;
  stream.next_out = (Bytef *) out;
  stream.avail_out = size;

  const int result = inflate (&stream, Z_FINISH);
  inflateEnd (&stream);

  if (result != Z_STREAM_END || stream.avail_out != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "an entry did not inflate to its stated size");
      return FALSE;
    }
  return TRUE;
}

gboolean
chromium_zip_read (ChromiumZip *zip, const char *name, char **data,
                   gsize *length, GError **error)
{
  g_return_val_if_fail (zip != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (length != NULL, FALSE);

  *data = NULL;
  *length = 0;

  const ZipEntry *entry = NULL;
  for (gsize i = 0; i < zip->n_entries; i++)
    if (g_strcmp0 (zip->entries[i].name, name) == 0)
      {
        entry = &zip->entries[i];
        break;
      }
  if (!entry)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "the package holds no \"%s\"", name);
      return FALSE;
    }

  /* A directory entry states size 0 and carries no data worth reading. */
  if (entry->size == 0)
    return TRUE;

  const char *base = zip->data + zip->zip_offset;
  const gsize local = entry->local_offset;
  if (local + 30 > zip->length - zip->zip_offset ||
      read32 (base + local) != ZIP_LOCAL_MAGIC)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "the entry \"%s\" has a broken local header", name);
      return FALSE;
    }

  /* The local header's sizes are authoritative for streamed entries and are
     what the central directory should agree with; trust the central one, but
     bound the read by the file. */
  const guint16 name_length = read16 (base + local + 26);
  const guint16 extra_length = read16 (base + local + 28);
  const gsize data_at = local + 30u + name_length + extra_length;
  if (data_at + entry->packed_size > zip->length - zip->zip_offset)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "the entry \"%s\" runs past the end of the package", name);
      return FALSE;
    }

  char *out = g_malloc (entry->size);
  if (entry->method == 0)
    {
      if (entry->packed_size != entry->size)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "the stored entry \"%s\" disagrees with itself", name);
          g_free (out);
          return FALSE;
        }
      memcpy (out, base + data_at, entry->size);
    }
  else if (entry->method == 8)
    {
      if (!inflate_raw (base + data_at, entry->packed_size, out, entry->size,
                        error))
        {
          g_free (out);
          return FALSE;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "the entry \"%s\" uses compression method %u, which is "
                   "neither stored nor deflate", name, entry->method);
      g_free (out);
      return FALSE;
    }

  /* The CRC is the one integrity check the format itself offers, and a
     corrupted install is worse than a failed one. */
  const gulong crc = crc32 (0L, Z_NULL, 0);
  if ((guint32) crc32 (crc, (const Bytef *) out, entry->size) != entry->crc)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "the entry \"%s\" fails its checksum", name);
      g_free (out);
      return FALSE;
    }

  *data = out;
  *length = entry->size;
  return TRUE;
}
