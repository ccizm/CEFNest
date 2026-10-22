#!/usr/bin/env bash
#
# Locate the built CEF helper the way the gate scripts need it.
#
# The helper is built by cmake, not meson, and its output directory depends on
# the generator: a multi-config generator writes build/Release/cefnest-helper,
# a single-config one writes build/cefnest-helper. Every gate used to hardcode
# the first shape, so a Debug or single-config build tree looked like "no
# helper" even when it was there.
#
# CHROMIUM_HELPER still wins; a caller that names a binary means it. Otherwise
# the first executable below is used. The last fallback keeps the old error
# message in the gate ("no helper at ...") when nothing was built at all.
#
# Expected to be sourced after the caller has set ROOT.

if [ -z "${CHROMIUM_HELPER:-}" ]; then
  for candidate in "$ROOT/helper/build/Release/cefnest-helper" \
                   "$ROOT/helper/build/cefnest-helper" \
                   "$ROOT/helper/build/Debug/cefnest-helper"; do
    if [ -x "$candidate" ]; then
      CHROMIUM_HELPER=$candidate
      break
    fi
  done
fi

HELPER=${CHROMIUM_HELPER:-"$ROOT/helper/build/Release/cefnest-helper"}
