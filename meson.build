project(
  'cefnest',
  'c',
  version: '0.0.1',
  license: 'LGPL-2.1-only',
  meson_version: '>= 1.3.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

gtk_dep = dependency('gtk4', version: '>=4.14')
json_dep = dependency('json-glib-1.0')
wl_server_dep = dependency('wayland-server')
wl_client_dep = dependency('wayland-client')
xkb_dep = dependency('xkbcommon')
gbm_dep = dependency('gbm')
drm_dep = dependency('libdrm')
threads_dep = dependency('threads')
pkgconfig = import('pkgconfig')
fs = import('fs')
# math.h is not part of glibc's default link set, and rounding a wheel detent
# into 120ths is the only thing that needs it.
m_dep = meson.get_compiler('c').find_library('m', required: false)

wayland_scanner = find_program('wayland-scanner')
add_project_arguments ('-Wno-unused-parameter', language: 'c')
# The staging XML below (fractional-scale, cursor-shape) is 1.31/1.32 and up.
# Ask for the version, so an older distribution fails here with a message about
# wayland-protocols instead of a wayland-scanner error about a missing file.
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.32')
protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

# The helper is a CEF application, so it drags the whole Chromium runtime with
# it, and that runtime is architecture-specific. Installing it under a directory
# named after the architecture is what lets two builds share one prefix instead
# of overwriting each other's libcef.so; the component then asks for the one its
# own host machine names, which is the same value the install rule below uses.
# get_option() hands these directories back relative to the prefix, which is
# what an install rule wants and what a path baked into a binary is not: the
# component would otherwise go looking for its helper under whatever directory
# the browser happened to be started from.
helper_arch = host_machine.cpu_family()
helper_install_dir = get_option('prefix') / get_option('libexecdir') / 'cefnest' / helper_arch
if get_option('helper-path') == ''
  helper_default_path = helper_install_dir / 'cefnest-helper'
else
  helper_default_path = get_option('helper-path')
endif
# What cmake leaves behind in the source tree: the built helper and every runtime
# file it needs, already next to each other and already found through $ORIGIN.
helper_build_dir = meson.current_source_dir() / get_option('helper-build-dir')
helper_build_path = helper_build_dir / 'cefnest-helper'
# The demo's other client, the one it drives when there is no Chromium to start.
# It is not architecture-specific, so it sits in libexecdir itself rather than in
# the per-architecture directory next door.
client_install_path = get_option('prefix') / get_option('libexecdir') / 'cefnest' / 'shm-client'

gen_inc = include_directories('.')

xdg_shell_server_header = custom_target(
  'xdg-shell-server-header',
  input: xdg_shell_xml,
  output: 'xdg-shell-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_header = custom_target(
  'xdg-shell-client-header',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_code = custom_target(
  'xdg-shell-code',
  input: xdg_shell_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

linux_dmabuf_xml = protocols_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml'

dmabuf_server_header = custom_target(
  'linux-dmabuf-server-header',
  input: linux_dmabuf_xml,
  output: 'linux-dmabuf-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

dmabuf_client_header = custom_target(
  'linux-dmabuf-client-header',
  input: linux_dmabuf_xml,
  output: 'linux-dmabuf-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

dmabuf_code = custom_target(
  'linux-dmabuf-code',
  input: linux_dmabuf_xml,
  output: 'linux-dmabuf-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

viewporter_xml = protocols_dir / 'stable/viewporter/viewporter.xml'

viewporter_server_header = custom_target(
  'viewporter-server-header',
  input: viewporter_xml,
  output: 'viewporter-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

viewporter_client_header = custom_target(
  'viewporter-client-header',
  input: viewporter_xml,
  output: 'viewporter-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

viewporter_code = custom_target(
  'viewporter-code',
  input: viewporter_xml,
  output: 'viewporter-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

fractional_scale_xml = protocols_dir / 'staging/fractional-scale/fractional-scale-v1.xml'

fractional_scale_server_header = custom_target(
  'fractional-scale-server-header',
  input: fractional_scale_xml,
  output: 'fractional-scale-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

fractional_scale_client_header = custom_target(
  'fractional-scale-client-header',
  input: fractional_scale_xml,
  output: 'fractional-scale-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

fractional_scale_code = custom_target(
  'fractional-scale-code',
  input: fractional_scale_xml,
  output: 'fractional-scale-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

primary_selection_xml = protocols_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml'

primary_selection_server_header = custom_target(
  'primary-selection-server-header',
  input: primary_selection_xml,
  output: 'primary-selection-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

primary_selection_client_header = custom_target(
  'primary-selection-client-header',
  input: primary_selection_xml,
  output: 'primary-selection-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

primary_selection_code = custom_target(
  'primary-selection-code',
  input: primary_selection_xml,
  output: 'primary-selection-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

text_input_xml = protocols_dir / 'unstable/text-input/text-input-unstable-v3.xml'

text_input_server_header = custom_target(
  'text-input-server-header',
  input: text_input_xml,
  output: 'text-input-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

text_input_client_header = custom_target(
  'text-input-client-header',
  input: text_input_xml,
  output: 'text-input-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

text_input_code = custom_target(
  'text-input-code',
  input: text_input_xml,
  output: 'text-input-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

cursor_shape_xml = protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml'

cursor_shape_server_header = custom_target(
  'cursor-shape-server-header',
  input: cursor_shape_xml,
  output: 'cursor-shape-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

cursor_shape_client_header = custom_target(
  'cursor-shape-client-header',
  input: cursor_shape_xml,
  output: 'cursor-shape-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

cursor_shape_code = custom_target(
  'cursor-shape-code',
  input: cursor_shape_xml,
  output: 'cursor-shape-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# cursor-shape-v1 version 2 asks for a device for a zwp_tablet_tool_v2, and the
# generated messages table names that interface even though the compositor has
# no tablet. Only its descriptor is needed, so the tablet code is linked for
# that and nothing implements it.
tablet_xml = protocols_dir / 'stable/tablet/tablet-v2.xml'

tablet_code = custom_target(
  'tablet-code',
  input: tablet_xml,
  output: 'tablet-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

subdir('src')
subdir('examples')
subdir('test')
