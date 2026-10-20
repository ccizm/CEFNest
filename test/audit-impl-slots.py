#!/usr/bin/env python3
"""Audit: which protocol request slots does the nested compositor leave NULL?

A wl_resource implementation struct is indexed by the wire opcode, so a NULL
slot is not a refused request: libwayland-server aborts the whole compositor
("listener function for opcode N of IFACE is NULL") the moment a client sends
that request. Parse every "struct X_interface" from the installed and the
generated server headers, then every initializer in src/, and report the holes.
"""
import re
import glob
import os
import sys

headers = ['/usr/include/wayland-server-protocol.h']
headers += glob.glob('builddir/*-server-protocol.h')

members = {}
for path in headers:
    if not os.path.exists(path):
        continue
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

rows = []
for path in sorted(glob.glob('src/*.c')):
    text = open(path).read()
    for m in re.finditer(r'static const struct (\w+)_interface (\w+) = \{(.*?)\n\};', text, re.S):
        iface, var, body = m.group(1), m.group(2), m.group(3)
        provided = set(re.findall(r'\.(\w+)\s*=', body))
        positional = re.findall(r'^\s*(?!/\*|\.)(\w+),', body, re.M)
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
sys.exit (1 if missing else 0)
