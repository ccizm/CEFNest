shm_client = executable(
  'shm-client',
  [ 'shm-client.c', xdg_shell_client_header, xdg_shell_code,
    dmabuf_client_header, dmabuf_code,
    viewporter_client_header, viewporter_code,
    fractional_scale_client_header, fractional_scale_code,
    primary_selection_client_header, primary_selection_code,
    text_input_client_header, text_input_code,
    cursor_shape_client_header, cursor_shape_code, tablet_code ],
  include_directories: gen_inc,
  dependencies: [ wl_client_dep, xkb_dep, gbm_dep, drm_dep ],
  # The demo falls back to this client when it has no helper to start, so an
  # installed demo needs it; the build tree copy is what every run uses in place.
  install: get_option('install-demo'),
  install_dir: get_option('prefix') / get_option('libexecdir') / 'cefnest',
)

seat_selftest = executable(
  'seat-selftest',
  [ 'seat-selftest.c' ],
  dependencies: [ libcefnest_dep, gtk_dep ],
  c_args: [ '-DCLIENT_PATH="' + meson.current_build_dir() + '/shm-client"' ],
  install: false,
)

# The helper is a separate binary, so finding it is a rule rather than a
# default; this checks the rule against the same two paths the library was
# configured with, with no display and no Chromium in the loop.
helper_path = executable(
  'helper-path',
  [ 'helper-path.c' ],
  c_args: [
    '-DHELPER_PATH="' + helper_default_path + '"',
    '-DHELPER_DEVELOPMENT="' + helper_build_path + '"',
  ],
  dependencies: [ libcefnest_dep, gtk_dep, json_dep ],
  install: false,
)

test('helper-path', helper_path, suite: 'unit', timeout: 30)

# Replays evdev events through /dev/uinput, so the widget -> nested seat path
# can be driven without a hand on the real devices. Needs write access to the
# node, which normally means running it through sudo.
uinput_inject = executable(
  'uinput-inject',
  [ 'uinput-inject.c' ],
  install: false,
)

python = find_program('python3', 'python', required: false)

if python.found()
  # The static half of the empty-slot check: every request slot a client can
  # reach at the versions we advertise has to be filled, or the compositor
  # aborts. Needs the generated server headers, which the root meson.build puts
  # at the top of the build directory, so name that and not this subdir.
  test('impl-slot-audit', python,
    args: [ files('audit-impl-slots.py'), meson.project_build_root() ],
    depends: [ xdg_shell_server_header, dmabuf_server_header,
               viewporter_server_header, fractional_scale_server_header,
               text_input_server_header, cursor_shape_server_header ],
    suite: 'protocol',
    timeout: 60)

  # The other half of "a source that holds no reference": every g_idle_add /
  # g_timeout_add / g_source_attach in the library has to be either handed a
  # reference of its own or cancelled by the dispose of the object it points
  # into, or the main loop runs the callback over freed memory. Static, so it
  # costs nothing and needs no display.
  test('loop-source-audit', python,
    args: [ files('audit-loop-sources.py') ],
    suite: 'protocol',
    timeout: 60)
endif

# One run per thing the client asks the compositor for. The arguments after the
# client path go to the client, so each mode binds a different set of protocol
# objects; every one of them has to finish the scripted sequence and hand its
# file descriptors back. A machine with no display gets a skip, not a failure —
# see the gtk_init_check() guard in seat-selftest.c.
#
# gtk-client is the odd one: the shm client cannot crash on a legal event
# sequence because it only ever sets flags, so that step brings in a second
# copy of the test, built against GTK, as a client of its own compositor.
foreach mode : [ 'plain', 'dmabuf', 'text-input', 'cursor-shape', 'popup',
                 'refused', 'pool-budget', 'wrong-object', 'pool-shrink',
                 'empty-copy', 'popup-depth', 'drag-fake-serial',
                 'dnd-requests', 'drag-out', 'drag-in', 'fullscreen',
                 'second-window', 'gtk-client' ]
  selftest_args = [ 'seat-selftest-' + mode + '.png', shm_client.full_path() ]
  if mode != 'plain'
    selftest_args += [ '--' + mode ]
  endif

  test('seat-selftest-' + mode, seat_selftest,
    args: selftest_args,
    depends: shm_client,
    suite: 'selftest',
    timeout: 120)
endforeach
