project(
  'chromium-gtk',
  'c',
  version: '0.0.1',
  license: 'LGPL-2.1-only',
  meson_version: '>=1.0',
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
# math.h is not part of glibc's default link set, and rounding a wheel detent
# into 120ths is the only thing that needs it.
m_dep = meson.get_compiler('c').find_library('m', required: false)

wayland_scanner = find_program('wayland-scanner')
add_project_arguments ('-Wno-unused-parameter', language: 'c')
protocols_dir = run_command('pkg-config', '--variable=pkgdatadir', 'wayland-protocols', check: true).stdout().strip()
xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

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
subdir('test')
