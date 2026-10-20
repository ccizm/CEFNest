shm_client = executable(
  'shm-client',
  [ 'shm-client.c', xdg_shell_client_header, xdg_shell_code,
    dmabuf_client_header, dmabuf_code,
    viewporter_client_header, viewporter_code,
    fractional_scale_client_header, fractional_scale_code,
    text_input_client_header, text_input_code,
    cursor_shape_client_header, cursor_shape_code, tablet_code ],
  include_directories: gen_inc,
  dependencies: [ wl_client_dep, xkb_dep, gbm_dep, drm_dep ],
  install: false,
)

seat_selftest = executable(
  'seat-selftest',
  [ 'seat-selftest.c' ],
  dependencies: [ libnested_dep, gtk_dep ],
  c_args: [ '-DCLIENT_PATH="' + meson.current_build_dir() + '/shm-client"' ],
  install: false,
)

# Replays evdev events through /dev/uinput, so the widget -> nested seat path
# can be driven without a hand on the real devices. Needs write access to the
# node, which normally means running it through sudo.
uinput_inject = executable(
  'uinput-inject',
  [ 'uinput-inject.c' ],
  install: false,
)
