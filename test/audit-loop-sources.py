#!/usr/bin/env python3
"""Audit: does every loop source the library arms get cancelled on the way out?

g_idle_add(), g_timeout_add() and g_source_attach() do not take a reference:
they keep the pointer they were handed. If that pointer is the object being
built, nothing keeps it alive — dispose runs, the memory goes, and the main loop
calls the callback with the dead pointer a moment later. There are two honest
answers to that: hand the source a reference of its own, or cancel the source in
the dispose of the object it points into. The compositor does the second for its
frame timer and its two replay idles, and the second is the one that is easy to
forget — nested-data-device.c arms a primary replay idle in exactly the shape
nested.c already cancels the selection one in.

So: collect every g_source_remove (self->field) and g_clear_handle_id
(&self->field) inside any *_dispose body in src/, then report every
`self->field = g_idle_add / g_timeout_add / g_source_attach` that has neither a
cancel of that name nor a g_object_ref() in its own argument list.

Field names are per-object (`selection_replay_id`, `frame_timer_id`), so a match
by name across files is the same match by object; a field that two objects shared
by name would be the one way this could pass while it should not.

Run with no arguments from anywhere in the checkout: the sources are found
relative to this file.
"""
import glob
import os
import re
import sys

root = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))

ARM = re.compile (r'self->(\w+)\s*=\s*\n?\s*'
                  r'(g_idle_add|g_timeout_add\w*|g_source_attach)\s*\(')
REF = re.compile (r'g_object_ref\s*\(')
CANCEL = re.compile (r'g_source_remove\s*\(\s*self->(\w+)\s*\)|'
                     r'g_clear_handle_id\s*\(\s*&self->(\w+)')
DISPOSE = re.compile (r'^\w+_dispose\s*\(GObject \*object\)', re.M)


def bodies (text, header):
    """Every brace-matched body that follows a function header in `text`."""
    for match in header.finditer (text):
        if '{' not in text[match.end ():]:
            continue
        at = text.index ('{', match.end ())
        depth = 0
        for i in range (at, len (text)):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    yield text[at:i + 1]
                    break


cancelled = set ()
sources = {}

for path in sorted (glob.glob (os.path.join (root, 'src', '*.c'))):
    text = open (path).read ()
    sources[path] = text
    for body in bodies (text, DISPOSE):
        for match in CANCEL.finditer (body):
            cancelled.add (match.group (1) or match.group (2))

missing = 0
covered = 0
checked = 0

for path in sorted (sources):
    text = sources[path]

    for match in ARM.finditer (text):
        field, arms, at = match.group (1), match.group (2), match.end ()
        end = text.find (';', at)
        tail = text[at:end if end >= 0 else at + 400]
        checked += 1

        if REF.search (tail) or field in cancelled:
            covered += 1
            continue

        missing += 1
        print ('MISSING  %s:%d  %s = %s (...) is armed and never cancelled by '
               'any dispose' % (os.path.relpath (path, root),
                                 text[:at].count ('\n') + 1, field, arms))

print ('\n%d source(s) armed, %d covered, %d never cancelled' %
       (checked, covered, missing))
sys.exit (1 if missing else 0)
