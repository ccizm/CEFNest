#!/usr/bin/env python3
"""Audit: which protocol request slots does the nested compositor leave NULL?

A wl_resource implementation struct is indexed by the wire opcode, so a NULL
slot is not a refused request: libwayland-server aborts the whole compositor
("listener function for opcode N of IFACE is NULL") the moment a client sends
that request. Parse every "struct X_interface" from the installed and the
generated server headers, then every initializer in src/, and report the holes.
A slot that is spelled out and set to NULL counts as a hole, in either
spelling — `.destroy = NULL` and a positional NULL both leave the array entry
empty, and the abort cannot tell how the source wrote it.

Run with no arguments from anywhere in the checkout: the sources are found
relative to this file, and the generated headers relative to ./builddir. Pass a
build directory as the first argument when meson put it somewhere else — that
is what `meson test` does.
"""
import re
import glob
import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'builddir')


def core_header ():
    """The installed wayland-server-protocol.h, wherever this distro put it."""
    try:
        out = subprocess.run (['pkg-config', '--cflags', 'wayland-server'],
                              capture_output = True, text = True, check = True).stdout
    except Exception:
        out = ''

    dirs = [f[2:] for f in out.split () if f.startswith ('-I')] + ['/usr/include']
    for d in dirs:
        path = os.path.join (d, 'wayland-server-protocol.h')
        if os.path.exists (path):
            return path
    return None


headers = []
core = core_header ()
if core:
    headers.append (core)
headers += glob.glob (os.path.join (build, '*-server-protocol.h'))

members = {}
for path in headers:
    src = open(path).read()
    for m in re.finditer(r'struct (\w+)_interface \{(.*?)\n\};', src, re.S):
        iface, body = m.group(1), m.group(2)
        names = re.findall(r'\b(?:void|int)\s+\(\*(\w+)\)\s*\(', body)
        table = []
        for n in names:
            sm = re.search(r'#define %s_%s_SINCE_VERSION (\d)' %
                           (iface.upper(), n.upper()), src)
            table.append((n, int(sm.group(1)) if sm else 1))
        members.setdefault(iface, table)

if 'wl_data_source' not in members:
    print('the installed wayland server header was not found, so nothing was '
          'audited (looked for wayland-server-protocol.h and %s)' % build)
    sys.exit(2)

rows = []
for path in sorted(glob.glob(os.path.join(root, 'src', '*.c'))):
    text = open(path).read()
    for m in re.finditer(r'static const struct (\w+)_interface (\w+) = \{(.*?)\n\};', text, re.S):
        iface, var, body = m.group(1), m.group(2), m.group(3)
        # A slot written out but set to NULL is an empty slot: libwayland-server
        # indexes the table by opcode and does not care how the source spelled
        # the hole, so `.destroy = NULL` aborts exactly like a missing entry.
        # Counting either spelling as coverage made this audit pass a tree that
        # had deliberately emptied a slot — see docs/internals/empty-request-slot.md.
        provided = set(re.findall(r'\.(\w+)\s*=\s*(?!NULL\b|0\b)\S', body))
        positional = re.findall(r'^\s*(?!/\*|\.|NULL\b|0\b)(\w+),', body, re.M)
        rows.append((path, iface, var, provided, positional))

missing = 0
unknown = 0
for path, iface, var, provided, positional in rows:
    table = members.get(iface)
    if table is None:
        print('  ? %-24s %-30s interface not found in any header' % (path, var))
        unknown += 1
        continue
    for i, (name, since) in enumerate(table):
        covered = name in provided or (i < len(positional))
        if not covered:
            print('MISSING  %-24s %-30s slot %d = %-22s since %d' %
                  (path, var, i, name, since))
            missing += 1

print('\n%d uncovered slot(s), %d unknown interface(s), %d implementation struct(s)'
      % (missing, unknown, len(rows)))
# An interface that is in no header is not a pass: it is a struct that was never
# looked at, which is exactly what a missing generated header produces.
sys.exit (1 if (missing or unknown) else 0)
