src_inc = include_directories('.')

# The component itself: the compositor, the widget and the control channel. It
# is one library because a host that wants a page in a window has to link all
# three, and the widget hands its compositor to nobody else.
libcefnest = shared_library(
  'cefnest',
  [ 'nested.c', 'nested-dmabuf.c', 'nested-seat.c', 'nested-data-device.c',
    'nested-xdg.c', 'nested-text-input.c', 'nested-cursor.c',
    'chromium-view.c', 'chromium-session.c', 'source-window.c',
    xdg_shell_server_header, xdg_shell_code,
    dmabuf_server_header, dmabuf_code,
    viewporter_server_header, viewporter_code,
    fractional_scale_server_header, fractional_scale_code,
    text_input_server_header, text_input_code,
    primary_selection_server_header, primary_selection_code,
    cursor_shape_server_header, cursor_shape_code, tablet_code ],
  c_args: [
    '-DHELPER_PATH="' + helper_default_path + '"',
    '-DHELPER_DEVELOPMENT="' + helper_build_path + '"',
  ],
  include_directories: gen_inc,
  dependencies: [ gtk_dep, gsv_dep, json_dep, wl_server_dep, xkb_dep, gbm_dep, drm_dep,
                  threads_dep, m_dep ],
  version: get_option('cefnest-version'),
  soversion: '0',
  install: true,
)

libcefnest_dep = declare_dependency(
  link_with: libcefnest,
  include_directories: src_inc,
  dependencies: [ gtk_dep, gsv_dep, wl_server_dep, xkb_dep, gbm_dep, drm_dep, m_dep ],
)

# The compositor's own internals stay out of the installed tree: nested-internal.h
# is what the compositor modules share with each other, and the generated protocol
# headers name interfaces a host has no reason to implement.
install_headers(
  'nested.h', 'chromium-view.h', 'chromium-session.h',
  subdir: 'cefnest',
)

pkgconfig.generate(
  libcefnest,
  version: get_option('cefnest-version'),
  name: 'cefnest',
  description: 'A GTK 4 widget that shows a page rendered by real Chromium, composited through a Wayland compositor running in the same process',
  subdirs: 'cefnest',
  # A host that includes the widget's header includes gtk and wayland-server-core
  # with it, so those are its compile flags as much as they are ours. json-glib is
  # only used inside the library, and it is still listed: the .so links against it,
  # and a package that resolves a dependency by pkg-config should be able to find
  # the same one we were built with.
  requires: [ gtk_dep, gsv_dep, json_dep, wl_server_dep, xkb_dep, gbm_dep, drm_dep ],
  # Includedir itself as well as the subdirectory: a host writes
  # <cefnest/chromium-session.h>, and a bare "nested.h" is a name too easy to
  # find in somebody else's tree to be the only way to reach this one.
  extra_cflags: [ '-I${includedir}' ],
)

# The helper is not built here — cmake builds it — so this is only a copy of
# what cmake left behind, runtime files and all. They have to arrive next to the
# binary: it finds libcef.so through $ORIGIN and nothing else.
if get_option('install-helper')
  # install_subdir() of a directory that does not exist is not an error: meson
  # prints a confident "Installing subdir" line, creates the empty destination,
  # and exits 0. A package that installs no helper is the one failure this option
  # exists to avoid, so ask for the binary rather than for the directory — cmake
  # may have been pointed somewhere else entirely.
  if not fs.is_file(helper_build_path)
    error('install-helper is on but ' + helper_build_path + ' does not exist. ' +
          'Build the helper with cmake first, or point helper-build-dir at the ' +
          'cmake output directory that holds it.')
  endif
  install_subdir(
    helper_build_dir,
    install_dir: helper_install_dir,
    strip_directory: true,
  )
endif

chromium_demo = executable(
  'cefnest-demo',
  [ 'main.c' ],
  c_args: [
    '-DCLIENT_PATH="' + client_install_path + '"',
    '-DCLIENT_DEVELOPMENT="' + meson.project_build_root() + '/test/shm-client"',
  ],
  # $ORIGIN-relative rather than the absolute libdir: a prefix copied somewhere
  # else — which is what a package build does with DESTDIR — then still starts.
  install_rpath: '$ORIGIN/' + fs.relative_to (
                   get_option('prefix') / get_option('libdir'),
                   get_option('prefix') / get_option('bindir')),
  dependencies: [ libcefnest_dep, gtk_dep, json_dep ],
  install: get_option('install-demo'),
)
