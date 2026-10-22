#!/usr/bin/env python3
"""Audit: does anything the widget arms on every map have a way to be un-armed?

GTK keeps a separate marshalling slot for every g_signal_connect() and a separate
controller for every gtk_widget_add_controller(). Both are cheap when they are
armed once, with the widget; armed from a `map` handler they are a pile-up, because
a window is hidden and shown again — minimised, sent to another workspace, closed
into the session's tray — and each cycle adds one more. Then one right click
answers five menus, one dropped file starts five drags, and nothing says so: the
first handler wins the race and the rest fire behind it, each on a widget that is
already done with that signal.

The honest shapes are two: arm it with the widget, in init/construct, where it
happens once by construction; or arm it per cycle and take it down again in the
matching unmap/dispose, by a stored handler id. This reports an arm that is
neither — from inside a `*_on_map` body (following one level of same-file helper
calls, because `chromium_view_watch_drops()` is where the controller actually
goes in), an arm whose result is thrown away, or stored in a field no code in that
file ever disconnects.

Nothing is exempt by its target: a slot on the widget that is being mapped is one
more slot per cycle just like one on anything else, and a controller added to that
widget on every map is the same pile-up. The only thing that settles it is whether
the code can take the arm down again.

Run with no arguments from anywhere in the checkout: the sources are found
relative to this file.
"""
import glob
import os
import re
import sys

root = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))

ARM = re.compile (r'\b(g_signal_connect\w*|gtk_widget_add_controller)\s*\(')
# C puts the assignment and the call on separate lines as often as not, so ask
# what is directly above the arm instead of hoping both fit on one line.
STORED_ABOVE = re.compile (r'self->(\w+)\s*=\s*$')
DISCONNECT = re.compile (r'g_signal_handler_disconnect\s*\([^,]+,\s*self->(\w+)\s*\)|'
                         r'g_clear_handle_id\s*\(\s*&self->(\w+)|'
                         r'gtk_widget_remove_controller\s*\([^,]+,\s*self->(\w+)')
MAP_BODY = re.compile (r'^\w+_on_map\s*\([^)]*\)', re.M)
FUNC_HEADER = re.compile (r'^(\w+)\s*\([^)]*\)\s*$', re.M)


def body_at (text, start):
    """The brace-matched body that follows the header ending at `start`.

    Returns (body, offset) where offset is where `body` begins in `text`, so a
    report can name the line the arm is on.
    """
    open_brace = text.find ('{', start)
    if open_brace < 0:
        return None
    depth = 0
    for index in range (open_brace, len (text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:index], open_brace + 1
    return None


def bodies (text, header):
    """Every (body, offset) that follows a header matched by `header` in `text`."""
    found = []
    for match in header.finditer (text):
        found_body = body_at (text, match.end ())
        if found_body is not None:
            found.append (found_body)
    return found


def field_stored_above (body, arm_start):
    """The self->field this arm's result goes into, or None when it is discarded.

    Comments between the `=` and the call are the only thing allowed in between,
    so an arm behind an unrelated statement is not counted as stored.
    """
    tail = body[:arm_start].rstrip ()
    while tail.endswith ('*/'):
        head = tail.rfind ('/*')
        if head < 0:
            return None
        tail = tail[:head].rstrip ()
    match = STORED_ABOVE.search (tail)
    return match.group (1) if match else None


def arms_unremovably (body, offset, text, released):
    """Every arm in `body` that cannot be taken down again.

    `released` is the set of self->field names this file disconnects.
    """
    problems = []

    for match in ARM.finditer (body):
        field = field_stored_above (body, match.start ())
        line = text.count ('\n', 0, offset + match.start ()) + 1
        if field is not None:
            if field not in released:
                problems.append ((line, match.group (1),
                                  'stored in self->%s, which nothing disconnects' % field))
            continue

        problems.append ((line, match.group (1),
                          'the id is thrown away, so it cannot be taken down'))

    return problems


def body_names (body):
    return set (re.findall (r'\b(\w+)\s*\(', body))


def main ():
    failures = []
    handlers = 0

    for path in sorted (glob.glob (os.path.join (root, 'src', '*.c'))):
        with open (path, encoding = 'utf-8') as handle:
            text = handle.read ()

        released = set ()
        for match in DISCONNECT.finditer (text):
            released.update (group for group in match.groups () if group)

        functions = {}
        for match in FUNC_HEADER.finditer (text):
            name = match.group (1)
            found_body = body_at (text, match.end ())
            if found_body is not None:
                functions[name] = found_body

        for body, offset in bodies (text, MAP_BODY):
            handlers += 1
            problems = arms_unremovably (body, offset, text, released)
            for callee in sorted (body_names (body) & set (functions)):
                callee_body, callee_offset = functions[callee]
                problems += arms_unremovably (callee_body, callee_offset, text, released)
            for line, arm, why in problems:
                failures.append ('%s:%d: %s from a map handler, and %s' %
                                 (os.path.relpath (path, root), line, arm, why))

    if not handlers:
        print ('no map handlers found in src/*.c: MAP_BODY no longer matches the '
               'source, so this audit would be grading nothing')
        return 1

    if failures:
        print ('map-handler arms that cannot be taken down:')
        for line in sorted (set (failures)):
            print ('  ' + line)
        print ('\nArm them with the widget (init), or store the id and disconnect '
               'it in the matching unmap/dispose.')
        return 1

    print ('ok: every arm made from a map handler can be taken down again')
    return 0


if __name__ == '__main__':
    sys.exit (main ())
